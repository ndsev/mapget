/** Construction-only compatibility and namespace translation. This implementation
 * is included after MapSchema::Impl so none of its scratch state is public ABI. */
class MapSchema::Impl::CompositionBuilder
{
    Impl& target_;
    std::span<ConstPtr const> inputs_;
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
        if (id > inputs_[source]->typeCount())
            throw std::invalid_argument("Dangling constituent schema reference.");
        return offsets_[source] + id - 1;
    }
    /** Borrow the canonical declaration; source owners outlive this construction. */
    simfil::SchemaType node(size_t index) const
    {
        auto [source, id] = origins_[index];
        return inputs_[source]->schemaType(id);
    }
    /** Mapget metadata stays in its owner's column, never in a copied generic graph. */
    Impl const& source(size_t index) const { return *inputs_[origins_[index].first]->schema_; }
    /** Generated contexts are reconstructed after producer declarations have been merged. */
    bool queryRoot(size_t index) const
    {
        return source(index).data(origins_[index].second).flags & QueryRootFlag;
    }
    /** Name IDs belong to their input dictionary, including application-specific kinds. */
    std::string_view kindName(size_t index) const
    {
        return source(index).name(simfil::SchemaType::kindNameId(node(index).kind()));
    }
    /** Translate an open kind vocabulary without copying source dictionary IDs. */
    Kind targetKind(size_t index)
    {
        return simfil::SchemaType::makeKind(
            target_.intern(kindName(index)),
            simfil::SchemaType::affinities(node(index).kind()));
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
    template <class A, class B>
    void edges(size_t left, size_t right, A const& a, B const& b, std::string const& path)
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
    /** Compare scalar declarations directly, preserving full-width encodings and source order. */
    void scalarMetadata(simfil::SchemaType a, simfil::SchemaType b, std::string const& path)
    {
        std::vector<std::string_view> as, bs;
        a.forEachEnumSymbol([&](auto id)
                            { as.push_back(a.pool()->strings()->resolve(id).value()); });
        b.forEachEnumSymbol([&](auto id)
                            { bs.push_back(b.pool()->strings()->resolve(id).value()); });
        std::vector<simfil::SchemaType::EnumValue> av, bv;
        a.forEachEnumValue([&](auto const& value) { av.push_back(value); });
        b.forEachEnumValue([&](auto const& value) { bv.push_back(value); });
        if (as != bs || av != bv || a.bitWidth() != b.bitWidth())
            conflict(path, "enum or bitmask contract differs");
        auto ad = a.numericDomain(), bd = b.numericDomain();
        if (bool(ad) != bool(bd))
            conflict(path, "numeric interpretation differs");
        if (ad) {
            if (ad->encoding != bd->encoding || ad->unit != bd->unit ||
                ad->minimum != bd->minimum || ad->maximum != bd->maximum ||
                ad->scale != bd->scale || ad->offset != bd->offset ||
                ad->complete != bd->complete ||
                ad->ordinaryRanges.size() != bd->ordinaryRanges.size())
                conflict(path, "numeric interpretation differs");
            for (size_t i = 0; i < ad->ordinaryRanges.size(); ++i)
                if (ad->ordinaryRanges[i].min != bd->ordinaryRanges[i].min ||
                    ad->ordinaryRanges[i].max != bd->ordinaryRanges[i].max)
                    conflict(path, "ordinary measurement ranges differ");
        }
        std::vector<simfil::SchemaType::NamedValue> an, bn;
        a.forEachNamedValue([&](auto const& value) { an.push_back(value); });
        b.forEachNamedValue([&](auto const& value) { bn.push_back(value); });
        if (an.size() != bn.size())
            conflict(path, "named value contract differs");
        for (size_t i = 0; i < an.size(); ++i) {
            auto const& x = an[i];
            auto const& y = bn[i];
            if (x.name != y.name || x.value != y.value || x.rawValue != y.rawValue ||
                x.role != y.role || x.meaning != y.meaning || x.deprecated != y.deprecated ||
                x.removed != y.removed)
                conflict(path, "named value contract differs");
        }
    }
    /** Check shared meaning and discover child correspondences. New object fields are additive. */
    void compare(size_t left, size_t right, std::string const& path)
    {
        auto a = node(left), b = node(right);
        auto const& am = source(left).data(a.id());
        auto const& bm = source(right).data(b.id());
        if (queryRoot(left) || queryRoot(right))
            conflict(path, "generated query contexts are not producer declarations");
        if (kindName(left) != kindName(right) ||
            simfil::SchemaType::affinities(a.kind()) != simfil::SchemaType::affinities(b.kind()) ||
            a.composition() != b.composition() || a.nullable() != b.nullable())
            conflict(path, "kind, composition or nullability differs");
        auto text = [&](auto x, auto y)
        {
            if (!x.empty() && !y.empty() && x != y)
                conflict(path, "declared type meaning differs");
        };
        text(a.typeName(), b.typeName());
        text(source(left).text(am.zserioType), source(right).text(bm.zserioType));
        text(source(left).text(am.metaType), source(right).text(bm.metaType));
        text(source(left).text(am.attributeType), source(right).text(bm.attributeType));
        text(source(left).text(am.attributeCode), source(right).text(bm.attributeCode));
        scalarMetadata(a, b, path);
        edges(left, right, a.elements(), b.elements(), path + "/items");
        edges(left, right, a.alternatives(), b.alternatives(), path + "/alternatives");
        pair(
            slot(origins_[left].first, a.baseType().id()),
            slot(origins_[right].first, b.baseType().id()),
            path + "/base");
        auto fields = [&](simfil::SchemaType x, simfil::SchemaType y, size_t xs, size_t ys)
        {
            x.forEachDirectField(
                [&](auto const& field)
                {
                    auto other = y.field(field.name());
                    if (!other)
                        return;
                    auto fieldPath = path + "/" + std::string(field.name());
                    if (other.name() != field.name())
                        conflict(fieldPath, "lookup alias and enumerable field differ");
                    if (field.required() != other.required() ||
                        field.multimap() != other.multimap() || field.role() != other.role())
                        conflict(fieldPath, "presence, multiplicity or enumeration role differs");
                    edges(xs, ys, field.types(), other.types(), fieldPath);
                });
        };
        fields(a, b, left, right);
        fields(b, a, right, left);
        auto aliases = [&](simfil::SchemaType x, simfil::SchemaType y, size_t origin)
        {
            x.forEachFieldAlias(
                [&](auto alias, auto canonical)
                {
                    auto aliasName = source(origin).name(alias);
                    auto other = y.field(aliasName);
                    if (other && other.name() != source(origin).name(canonical))
                        conflict(path + "/" + std::string(aliasName), "field alias target differs");
                });
        };
        aliases(a, b, left);
        aliases(b, a, right);
        // Validation-only references are graph edges too, but numeric namespaces
        // differ. Normalize references only for comparison, never literal values.
        auto aa = source(left).annotation(am.annotations),
             ba = source(right).annotation(bm.annotations);
        std::map<std::string, TypeId> ar, br;
        auto normalize = [&](auto& json, auto& refs)
        {
            schema_detail::forEachValidationSchema(
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
        if (!a.hasAffinity(simfil::ValueType::Object) && aa != ba)
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
    /** Translate a type edge after final construction identities are assigned. */
    TypeId mapped(size_t source, TypeId id) const
    {
        auto result = mapped_[slot(source, id)];
        if (id && !result)
            throw std::invalid_argument("Producer data refers to a generated query context.");
        return result;
    }
    /** Write merged declarations directly into canonical columns and arenas. */
    void materialize(size_t index, bool first)
    {
        auto sourceType = node(index);
        auto const& origin = source(index);
        auto originIndex = origins_[index].first;
        auto id = mapped_[index];
        // Keep the derived owner: this canonical copier preserves arbitrary source
        // declarations through its protected primitives, never a public ID facade.
        auto& graph = target_.pool;
        auto out = graph.schemaType(id);
        auto const& metadata = origin.data(sourceType.id());
        auto& outMetadata = target_.data(id);
        if (first) {
            out.setComposition(sourceType.composition());
            out.setNullable(sourceType.nullable());
            if (auto base = sourceType.baseType().id())
                graph.setBaseSchema(id, mapped(originIndex, base));
            for (auto child : sourceType.elements())
                graph.addElementSchema(id, mapped(originIndex, child));
            // Keep order and multiplicity: oneOf(A, A) must not turn into oneOf(A).
            for (auto child : sourceType.alternatives())
                graph.addAlternative(id, mapped(originIndex, child));
            sourceType.forEachEnumSymbol(
                [&](auto symbol) { graph.addEnumSymbol(id, origin.name(symbol)); });
            sourceType.forEachEnumValue([&](auto const& value) { graph.addEnumValue(id, value); });
            if (sourceType.isBitmask())
                out.asBitmask().setBitWidth(sourceType.bitWidth());
            if (auto numeric = sourceType.numericDomain())
                out.asScalar().setNumericDomain(*numeric);
            sourceType.forEachNamedValue([&](auto const& value) { out.addNamedValue(value); });
        }
        if (out.typeName().empty())
            out.setTypeName(sourceType.typeName());
        auto text = [&](ModelNodeAddress& address, ModelNodeAddress other)
        {
            if (!address)
                address = target_.text(origin.text(other));
        };
        text(outMetadata.zserioType, metadata.zserioType);
        text(outMetadata.attributeType, metadata.attributeType);
        text(outMetadata.attributeCode, metadata.attributeCode);
        text(outMetadata.metaType, metadata.metaType);
        text(outMetadata.key, metadata.key);
        text(outMetadata.pointer, metadata.pointer);
        graph.setOpen(id, out.open() || sourceType.open());
        sourceType.forEachDirectField(
            [&](auto const& field)
            {
                auto previous = out.field(field.name());
                if (previous && previous.parentType() == out)
                    return;
                // Generic setters append edges, including explicit unknown targets.
                for (auto child : field.types())
                    graph.addFieldSchema(
                        id,
                        field.name(),
                        mapped(originIndex, child),
                        field.multimap());
                graph.setFieldRequired(id, field.name(), field.required());
                graph.setFieldRole(id, field.name(), field.role());
            });
        sourceType.forEachFieldAlias(
            [&](auto alias, auto canonical)
            {
                auto name = origin.name(alias), target = origin.name(canonical);
                bool present = false;
                out.forEachFieldAlias(
                    [&](auto existing, auto destination)
                    {
                        if (target_.name(existing) != name)
                            return;
                        present = true;
                        if (target_.name(destination) != target)
                            conflict(std::string(name), "field alias target differs");
                    });
                if (!present)
                    graph.addFieldAlias(id, name, target);
            });
        for (auto owner : origin.attributeOwners(sourceType.id())) {
            owner.attributeSchema_ = id;
            target_.pool.setAttributeMetadata(
                id,
                std::move(owner),
                std::string(origin.text(metadata.attributeType)),
                std::string(origin.text(metadata.zserioType)));
        }
    }

public:
    /** Retain only source IDs and transient equivalence state; source graphs remain untouched. */
    CompositionBuilder(Impl& target, std::span<ConstPtr const> inputs)
        : target_(target), inputs_(inputs)
    {
        for (size_t s = 0; s < inputs.size(); ++s) {
            offsets_.push_back(origins_.size());
            for (size_t id = 1; id <= inputs[s]->typeCount(); ++id) {
                if (origins_.size() >= MaxEdges)
                    throw std::length_error("Composition input graph budget exceeded.");
                auto index = origins_.size();
                origins_.emplace_back(s, TypeId(id));
                parents_.push_back(index);
                members_.push_back({index});
                mapped_.push_back(0);
            }
            for (size_t l = 0; l < inputs[s]->schema_->layers.size(); ++l)
                layers_[std::string(inputs[s]->schema_->name(inputs[s]->schema_->layers[l].name))]
                    .emplace_back(s, l);
            target_.constituents
                .emplace_back(std::string(inputs[s]->schemaId()), inputs[s]->format());
        }
    }
    /** Merge contracts, rebuild query overlays, and retain ingestion-only translation tables. */
    void build()
    {
        for (auto const& input : inputs_)
            for (auto const& identity : input->featureIdentities())
                target_.pool.addFeatureType(identity);
        std::map<std::string_view, size_t, std::less<>> globalKeys;
        for (size_t s = 0; s < inputs_.size(); ++s)
            inputs_[s]->schema_->forEachAlias(
                GlobalLayer,
                [&](std::string_view key, TypeId id)
                {
                    auto [it, inserted] = globalKeys.emplace(key, slot(s, id));
                    if (!inserted)
                        pair(it->second, slot(s, id), std::string(key));
                });
        for (auto const& [name, sources] : layers_) {
            auto const& first =
                inputs_[sources.front().first]->schema_->layers[sources.front().second];
            std::map<std::string_view, size_t, std::less<>> features;
            std::map<std::string_view, size_t, std::less<>> aliases;
            for (auto [s, l] : sources) {
                auto const& layer = inputs_[s]->schema_->layers[l];
                if (first.type != layer.type)
                    conflict(name, "layer kind differs");
                if (inputs_[s]->schema_->featureRoots(layer).empty() ||
                    inputs_[sources.front().first]->schema_->featureRoots(first).empty())
                    pair(
                        slot(sources.front().first, first.root),
                        slot(s, layer.root),
                        name + "/root");
                for (auto const& [featureId, root] : inputs_[s]->schema_->featureRoots(layer)) {
                    auto feature = inputs_[s]->schema_->name(featureId);
                    auto [it, inserted] = features.emplace(feature, slot(s, root));
                    if (!inserted) {
                        pair(it->second, slot(s, root), name + "/" + std::string(feature));
                    }
                }
                inputs_[s]->schema_->forEachAlias(
                    l,
                    [&](std::string_view key, TypeId id)
                    {
                        auto [it, inserted] = aliases.emplace(key, slot(s, id));
                        if (!inserted)
                            pair(it->second, slot(s, id), name + "/" + std::string(key));
                    });
            }
        }
        unify();
        // Assign only effective IDs, not the sum of contributor namespaces. Shared
        // recursive graphs can therefore compose without exhausting uint16_t early.
        for (size_t i = 1; i < origins_.size(); ++i) {
            if (queryRoot(i))
                continue;
            auto root = representative(i);
            if (!mapped_[root])
                mapped_[root] = target_.pool.newType(targetKind(i)).id();
            mapped_[i] = mapped_[root];
        }
        std::set<TypeId> initialized;
        for (size_t i = 1; i < origins_.size(); ++i)
            if (!queryRoot(i))
                materialize(i, initialized.insert(mapped_[i]).second);
        for (auto const& [name, sources] : layers_) {
            auto const& first =
                inputs_[sources.front().first]->schema_->layers[sources.front().second];
            auto output = target_.pool.newLayer(name, LayerType(first.type));
            std::map<std::string, TypeId, std::less<>> localAliases;
            std::map<std::string_view, TypeId, std::less<>> produced;
            // These declaration inputs exist only during composition; the owner stores rows
            // immediately.
            std::vector<LayerSchema::RelationDeclaration> relations;
            bool known = true;
            for (auto [s, l] : sources) {
                auto const& origin = *inputs_[s]->schema_;
                auto const& source = origin.layers[l];
                known = known && origin.relationsKnown(source);
                for (auto const& row : origin.relationsOf(source)) {
                    auto declaration = origin.relationDeclaration(row);
                    auto previous = std::ranges::find_if(
                        relations,
                        [&](auto const& value) {
                            return value.ownerType_ == declaration.ownerType_ &&
                                value.name_ == declaration.name_;
                        });
                    if (previous == relations.end())
                        relations.push_back(std::move(declaration));
                    else if (
                        previous->targetTypes_ != declaration.targetTypes_ ||
                        previous->complete_ != declaration.complete_ ||
                        previous->locality_ != declaration.locality_)
                        conflict(name, "forward relation contract differs");
                }
                auto roots = origin.featureRoots(source);
                if (roots.empty())
                    output->setRootSchema(mapped(s, source.root));
                for (auto const& row : roots)
                    produced.emplace(origin.name(row.name), mapped(s, row.root));
                origin.forEachAlias(
                    l,
                    [&](std::string_view key, TypeId id)
                    { localAliases[std::string(key)] = mapped(s, id); });
            }
            for (auto const& [feature, root] : produced)
                output->addFeatureType(feature, root);
            target_.setRelations(output->addr().index(), std::move(relations), known);
            for (auto const& [key, id] : localAliases)
                output->registerSchemaKey(key, id);
        }
        for (auto const& [key, index] : globalKeys)
            target_.pool.registerSchemaKey(std::string(key), mapped_[index]);
        target_.prepareQueryRoots();
        for (size_t s = 0; s < inputs_.size(); ++s) {
            for (auto const& query : inputs_[s]->schema_->queryRoots) {
                auto merged =
                    target_.findQueryRoot(mapped(s, query.feature), mapped(s, query.attribute));
                if (!merged)
                    throw std::invalid_argument("Merged query context is unavailable.");
                mapped_[slot(s, query.root)] = merged;
            }
            for (size_t id = 1; id <= inputs_[s]->typeCount(); ++id)
                if (queryRoot(slot(s, TypeId(id))) && !mapped_[slot(s, TypeId(id))]) {
                    for (size_t candidate = 1; candidate <= target_.pool.typeCount(); ++candidate)
                        if ((target_.data(TypeId(candidate)).flags & QueryRootFlag) &&
                            target_.pool.schemaType(TypeId(candidate)).kind() ==
                                targetKind(slot(s, TypeId(id))) &&
                            !target_.data(TypeId(candidate)).metaType)
                        {
                            mapped_[slot(s, TypeId(id))] = TypeId(candidate);
                            break;
                        }
                    if (!mapped_[slot(s, TypeId(id))])
                        throw std::invalid_argument("Unmapped generated query scalar.");
                }
            auto& remap = target_.contributorRemaps[std::string(inputs_[s]->schemaId())];
            remap.resize(inputs_[s]->typeCount() + 1);
            for (size_t id = 1; id < remap.size(); ++id)
                remap[id] = mapped(s, TypeId(id));
        }
        // Merge annotations one equivalence class at a time, then allocate CBOR
        // once. Repeated writes would leave superseded payloads in the ModelPool.
        for (size_t i = 1; i < origins_.size(); ++i) {
            if (queryRoot(i) || members_[i].empty())
                continue;
            auto members = members_[i];
            std::ranges::sort(members);
            nlohmann::json merged;
            for (auto member : members) {
                auto const& metadata = source(member).data(node(member).id());
                auto extra = source(member).annotation(metadata.annotations);
                schema_detail::forEachValidationSchema(
                    extra,
                    [&](auto& value, auto const&)
                    {
                        value["$ref"] = "#/definitions/" +
                            std::to_string(mapped(origins_[member].first, reference(value)));
                    });
                merged = merged.is_null() ?
                    std::move(extra) :
                    mergeAnnotations(
                        std::move(merged),
                        std::move(extra),
                        std::string(source(member).text(metadata.key)));
            }
            target_.data(mapped_[i]).annotations = target_.annotation(merged);
        }
        for (auto const& query : target_.queryRoots)
            target_.data(query.root).annotations = target_.data(query.attribute).annotations;
    }
};
