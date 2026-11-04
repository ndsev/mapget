/** Construction-only compatibility and namespace translation. This implementation
 * is included after MapSchema::Impl so none of its scratch state is public ABI. */
class MapSchema::Impl::CompositionBuilder
{
    using LogicalSchema = schema_detail::SchemaBuilder::LogicalSchema;
    Impl& target_;
    std::span<ConstPtr const> inputs_;
    std::vector<schema_detail::SchemaBuilder> graphs_;
    std::vector<size_t> offsets_;
    std::vector<std::pair<size_t, TypeId>> origins_{{0, 0}};
    std::vector<size_t> parents_{0};
    std::vector<std::vector<size_t>> members_{{}};
    std::vector<TypeId> mapped_{0};
    std::vector<std::tuple<size_t, size_t, std::string>> pending_;
    std::map<std::string, std::vector<std::pair<size_t, size_t>>, std::less<>> layers_;
    size_t work_ = 0;

    /** Qualify a local identity only in construction scratch space. */
    size_t slot(size_t source, TypeId id) const
    {
        if (!id)
            return 0;
        if (id >= graphs_[source].schemas_.size())
            throw std::invalid_argument("Dangling constituent schema reference.");
        return offsets_[source] + id - 1;
    }
    /** Read the native declaration without a JSON graph projection. */
    LogicalSchema const& node(size_t index) const
    {
        auto [source, id] = origins_[index];
        return graphs_[source].schemas_[id];
    }
    /** Compress equivalence paths; frozen artifacts never retain this union-find. */
    size_t representative(size_t index)
    {
        while (parents_[index] != index) {
            parents_[index] = parents_[parents_[index]];
            index = parents_[index];
        }
        return index;
    }
    /** Attribute diagnostics to the actual shared declaration or field. */
    [[noreturn]] static void conflict(std::string const& path, std::string_view reason)
    {
        throw std::invalid_argument(
            "Incompatible schema contribution at " + path + ": " + std::string(reason));
    }
    /** Queue correspondence without recursively following a possibly cyclic graph. */
    void pair(size_t left, size_t right, std::string path)
    {
        if (!left || !right) {
            if (left != right)
                conflict(path, "known and unknown types differ");
            return;
        }
        if (++work_ > 64 * 1024 * 1024)
            throw std::length_error("Schema composition work budget exceeded.");
        pending_.emplace_back(left, right, std::move(path));
    }
    /** Pair declared alternatives in order; changing their meaning is not an additive field. */
    void edges(
        size_t left,
        size_t right,
        std::span<TypeId const> a,
        std::span<TypeId const> b,
        std::string const& path)
    {
        if (a.size() != b.size())
            conflict(path, "alternative cardinality differs");
        for (size_t i = 0; i < a.size(); ++i)
            pair(slot(origins_[left].first, a[i]), slot(origins_[right].first, b[i]), path);
    }
    /** Decode references already validated by the native/imported artifact. */
    static TypeId reference(nlohmann::json const& value)
    {
        auto text = value.at("$ref").get<std::string>();
        constexpr std::string_view prefix = "#/definitions/";
        uint32_t id = 0;
        if (!text.starts_with(prefix))
            throw std::invalid_argument("Unresolved validation reference.");
        auto digits = std::string_view(text).substr(prefix.size());
        auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (error != std::errc{} || end != digits.data() + digits.size() || !id ||
            id > simfil::MaxTypeId)
            throw std::invalid_argument("Invalid validation type reference.");
        return TypeId(id);
    }
    /** Merge descriptive maps, rejecting contradictory values instead of first-wins. */
    static void mergeJson(nlohmann::json& out, nlohmann::json const& input, std::string const& path)
    {
        if (out.is_object() && input.is_object()) {
            for (auto const& [key, value] : input.items()) {
                auto found = out.find(key);
                if (found == out.end())
                    out[key] = value;
                else
                    mergeJson(*found, value, path + "/" + key);
            }
        }
        else if (out != input)
            conflict(path, "annotation meaning differs");
    }
    /** Union forward contracts while retaining unknown versus explicitly empty coverage. */
    static nlohmann::json
    mergeAnnotations(nlohmann::json a, nlohmann::json b, std::string const& path)
    {
        if (a.is_null())
            a = nlohmann::json::object();
        if (b.is_null())
            b = nlohmann::json::object();
        auto declarations = nlohmann::json::array();
        bool any = false, known = true;
        std::optional<bool> produced;
        for (auto* value : {&a, &b}) {
            auto metadata = value->find("x-mapget");
            if (metadata != value->end() && metadata->contains("produced")) {
                produced = produced.value_or(false) || metadata->at("produced").get<bool>();
                metadata->erase("produced");
            }
            bool present = metadata != value->end() && metadata->contains("relationDeclarations");
            known = known && present && metadata->value("relationDeclarationsKnown", true);
            if (!present)
                continue;
            any = true;
            for (auto relation : metadata->at("relationDeclarations")) {
                auto& targets = relation.at("targetTypes");
                std::sort(targets.begin(), targets.end());
                auto old = std::ranges::find_if(
                    declarations,
                    [&](auto const& candidate)
                    {
                        return candidate.at("ownerType") == relation.at("ownerType") &&
                            candidate.at("name") == relation.at("name");
                    });
                if (old == declarations.end())
                    declarations.push_back(std::move(relation));
                else if (*old != relation)
                    conflict(path, "forward relation contract differs");
            }
            metadata->erase("relationDeclarations");
            metadata->erase("relationDeclarationsKnown");
            if (metadata->empty())
                value->erase(metadata);
        }
        mergeJson(a, b, path);
        if (produced)
            a["x-mapget"]["produced"] = *produced;
        if (any) {
            a["x-mapget"]["relationDeclarations"] = std::move(declarations);
            if (!known)
                a["x-mapget"]["relationDeclarationsKnown"] = false;
        }
        return a;
    }
    /** Check shared meaning and discover child correspondences. New object fields are additive. */
    void compare(size_t left, size_t right, std::string const& path)
    {
        auto const& a = node(left);
        auto const& b = node(right);
        if (a.queryRoot_ || b.queryRoot_)
            conflict(path, "generated query contexts are not producer declarations");
        if (a.kind_ != b.kind_ || a.composition_ != b.composition_ || a.nullable_ != b.nullable_)
            conflict(path, "kind, composition or nullability differs");
        auto text = [&](auto const& x, auto const& y)
        {
            if (!x.empty() && !y.empty() && x != y)
                conflict(path, "declared type meaning differs");
        };
        text(a.typeName_, b.typeName_);
        text(a.zserioType_, b.zserioType_);
        text(a.entry_.metaType_, b.entry_.metaType_);
        text(a.attributeType_, b.attributeType_);
        text(a.attributeTypeCode_, b.attributeTypeCode_);
        if (a.directEnumSymbols_ != b.directEnumSymbols_ || a.enumValues_ != b.enumValues_)
            conflict(path, "enum contract differs");
        edges(left, right, a.elementSchemas_, b.elementSchemas_, path + "/items");
        edges(left, right, a.alternatives_, b.alternatives_, path + "/alternatives");
        for (auto const& field : a.directFields_) {
            if (!b.childSchemas_.contains(field))
                continue;
            auto fieldPath = path + "/" + field;
            auto ar = a.required_.find(field), br = b.required_.find(field);
            if ((ar == a.required_.end()) != (br == b.required_.end()) ||
                (ar != a.required_.end() && ar->second != br->second) ||
                a.multimapFields_.contains(field) != b.multimapFields_.contains(field) ||
                a.contextFields_.contains(field) != b.contextFields_.contains(field))
                conflict(fieldPath, "presence, multiplicity or enumeration role differs");
            auto ac = a.childSchemas_.find(field), bc = b.childSchemas_.find(field);
            edges(
                left,
                right,
                ac == a.childSchemas_.end() ? std::span<TypeId const>{} : ac->second,
                bc == b.childSchemas_.end() ? std::span<TypeId const>{} : bc->second,
                fieldPath);
        }
        // Validation-only references are still graph edges. Compare their layout
        // without confusing independently allocated numeric identities.
        auto aa = a.annotations_, ba = b.annotations_;
        std::map<std::string, TypeId> ar, br;
        auto normalize = [&](auto& json, auto& refs)
        {
            schema_detail::SchemaBuilder::validationSchemas(
                json,
                [&](auto& value, auto const& location)
                {
                    refs.emplace(location, reference(value));
                    value["$ref"] = "#";
                });
        };
        normalize(aa, ar);
        normalize(ba, br);
        for (auto const& [location, id] : ar)
            if (auto other = br.find(location); other != br.end())
                pair(
                    slot(origins_[left].first, id),
                    slot(origins_[right].first, other->second),
                    path + location);
        if (!hasAffinity(a.kind_, simfil::ValueType::Object) && aa != ba)
            conflict(path, "scalar or array metadata differs");
        (void)mergeAnnotations(std::move(aa), std::move(ba), path);
    }
    /** Unify whole equivalence classes, checking every new cross-contributor pair once. */
    void unify()
    {
        while (!pending_.empty()) {
            auto [left, right, path] = std::move(pending_.back());
            pending_.pop_back();
            left = representative(left);
            right = representative(right);
            if (left == right)
                continue;
            if (members_[left].size() < members_[right].size())
                std::swap(left, right);
            for (auto a : members_[left])
                for (auto b : members_[right]) {
                    if (++work_ > 64 * 1024 * 1024)
                        throw std::length_error("Schema composition work budget exceeded.");
                    compare(a, b, path);
                }
            parents_[right] = left;
            members_[left]
                .insert(members_[left].end(), members_[right].begin(), members_[right].end());
            members_[right].clear();
        }
    }
    /** Translate a value-domain edge after final construction identities are assigned. */
    TypeId mapped(size_t source, TypeId id) const
    {
        auto result = mapped_[slot(source, id)];
        if (id && !result)
            throw std::invalid_argument("Producer data refers to a generated query context.");
        return result;
    }
    /** Copy one native declaration into its merged output row. */
    void materialize(size_t index, bool first)
    {
        auto const& source = node(index);
        auto origin = origins_[index].first;
        auto id = mapped_[index];
        auto& out = target_.builder->schemas_[id];
        if (first) {
            out = source;
            out.id_ = out.entry_.id_ = id;
            out.directFields_.clear();
            out.childSchemas_.clear();
            out.elementSchemas_.clear();
            out.alternatives_.clear();
            out.attributeOwners_.clear();
            out.annotations_ = nlohmann::json::object();
        }
        auto text = [](auto& value, auto const& other)
        {
            if (value.empty())
                value = other;
        };
        text(out.typeName_, source.typeName_);
        text(out.zserioType_, source.zserioType_);
        text(out.attributeType_, source.attributeType_);
        text(out.attributeTypeCode_, source.attributeTypeCode_);
        text(out.entry_.metaType_, source.entry_.metaType_);
        text(out.entry_.key_, source.entry_.key_);
        text(out.entry_.jsonPointer_, source.entry_.jsonPointer_);
        out.open_ = out.open_ || source.open_;
        out.required_.insert(source.required_.begin(), source.required_.end());
        out.multimapFields_.insert(source.multimapFields_.begin(), source.multimapFields_.end());
        out.contextFields_.insert(source.contextFields_.begin(), source.contextFields_.end());
        for (auto const& name : source.directFields_) {
            auto [children, inserted] = out.childSchemas_.try_emplace(name);
            if (inserted) {
                out.directFields_.push_back(name);
                if (auto found = source.childSchemas_.find(name);
                    found != source.childSchemas_.end())
                    for (auto child : found->second)
                        children->second.push_back(mapped(origin, child));
            }
        }
        // Ordered alternatives were paired during compatibility checking. Keep
        // their multiplicity: oneOf(A, A) is not equivalent to oneOf(A).
        if (first) {
            for (auto child : source.elementSchemas_)
                out.elementSchemas_.push_back(mapped(origin, child));
            for (auto child : source.alternatives_)
                out.alternatives_.push_back(mapped(origin, child));
        }
        for (auto owner : source.attributeOwners_) {
            owner.attributeSchema_ = id;
            target_.builder->registerAttributeOwner(id, std::move(owner));
        }
        target_.builder->entriesById_[id] = out.entry_;
    }

public:
    /** Snapshot only transient native declarations; published graphs remain untouched. */
    CompositionBuilder(Impl& target, std::span<ConstPtr const> inputs)
        : target_(target), inputs_(inputs)
    {
        for (size_t s = 0; s < inputs.size(); ++s) {
            graphs_.push_back(inputs[s]->schema_->exportBuilder());
            offsets_.push_back(origins_.size());
            for (size_t id = 1; id < graphs_.back().schemas_.size(); ++id) {
                if (origins_.size() >= MaxEdges)
                    throw std::length_error("Composition input graph budget exceeded.");
                auto index = origins_.size();
                origins_.emplace_back(s, TypeId(id));
                parents_.push_back(index);
                members_.push_back({index});
                mapped_.push_back(0);
            }
            for (size_t l = 0; l < inputs[s]->schema_->layers.size(); ++l)
                layers_[inputs[s]->schema_->layers[l].name].emplace_back(s, l);
            target_.constituents
                .emplace_back(std::string(inputs[s]->schemaId()), inputs[s]->format());
        }
    }
    /** Merge contracts, rebuild query overlays, and retain ingestion-only translation tables. */
    void build()
    {
        std::map<std::string_view, size_t, std::less<>> globalKeys;
        for (size_t s = 0; s < inputs_.size(); ++s)
            for (auto const& [key, id] : inputs_[s]->schema_->globalAliases) {
                auto [it, inserted] = globalKeys.emplace(key, slot(s, id));
                if (!inserted)
                    pair(it->second, slot(s, id), std::string(key));
            }
        for (auto const& [name, sources] : layers_) {
            auto const& first =
                inputs_[sources.front().first]->schema_->layers[sources.front().second];
            std::map<std::string_view, std::pair<FeatureTypeInfo const*, size_t>, std::less<>>
                features;
            std::map<std::string_view, size_t, std::less<>> aliases;
            for (auto [s, l] : sources) {
                auto const& layer = inputs_[s]->schema_->layers[l];
                if (first.type != layer.type)
                    conflict(name, "layer kind differs");
                if (layer.features.empty() || first.features.empty())
                    pair(
                        slot(sources.front().first, first.root),
                        slot(s, layer.root),
                        name + "/root");
                for (size_t f = 0; f < layer.features.size(); ++f) {
                    auto const& feature = layer.features[f];
                    auto [it, inserted] = features.emplace(
                        feature.name_,
                        std::pair{&feature, slot(s, layer.featureRoots[f])});
                    if (!inserted) {
                        if (it->second.first->toJson() != feature.toJson())
                            conflict(
                                name + "/" + feature.name_,
                                "feature identity composition differs");
                        pair(
                            it->second.second,
                            slot(s, layer.featureRoots[f]),
                            name + "/" + feature.name_);
                    }
                }
                for (auto const& [key, id] : layer.aliases) {
                    auto [it, inserted] = aliases.emplace(key, slot(s, id));
                    if (!inserted)
                        pair(it->second, slot(s, id), name + "/" + std::string(key));
                }
            }
        }
        unify();
        // Assign only effective IDs, not the sum of contributor namespaces. Shared
        // recursive graphs can therefore compose without exhausting uint16_t early.
        for (size_t i = 1; i < origins_.size(); ++i) {
            if (node(i).queryRoot_)
                continue;
            auto root = representative(i);
            if (!mapped_[root])
                mapped_[root] = target_.builder->allocate(node(i).kind_, {}, {}, {});
            mapped_[i] = mapped_[root];
        }
        std::set<TypeId> initialized;
        for (size_t i = 1; i < origins_.size(); ++i)
            if (!node(i).queryRoot_)
                materialize(i, initialized.insert(mapped_[i]).second);
        for (auto const& [name, sources] : layers_) {
            Layer layer;
            layer.name = name;
            layer.relationsDeclared = true;
            layer.relationsKnown = true;
            for (auto [s, l] : sources) {
                auto const& source = inputs_[s]->schema_->layers[l];
                layer.type = source.type;
                layer.relationsKnown = layer.relationsKnown && source.relationsKnown;
                for (auto const& relation : source.relations) {
                    auto previous = std::ranges::find_if(
                        layer.relations,
                        [&](auto const& value) {
                            return value.ownerType_ == relation.ownerType_ &&
                                value.name_ == relation.name_;
                        });
                    if (previous == layer.relations.end())
                        layer.relations.push_back(relation);
                    else if (
                        previous->targetTypes_ != relation.targetTypes_ ||
                        previous->complete_ != relation.complete_ ||
                        previous->locality_ != relation.locality_)
                        conflict(name, "forward relation contract differs");
                }
                if (source.features.empty())
                    layer.root = mapped(s, source.root);
                for (size_t f = 0; f < source.features.size(); ++f) {
                    auto found = std::ranges::find(
                        layer.features,
                        source.features[f].name_,
                        &FeatureTypeInfo::name_);
                    if (found == layer.features.end()) {
                        layer.features.push_back(source.features[f]);
                        layer.featureRoots.push_back(mapped(s, source.featureRoots[f]));
                        layer.produced.push_back(source.produced[f]);
                    }
                    else
                        layer.produced[found - layer.features.begin()] |= source.produced[f];
                }
                for (auto const& [key, id] : source.aliases)
                    layer.buildAliases[std::string(key)] = mapped(s, id);
            }
            target_.layers.push_back(std::move(layer));
        }
        for (auto const& [key, index] : globalKeys)
            target_.builder->idsByKey_.emplace(key, mapped_[index]);
        target_.prepareQueryRoots();
        for (size_t s = 0; s < inputs_.size(); ++s) {
            for (auto const& [context, query] : inputs_[s]->schema_->queryRoots)
                mapped_[slot(s, query)] =
                    target_.queryRoots.at({mapped(s, context.first), mapped(s, context.second)});
            for (size_t id = 1; id < graphs_[s].schemas_.size(); ++id)
                if (graphs_[s].schemas_[id].queryRoot_ && !mapped_[slot(s, TypeId(id))]) {
                    auto const& source = graphs_[s].schemas_[id];
                    for (auto const& candidate : target_.builder->schemas_)
                        if (candidate.queryRoot_ && candidate.kind_ == source.kind_ &&
                            candidate.entry_.metaType_.empty()) {
                            mapped_[slot(s, TypeId(id))] = candidate.id_;
                            break;
                        }
                    if (!mapped_[slot(s, TypeId(id))])
                        throw std::invalid_argument("Unmapped generated query scalar.");
                }
            auto& remap = target_.contributorRemaps[std::string(inputs_[s]->schemaId())];
            remap.resize(graphs_[s].schemas_.size());
            for (size_t id = 1; id < remap.size(); ++id)
                remap[id] = mapped(s, TypeId(id));
        }
        // Reference IDs are final now. Keep annotations only on canonical rows;
        // overlay annotations are inherited from the merged payload below.
        initialized.clear();
        for (size_t i = 1; i < origins_.size(); ++i) {
            if (node(i).queryRoot_)
                continue;
            auto extra = node(i).annotations_;
            schema_detail::SchemaBuilder::validationSchemas(
                extra,
                [&](auto& value, auto const&)
                {
                    value["$ref"] = "#/definitions/" +
                        std::to_string(mapped(origins_[i].first, reference(value)));
                });
            auto& out = target_.builder->schemas_[mapped_[i]].annotations_;
            if (initialized.insert(mapped_[i]).second)
                out = std::move(extra);
            else
                out = mergeAnnotations(std::move(out), std::move(extra), node(i).entry_.key_);
        }
        for (auto const& [context, query] : target_.queryRoots)
            target_.builder->schemas_[query].annotations_ =
                target_.builder->schemas_[context.second].annotations_;
    }
};
