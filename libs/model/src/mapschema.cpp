#include "mapget/model/mapschema.h"
#include "mapschema-impl.h"

#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <picosha2.h>
#include "simfil/model/schema-model.h"

namespace mapget
{
using schema_detail::Composition;
using schema_detail::hasAffinity;
using schema_detail::Kind;
using schema_detail::matchesKind;
using simfil::ModelNodeAddress;
using simfil::StringId;
using simfil::TypeId;

#include "mapschema-composition.h"

MapSchema::MapSchema(MapFormat format)
    : SchemaPool(std::make_shared<StringPool>("schema")),
      schema_(std::make_unique<Impl>(*this, MapFormat::fromJson(format.toJson())))
{
}
MapSchema::~MapSchema() = default;
void MapSchema::requireMutable() const
{
    if (schema_->frozen || !schema_->builder)
        throw std::logic_error("MapSchema is immutable after finalization.");
}
bool MapSchema::finalized() const
{
    return schema_->frozen;
}
MapFormat const& MapSchema::format() const
{
    return schema_->format;
}
std::string_view MapSchema::schemaId() const
{
    if (!finalized())
        throw std::logic_error("MapSchema identity is unavailable before finalization.");
    return schema_->identity;
}
size_t MapSchema::typeCount() const
{
    return (schema_->builder ? schema_->builder->schemas_.size() : schema_->types.size()) - 1;
}

LayerSchema::Ptr MapSchema::newLayer(std::string_view name, LayerType type)
{
    requireMutable();
    if (name.empty())
        throw std::invalid_argument("A schema layer requires a nonempty name.");
    if (auto existing = layer(name)) {
        if (existing->layerType() != type)
            throw std::invalid_argument(
                "Schema layer kind conflicts with an existing declaration.");
        return existing;
    }
    if (schema_->layers.size() == UINT16_MAX)
        throw std::length_error("MapSchema layer count exceeds 16-bit namespace.");
    auto index = schema_->layers.size();
    Impl::Layer item;
    item.name = name;
    item.type = type;
    schema_->layers.push_back(std::move(item));
    auto result = LayerSchema::Ptr::make(shared_from_this(), ModelNodeAddress{Impl::Layers, index});
    return result;
}
LayerSchema::Ptr MapSchema::layer(std::string_view name) const
{
    for (size_t i = 0; i < schema_->layers.size(); ++i)
        if (schema_->layers[i].name == name)
            return LayerSchema::Ptr::make(shared_from_this(), ModelNodeAddress{Impl::Layers, i});
    return {};
}
std::vector<std::string> MapSchema::layerNames() const
{
    std::vector<std::string> names;
    for (auto const& layer : schema_->layers)
        names.push_back(layer.name);
    return names;
}

TypeId MapSchema::addSchema(Kind kind, std::string key, std::string metaType, std::string pointer)
{
    requireMutable();
    if (!key.empty() && schema_->builder->idsByKey_.contains(key))
        throw std::invalid_argument("Duplicate global schema key: " + key);
    return schema_->builder
        ->allocate(kind, std::move(key), std::move(pointer), std::move(metaType));
}
void MapSchema::registerSchemaKey(std::string key, TypeId id)
{
    requireMutable();
    schema_->requireType(id);
    auto [it, inserted] = schema_->builder->idsByKey_.emplace(std::move(key), id);
    if (!inserted && it->second != id)
        throw std::invalid_argument("A global schema key has conflicting declarations.");
}
TypeId MapSchema::findSchema(std::string_view key) const
{
    if (schema_->builder) {
        auto it = schema_->builder->idsByKey_.find(key);
        return it == schema_->builder->idsByKey_.end() ? 0 : it->second;
    }
    auto it = schema_->globalAliases.find(key);
    return it == schema_->globalAliases.end() ? 0 : it->second;
}
void MapSchema::addFieldSchema(TypeId parent, std::string field, TypeId child, bool multimap)
{
    requireMutableType(parent);
    if (child)
        schema_->requireType(child);
    auto& builder = *schema_->builder;
    auto const& definition = builder.schemas_[parent];
    if (definition.reservedFields_.contains(field)) {
        auto found = definition.childSchemas_.find(field);
        if (found == definition.childSchemas_.end() ||
            found->second != std::vector<TypeId>{child} ||
            definition.multimapFields_.contains(field) != multimap)
            throw std::invalid_argument("Cannot redefine reserved mapget field: " + field);
        return;
    }
    builder.addDirectField(parent, field);
    if (child)
        builder.addChild(parent, field, child);
    if (multimap)
        builder.schemas_[parent].multimapFields_.insert(std::move(field));
}
void MapSchema::addElementSchema(TypeId parent, TypeId child)
{
    requireMutableType(parent);
    schema_->requireType(child);
    schema_->builder->addElementSchema(parent, child);
}
void MapSchema::addEnumSymbol(TypeId id, std::string name)
{
    requireMutableType(id);
    schema_->builder->addEnumSymbol(id, name);
}
void MapSchema::addEnumSymbols(TypeId id, std::span<std::string const> names)
{
    for (auto const& name : names)
        addEnumSymbol(id, name);
}
void MapSchema::addEnumValue(TypeId id, simfil::ScalarValueType value)
{
    requireMutableType(id);
    if (std::holds_alternative<std::string>(value) ||
        std::holds_alternative<std::string_view>(value) ||
        std::holds_alternative<simfil::Bitmask>(value))
        throw std::invalid_argument("Use symbol/named-mask declarations for strings and bitmasks.");
    schema_->builder->schemas_[id].enumValues_.push_back(std::move(value));
}
void MapSchema::setComposition(TypeId id, Composition op)
{
    requireMutableType(id);
    auto& d = schema_->builder->schemas_[id];
    d.composition_ = op;
    if (op != Composition::None)
        d.kind_ = op == Composition::AllOf ?
            Kind::Intersection :
            op == Composition::OneOf ?
            Kind::OneOf :
            Kind::Union;
}
void MapSchema::addAlternative(TypeId id, TypeId other)
{
    requireMutableType(id);
    schema_->requireType(other);
    auto& ids = schema_->builder->schemas_[id].alternatives_;
    if (schema_->builder->schemas_[id].composition_ == Composition::OneOf ||
        std::ranges::find(ids, other) == ids.end())
        ids.push_back(other);
}
void MapSchema::setTypeName(TypeId id, std::string name)
{
    requireMutableType(id);
    schema_->builder->schemas_[id].typeName_ = std::move(name);
}
void MapSchema::setOpen(TypeId id, bool open)
{
    requireMutableType(id);
    schema_->builder->schemas_[id].open_ = open;
}
void MapSchema::setNullable(TypeId id, std::optional<bool> value)
{
    requireMutableType(id);
    schema_->builder->schemas_[id].nullable_ = value;
}
void MapSchema::setFieldRequired(TypeId id, std::string field, std::optional<bool> required)
{
    requireMutableType(id);
    auto& values = schema_->builder->schemas_[id].required_;
    if (schema_->builder->schemas_[id].reservedFields_.contains(field)) {
        auto found = values.find(field);
        if (found == values.end() ? required.has_value() : required != found->second)
            throw std::invalid_argument("Cannot change reserved field presence: " + field);
        return;
    }
    if (required)
        values[std::move(field)] = *required;
    else
        values.erase(field);
}
void MapSchema::setZserioType(TypeId id, std::string name)
{
    requireMutableType(id);
    auto& d = schema_->builder->schemas_[id];
    d.zserioType_ = std::move(name);
    if (d.typeName_.empty())
        d.typeName_ = d.zserioType_;
}

void MapSchema::setFieldRole(TypeId id, std::string field, simfil::Schema::FieldRole role)
{
    requireMutableType(id);
    auto& fields = schema_->builder->schemas_[id].contextFields_;
    if (schema_->builder->schemas_[id].reservedFields_.contains(field) &&
        (role == simfil::Schema::FieldRole::Context) != fields.contains(field))
        throw std::invalid_argument("Cannot change reserved field role: " + field);
    if (role == simfil::Schema::FieldRole::Context)
        fields.insert(std::move(field));
    else
        fields.erase(field);
}
void MapSchema::setAttributeMetadata(
    TypeId id,
    LayerSchema::AttributePathOwner owner,
    std::string type,
    std::string zserio)
{
    requireMutableType(id);
    auto& d = schema_->builder->schemas_[id];
    d.attributeType_ = std::move(type);
    if (!zserio.empty())
        d.zserioType_ = std::move(zserio);
    if (d.typeName_.empty())
        d.typeName_ = d.zserioType_.empty() ? d.attributeType_ : d.zserioType_;
    schema_->builder->registerAttributeOwner(id, std::move(owner));
    if (!d.attributeOwners_.empty()) {
        auto const& first = d.attributeOwners_.front().attributeName_;
        d.attributeTypeCode_ =
            std::ranges::all_of(
                d.attributeOwners_,
                [&](auto const& site) { return site.attributeName_ == first; }) ?
            first :
            std::string{};
    }
}
void MapSchema::setJsonSchemaAnnotations(TypeId id, nlohmann::json annotations)
{
    requireMutableType(id);
    if (!annotations.is_object() ||
        schema_detail::SchemaBuilder::annotations(annotations) != annotations)
        throw std::invalid_argument(
            "Schema annotations cannot replace structural type declarations.");
    auto& builder = *schema_->builder;
    auto start = builder.schemas_.size();
    std::optional<decltype(builder.idsByKey_)> aliases;
    try {
        schema_detail::SchemaBuilder::validationSchemas(
            annotations,
            [&](auto& constraint, auto const&)
            {
                if (!aliases)
                    aliases = builder.idsByKey_;
                auto child = addJsonSchema(constraint);
                constraint = {{"$ref", "#/definitions/" + std::to_string(child)}};
            });
        builder.schemas_[id].annotations_ = std::move(annotations);
    }
    catch (...) {
        builder.schemas_.resize(start);
        builder.entriesById_.resize(start);
        if (aliases)
            builder.idsByKey_ = std::move(*aliases);
        throw;
    }
}

void MapSchema::setNumericDomain(TypeId id, simfil::Schema::NumericDomain const& value)
{
    requireMutableType(id);
    nlohmann::json json{
        {"encoding", value.encoding},
        {"unit", value.unit},
        {"scale", value.scale},
        {"offset", value.offset},
        {"complete", value.complete}};
    auto numeric = [](auto const& number)
    {
        return std::visit([](auto v) { return nlohmann::json(v); }, number);
    };
    if (value.minimum)
        json["minimum"] = numeric(*value.minimum);
    if (value.maximum)
        json["maximum"] = numeric(*value.maximum);
    for (auto const& range : value.ordinaryRanges)
        json["ordinaryRanges"]
            .push_back({{"minimum", numeric(range.min)}, {"maximum", numeric(range.max)}});
    schema_->builder->schemas_[id].annotations_["x-mapget"]["numericDomain"] = std::move(json);
}
void MapSchema::addNamedValue(TypeId id, simfil::Schema::NamedValue const& value)
{
    requireMutableType(id);
    static constexpr std::string_view roles[]{"constant", "special", "flag"};
    static constexpr std::string_view
        meanings[]{"ordinary", "unknown", "variable", "unbounded", "saturated"};
    if (unsigned(value.role) >= std::size(roles) ||
        unsigned(value.meaning) >= std::size(meanings) || value.name.empty())
        throw std::invalid_argument("Invalid named-value declaration.");
    nlohmann::json json{
        {"name", value.name},
        {"value", std::visit([](auto v) { return nlohmann::json(v); }, value.value)},
        {"role", roles[unsigned(value.role)]},
        {"meaning", meanings[unsigned(value.meaning)]},
        {"deprecated", value.deprecated},
        {"removed", value.removed}};
    if (value.rawValue)
        json["rawValue"] =
            std::visit([](auto raw) { return nlohmann::json(raw); }, *value.rawValue);
    auto& values = schema_->builder->schemas_[id].annotations_["x-mapget"]["namedValues"];
    if (!values.is_null())
        for (auto const& old : values)
            if (old.at("name") == json.at("name")) {
                if (old != json)
                    throw std::invalid_argument("Conflicting named-value declaration.");
                return;
            }
    values.push_back(std::move(json));
}
void MapSchema::setBitWidth(TypeId id, uint8_t width)
{
    requireMutableType(id);
    if (width == 0 || width > 64)
        throw std::invalid_argument("Bitmask width must be 1..64.");
    schema_->builder->schemas_[id].annotations_["x-mapget"]["bitWidth"] = width;
}
std::optional<simfil::Schema::NumericDomain> MapSchema::numericDomain(TypeId id) const
{
    auto d = type(id);
    return d ? d->numericDomain() : std::nullopt;
}
void MapSchema::forEachNamedValue(
    TypeId id,
    std::function<void(simfil::Schema::NamedValue const&)> const& fn) const
{
    if (auto d = type(id))
        d->forEachNamedValue(fn);
}
uint8_t MapSchema::bitWidth(TypeId id) const
{
    auto d = type(id);
    return d ? d->bitWidth() : 0;
}

TypeId
MapSchema::Impl::import(schema_detail::SchemaBuilder const& source, size_t layerIndex, TypeId root)
{
    std::vector<TypeId> remap(source.schemas_.size());
    auto importedCount =
        std::ranges::count_if(source.schemas_, [](auto const& value) { return value.id_ != 0; });
    if (importedCount > simfil::MaxTypeId + 1u - builder->schemas_.size())
        throw std::length_error("Combined MapSchema exceeds its 16-bit type namespace.");
    auto start = builder->schemas_.size();
    auto aliasesBefore = builder->idsByKey_;
    auto& layer = layers.at(layerIndex);
    auto localBefore = layer.buildAliases;
    try {
        for (size_t i = 1; i < source.schemas_.size(); ++i) {
            if (!source.valid(TypeId(i)))
                continue;
            auto const& d = source.schemas_[i];
            // Never reinterpret transported local IDs as graph-global IDs.
            remap[i] = builder->allocate(d.kind_, {}, {}, d.entry_.metaType_);
        }
        auto mapped = [&](TypeId id)
        {
            if (id == 0)
                return TypeId{0};
            if (id >= remap.size() || !remap[id])
                throw std::invalid_argument("Dangling type reference in imported schema.");
            return remap[id];
        };
        for (size_t i = 1; i < source.schemas_.size(); ++i)
            if (remap[i]) {
                auto copy = source.schemas_[i];
                schema_detail::SchemaBuilder::validationSchemas(
                    copy.annotations_,
                    [&](nlohmann::json& value, auto const&)
                    {
                        auto reference = value.at("$ref").get<std::string>();
                        constexpr std::string_view prefix = "#/definitions/";
                        if (!reference.starts_with(prefix))
                            throw std::invalid_argument("Unresolved schema validation reference.");
                        uint32_t id = 0;
                        auto digits = std::string_view(reference).substr(prefix.size());
                        auto [end, error] =
                            std::from_chars(digits.data(), digits.data() + digits.size(), id);
                        if (error != std::errc{} || end != digits.data() + digits.size() ||
                            id > simfil::MaxTypeId)
                            throw std::invalid_argument("Invalid schema validation reference.");
                        value["$ref"] = "#/definitions/" + std::to_string(mapped(TypeId(id)));
                    });
                copy.id_ = remap[i];
                copy.entry_.id_ = remap[i];
                for (auto& [_, children] : copy.childSchemas_)
                    for (auto& c : children)
                        c = mapped(c);
                for (auto& c : copy.elementSchemas_)
                    c = mapped(c);
                for (auto& c : copy.alternatives_)
                    c = mapped(c);
                auto owners = std::move(copy.attributeOwners_);
                copy.attributeOwners_.clear();
                builder->entriesById_[remap[i]] = copy.entry_;
                builder->schemas_[remap[i]] = std::move(copy);
                for (auto& owner : owners) {
                    owner.attributeSchema_ = remap[i];
                    owner.featureLayer_ = layer.name;
                    builder->registerAttributeOwner(remap[i], std::move(owner));
                }
            }
        for (auto const& [key, id] : source.idsByKey_) {
            auto [it, inserted] = layer.buildAliases.emplace(key, mapped(id));
            if (!inserted && it->second != mapped(id))
                throw std::invalid_argument("Conflicting layer-local schema key: " + key);
        }
    }
    catch (...) {
        builder->schemas_.resize(start);
        builder->entriesById_.resize(start);
        builder->idsByKey_ = std::move(aliasesBefore);
        layer.buildAliases = std::move(localBefore);
        throw;
    }
    return root ? remap.at(root) : 0;
}

LayerSchema::Ptr MapSchema::importLayer(
    std::string_view name,
    LayerType type,
    nlohmann::json const& json,
    std::span<FeatureTypeInfo const> features)
{
    requireMutable();
    schema_detail::SchemaBuilder::validateInput(json);
    auto inputs = nlohmann::json::array();
    for (auto const& info : features) {
        // Reuse the metadata validator, without persisting its JSON representation.
        inputs.push_back(FeatureTypeInfo::fromJson(info.toJson()).toJson());
    }
    auto fingerprint = picosha2::hash256_hex_string(json.dump() + inputs.dump());
    if (auto found = layer(name)) {
        auto const& old = schema_->layerAt(found->addr().index());
        if (old.type == type && old.importedFingerprint == fingerprint)
            return found;
        throw std::invalid_argument(
            "Conflicting declarations for schema layer '" + std::string(name) + "'.");
    }
    schema_detail::SchemaBuilder imported;
    TypeId importedRoot = 0;
    if (!json.is_null())
        importedRoot = schema_detail::LayerSchemaCompiler(imported, json).buildAll();
    auto start = schema_->builder->schemas_.size();
    auto result = newLayer(name, type);
    auto index = result->addr().index();
    try {
        auto& target = schema_->layers[index];
        target.importedFingerprint = std::move(fingerprint);
        target.root = schema_->import(imported, index, importedRoot);
        auto emitted = schema_->builder->featureRoots(target.root);
        for (auto const& info : features) {
            auto root = result->schemaId(LayerSchema::featureKey(info.name_));
            bool produced = emitted.empty() || std::ranges::find(emitted, root) != emitted.end();
            if (root) {
                auto const& annotations = schema_->builder->schemas_[root].annotations_;
                if (auto metadata = schema_detail::mapgetMetadata(annotations);
                    metadata && metadata->contains("produced"))
                    produced = produced && metadata->at("produced").get<bool>();
            }
            result->addFeatureType(info, root, produced);
        }
        if (!target.root) {
            target.root = result->addSchema(Kind::Object);
            result->setOpen(target.root, true);
        }
    }
    catch (...) {
        schema_->builder->schemas_.resize(start);
        schema_->builder->entriesById_.resize(start);
        schema_->layers.pop_back();
        // Root registration is deferred to finalization; no dangling root survives rollback.
        throw;
    }
    return result;
}

TypeId MapSchema::addJsonSchema(nlohmann::json const& json)
{
    requireMutable();
    schema_detail::SchemaBuilder::validateInput(json);
    auto start = schema_->builder->schemas_.size();
    auto aliases = schema_->builder->idsByKey_;
    try {
        return schema_detail::LayerSchemaCompiler(*schema_->builder, json).append();
    }
    catch (...) {
        schema_->builder->schemas_.resize(start);
        schema_->builder->entriesById_.resize(start);
        schema_->builder->idsByKey_ = std::move(aliases);
        throw;
    }
}

LayerSchema::LayerSchema(simfil::detail::mp_key key) : ModelNodeBase(key) {}
LayerSchema::LayerSchema(
    simfil::ModelConstPtr owner,
    ModelNodeAddress address,
    simfil::detail::mp_key key)
    : ModelNodeBase(std::move(owner), address, key)
{
}
std::shared_ptr<MapSchema const> LayerSchema::mapSchema() const
{
    return std::static_pointer_cast<MapSchema const>(model_);
}
MapSchema& LayerSchema::mutableOwner()
{
    auto& owner = *const_cast<MapSchema*>(mapSchema().get());
    owner.requireMutable();
    return owner;
}
std::string_view LayerSchema::name() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).name;
}
LayerType LayerSchema::layerType() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).type;
}
void LayerSchema::setRootSchema(TypeId root)
{
    auto& owner = mutableOwner();
    if (root)
        owner.schema_->requireType(root);
    owner.schema_->layerAt(addr_.index()).root = root;
}
TypeId LayerSchema::rootSchema() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).root;
}
std::span<FeatureTypeInfo const> LayerSchema::featureTypeInfos() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).features;
}
FeatureTypeInfo const* LayerSchema::getTypeInfo(std::string_view name, bool throwIfMissing) const
{
    for (auto const& feature : featureTypeInfos())
        if (feature.name_ == name)
            return &feature;
    if (throwIfMissing)
        throw std::invalid_argument(
            "Unknown feature type '" + std::string(name) + "' in schema layer '" +
            std::string(this->name()) + "'.");
    return nullptr;
}
bool LayerSchema::producesFeatureType(std::string_view name) const
{
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    for (size_t i = 0; i < layer.features.size(); ++i)
        if (layer.features[i].name_ == name)
            return layer.produced[i];
    if (layer.features.empty()) {
        auto types = featureTypes();
        return std::ranges::find(types, name) != types.end();
    }
    return false;
}
std::vector<std::string> LayerSchema::featureTypes() const
{
    std::vector<std::string> result;
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    for (size_t i = 0; i < layer.features.size(); ++i)
        if (layer.produced[i])
            result.push_back(layer.features[i].name_);
    if (layer.features.empty()) {
        auto const& graph = *mapSchema()->schema_;
        auto roots = graph.producedRoots(layer);
        auto appendAliases = [&](auto const& aliases)
        {
            constexpr std::string_view prefix = "Feature:";
            for (auto const& [key, id] : aliases)
                if (key.starts_with(prefix) && key.size() > prefix.size() &&
                    std::ranges::find(roots, id) != roots.end())
                    result.emplace_back(std::string_view(key).substr(prefix.size()));
        };
        // Descriptive type names need not match feature IDs (and may be absent).
        // Only this layer's produced Feature aliases establish those names.
        if (graph.builder)
            appendAliases(layer.buildAliases);
        else
            appendAliases(layer.aliases);
    }
    return result;
}
void LayerSchema::addFeatureType(FeatureTypeInfo info, TypeId root, bool produced)
{
    auto& owner = mutableOwner();
    info = FeatureTypeInfo::fromJson(info.toJson());
    auto& layer = owner.schema_->layerAt(addr_.index());
    if (!root)
        root = schemaId(featureKey(info.name_));
    if (root)
        owner.schema_->requireType(root);
    for (size_t i = 0; i < layer.features.size(); ++i)
        if (layer.features[i].name_ == info.name_) {
            if (layer.features[i].toJson() != info.toJson() || layer.produced[i] != produced ||
                (root && layer.featureRoots[i] != root))
                throw std::invalid_argument(
                    "Conflicting feature identity declaration: " + info.name_);
            return;
        }
    if (!root) {
        root = addSchema(FeatureKind, featureKey(info.name_), "Feature");
        setOpen(root, true);
        setTypeName(root, info.name_);
    }
    registerSchemaKey(featureKey(info.name_), root);
    layer.features.push_back(std::move(info));
    layer.featureRoots.push_back(root);
    layer.produced.push_back(produced);
}
TypeId LayerSchema::addSchema(Kind kind, std::string key, std::string metaType, std::string pointer)
{
    auto& owner = mutableOwner();
    auto id = owner.addSchema(kind, {}, std::move(metaType), std::move(pointer));
    owner.schema_->builder->schemas_[id].entry_.key_ = key;
    owner.schema_->builder->entriesById_[id].key_ = key;
    if (!key.empty())
        registerSchemaKey(std::move(key), id);
    return id;
}
void LayerSchema::registerSchemaKey(std::string key, TypeId id)
{
    auto& owner = mutableOwner();
    owner.schema_->requireType(id);
    auto& aliases = owner.schema_->layerAt(addr_.index()).buildAliases;
    auto [it, inserted] = aliases.emplace(std::move(key), id);
    if (!inserted && it->second != id)
        throw std::invalid_argument("Conflicting layer-local schema key: " + it->first);
}
TypeId LayerSchema::schemaId(std::string_view key) const
{
    auto owner = mapSchema();
    auto const& layer = owner->schema_->layerAt(addr_.index());
    if (owner->schema_->builder) {
        if (auto it = layer.buildAliases.find(key); it != layer.buildAliases.end())
            return it->second;
    }
    else if (auto it = layer.aliases.find(key); it != layer.aliases.end())
        return it->second;
    return owner->findSchema(key);
}
TypeId LayerSchema::addJsonSchema(nlohmann::json const& json)
{
    auto& owner = mutableOwner();
    auto& graph = *owner.schema_;
    // Compiler resolves converter-shared keys globally, with layer aliases taking precedence.
    auto globals = std::move(graph.builder->idsByKey_);
    graph.builder->idsByKey_ = globals;
    for (auto const& [key, id] : graph.layerAt(addr_.index()).buildAliases)
        graph.builder->idsByKey_[key] = id;
    try {
        auto id = owner.addJsonSchema(json);
        auto& aliases = graph.layerAt(addr_.index()).buildAliases;
        for (auto const& [key, value] : graph.builder->idsByKey_)
            if (!globals.contains(key) || globals.at(key) != value)
                aliases[key] = value;
        graph.builder->idsByKey_ = std::move(globals);
        return id;
    }
    catch (...) {
        graph.builder->idsByKey_ = std::move(globals);
        throw;
    }
}
void LayerSchema::finalize()
{
    if (!mapSchema()->finalized())
        mutableOwner().finalize();
}
LayerSchema::Ptr LayerSchema::fromJsonSchema(nlohmann::json json)
{
    if (json.is_null())
        return {};
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->importLayer("legacy", LayerType::Features, json);
    owner->finalize();
    return layer;
}

std::vector<TypeId> MapSchema::Impl::producedRoots(Layer const& layer) const
{
    std::vector<TypeId> result;
    if (!layer.features.empty()) {
        for (size_t i = 0; i < layer.features.size(); ++i)
            if (layer.produced[i])
                result.push_back(layer.featureRoots[i]);
        return result;
    }
    // Schema-only readers may discover produced roots without inventing feature ID
    // compositions. This does not make such a graph a valid feature creation contract.
    std::vector<TypeId> pending{layer.root};
    std::vector<bool> seen(builder ? builder->schemas_.size() : types.size());
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!valid(id) || seen[id])
            continue;
        seen[id] = true;
        if (metaType(id) == "Feature" ||
            (builder ? builder->schemas_[id].kind_ : pool.type(id)->kind()) ==
                LayerSchema::FeatureKind)
        {
            auto extra = builder ?
                builder->schemas_[id].annotations_ :
                annotation(types[id].annotations);
            auto metadata = schema_detail::mapgetMetadata(extra);
            if (!metadata || metadata->value("produced", true))
                result.push_back(id);
            continue;
        }
        if (builder) {
            auto const& domain = builder->schemas_[id];
            pending.insert(pending.end(), domain.alternatives_.begin(), domain.alternatives_.end());
            pending.insert(
                pending.end(),
                domain.elementSchemas_.begin(),
                domain.elementSchemas_.end());
        }
        else {
            auto const& domain = *pool.type(id);
            auto alternatives = domain.alternatives();
            pending.insert(pending.end(), alternatives.begin(), alternatives.end());
            domain.forEachElementSchema([&](auto child) { pending.push_back(child); });
        }
    }
    std::ranges::sort(result);
    return result;
}

void MapSchema::Impl::prepareQueryRoots()
{
    std::set<std::pair<TypeId, TypeId>> contexts;
    std::map<TypeId, std::pair<std::string, std::string>> featureNames;
    for (auto const& layer : layers)
        for (size_t i = 0; i < layer.features.size(); ++i)
            featureNames
                .emplace(layer.featureRoots[i], std::pair{layer.name, layer.features[i].name_});
    for (auto const& layer : layers)
        for (auto feature : producedRoots(layer)) {
            // Legacy schema-only inputs can declare Feature roots without an ID
            // composition. Query contexts still need their layer/name ownership.
            auto const& source = builder->schemas_[feature];
            auto metadata = schema_detail::mapgetMetadata(source.annotations_);
            auto name = schema_detail::metadataString(metadata, "featureType");
            featureNames.try_emplace(
                feature,
                layer.name,
                name.empty() ? source.typeName_ : std::move(name));
            std::vector<TypeId> pending{feature};
            std::vector<bool> seen(builder->schemas_.size());
            while (!pending.empty()) {
                auto id = pending.back();
                pending.pop_back();
                if (!builder->valid(id) || seen[id])
                    continue;
                seen[id] = true;
                auto const& d = builder->schemas_[id];
                if (d.entry_.metaType_ == "Attribute")
                    contexts.emplace(feature, id);
                for (auto const& [_, children] : d.childSchemas_)
                    pending.insert(pending.end(), children.begin(), children.end());
                pending.insert(pending.end(), d.elementSchemas_.begin(), d.elementSchemas_.end());
                pending.insert(pending.end(), d.alternatives_.begin(), d.alternatives_.end());
            }
        }
    if (contexts.empty())
        return;
    for (auto const& [context, _] : queryRoots)
        contexts.erase(context);
    if (contexts.empty())
        return;
    auto string = pool.primitive(Kind::String);
    auto integer = pool.primitive(Kind::Int);
    auto boolean = pool.primitive(Kind::Bool);
    for (auto [feature, attribute] : contexts) {
        auto copy = builder->schemas_[attribute];
        auto id = builder->allocate(LayerSchema::AttributeKind, {}, {}, "Attribute");
        copy.id_ = id;
        copy.entry_ = builder->entriesById_[id];
        copy.queryRoot_ = true;
        auto const& [featureLayer, featureName] = featureNames.at(feature);
        std::erase_if(
            copy.attributeOwners_,
            [&](auto const& owner)
            {
                return owner.featureType_ != featureName ||
                    (!owner.featureLayer_.empty() && owner.featureLayer_ != featureLayer);
            });
        copy.attributeTypeCode_.clear();
        if (!copy.attributeOwners_.empty()) {
            auto const& name = copy.attributeOwners_.front().attributeName_;
            if (std::ranges::all_of(
                    copy.attributeOwners_,
                    [&](auto const& owner) { return owner.attributeName_ == name; }))
                copy.attributeTypeCode_ = name;
        }
        // Only overlay edges are stored; the payload is referenced by TypeData::base.
        copy.directFields_.clear();
        copy.childSchemas_.clear();
        copy.required_.clear();
        copy.contextFields_.clear();
        copy.directEnumSymbols_.clear();
        copy.enumValues_.clear();
        copy.elementSchemas_.clear();
        // A named Attribute may describe several physical branches. Its query
        // context must retain those alternatives rather than hide their fields.
        builder->schemas_[id] = std::move(copy);
        auto overlay = [&](std::string_view name, TypeId child)
        {
            builder->addDirectField(id, name);
            builder->addChild(id, name, child);
            builder->schemas_[id].required_[std::string(name)] = true;
            builder->schemas_[id].contextFields_.emplace(name);
        };
        overlay("$name", string);
        overlay("$layer", string);
        overlay("$feature", feature);
        overlay("$attributeIndex", integer);
        overlay("$validityIndex", integer);
        overlay("$validityCount", integer);
        overlay("$hasValidity", boolean);
        queryRoots.emplace(std::pair{feature, attribute}, id);
    }
}

void MapSchema::Impl::compileScalarMetadata(
    TypeId id,
    schema_detail::SchemaBuilder::LogicalSchema const& source,
    nlohmann::json& extra)
{
    auto& generic = static_cast<simfil::SchemaPool&>(pool);
    auto kindName = simfil::Schema::kindNameId(source.kind_);
    auto explicitMetadata = schema_detail::mapgetMetadata(extra);
    if (source.composition_ == Composition::None &&
        (kindName == simfil::Schema::kindNameId(Kind::Int) ||
         kindName == simfil::Schema::kindNameId(Kind::Float)) &&
        (!explicitMetadata || !explicitMetadata->contains("numericDomain")))
    {
        auto inferred = nlohmann::json::object();
        for (auto key : {"minimum", "maximum"})
            if (auto bound = extra.find(key); bound != extra.end() && bound->is_number())
                inferred[key] = *bound;
        if (!inferred.empty())
            extra["x-mapget"]["numericDomain"] = std::move(inferred);
    }
    auto bits = simfil::Schema::affinities(source.kind_);
    auto mask = simfil::valueTypeAffinity(simfil::ValueType::Bitmask);
    auto isMask = source.composition_ == Composition::None && (bits & mask) &&
        !(bits & ~(mask | simfil::valueTypeAffinity(simfil::ValueType::Null)));
    auto metadata = extra.find("x-mapget");
    if (metadata == extra.end()) {
        if (isMask)
            generic.setBitWidth(id, 64);
        return;
    }
    if (auto declarations = metadata->find("relationDeclarations"); declarations != metadata->end())
    {
        if (!declarations->is_array())
            throw std::invalid_argument("relationDeclarations must be an array.");
        for (auto& relation : *declarations) {
            if (!relation.contains("complete") && relation.contains("coverage"))
                relation["complete"] = relation.at("coverage") == "complete";
            relation.erase("coverage");
            relation.erase("incoming");
        }
    }
    if (metadata->contains("bitWidth") && !isMask)
        throw std::invalid_argument("bitWidth requires a concrete raw-bitmask type.");
    auto width = metadata->value("bitWidth", isMask ? 64u : 0u);
    if ((isMask && !width) || width > 64)
        throw std::invalid_argument("Bitmask width must be 1..64.");
    if (isMask)
        generic.setBitWidth(id, uint8_t(width));
    metadata->erase("bitWidth");
    auto numericValue = [](nlohmann::json const& value) -> simfil::Schema::NumericValue
    {
        if (value.is_number_unsigned())
            return value.get<uint64_t>();
        if (value.is_number_integer())
            return value.get<int64_t>();
        if (value.is_number_float())
            return value.get<double>();
        throw std::invalid_argument("Numeric domain metadata must be numeric.");
    };
    if (auto values = metadata->find("namedValues"); values != metadata->end()) {
        if (!values->is_array())
            throw std::invalid_argument("namedValues must be an array.");
        std::set<std::string> seen;
        for (auto const& value : *values) {
            simfil::Schema::NamedValue named;
            auto name = value.at("name").get<std::string>();
            if (name.empty() || !seen.insert(name).second)
                throw std::invalid_argument("Duplicate or empty named-value name.");
            named.name = name;
            auto const& scalar = value.at("value");
            if (scalar.is_boolean())
                named.value = scalar.get<bool>();
            else if (scalar.is_string())
                named.value = std::string_view(scalar.get_ref<std::string const&>());
            else
                std::visit([&](auto number) { named.value = number; }, numericValue(scalar));
            auto role = value.value("role", "constant");
            if (role != "constant" && role != "special" && role != "flag")
                throw std::invalid_argument("Invalid named-value role.");
            named.role = role == "flag" ?
                simfil::Schema::NamedValue::Role::Flag :
                role == "special" ?
                simfil::Schema::NamedValue::Role::Special :
                simfil::Schema::NamedValue::Role::Constant;
            std::array<std::string_view, 5>
                meanings{"ordinary", "unknown", "variable", "unbounded", "saturated"};
            auto meaning = value.value("meaning", "ordinary");
            auto found = std::ranges::find(meanings, meaning);
            if (found == meanings.end())
                throw std::invalid_argument("Invalid named-value meaning.");
            named.meaning = simfil::Schema::NamedValue::Meaning(found - meanings.begin());
            named.deprecated = value.value("deprecated", false);
            named.removed = value.value("removed", false);
            if (auto raw = value.find("rawValue"); raw != value.end()) {
                if (raw->is_number_unsigned())
                    named.rawValue = raw->get<uint64_t>();
                else if (raw->is_number_integer())
                    named.rawValue = raw->get<int64_t>();
                else
                    throw std::invalid_argument("Named-value rawValue must be an integer.");
            }
            generic.addNamedValue(id, named);
        }
        metadata->erase("namedValues");
    }
    if (auto numeric = metadata->find("numericDomain"); numeric != metadata->end()) {
        simfil::Schema::NumericDomain domain;
        auto encoding = numeric->value("encoding", ""), unit = numeric->value("unit", "");
        domain.encoding = encoding;
        domain.unit = unit;
        if (auto minimum = numeric->find("minimum"); minimum != numeric->end())
            domain.minimum = numericValue(*minimum);
        if (auto maximum = numeric->find("maximum"); maximum != numeric->end())
            domain.maximum = numericValue(*maximum);
        domain.complete = numeric->value("complete", false);
        domain.scale = numeric->value("scale", 1.);
        domain.offset = numeric->value("offset", 0.);
        std::vector<simfil::Schema::NumericRange> ranges;
        if (auto input = numeric->find("ordinaryRanges"); input != numeric->end())
            for (auto const& range : *input)
                ranges.push_back(
                    {numericValue(range.at("minimum")), numericValue(range.at("maximum"))});
        domain.ordinaryRanges = ranges;
        generic.setNumericDomain(id, domain);
        metadata->erase("numericDomain");
    }
    if (metadata->empty())
        extra.erase("x-mapget");
}

void MapSchema::Impl::compile()
{
    for (auto& layer : layers)
        if (!layer.root) {
            std::vector<TypeId> roots;
            for (size_t i = 0; i < layer.featureRoots.size(); ++i)
                if (layer.produced[i])
                    roots.push_back(layer.featureRoots[i]);
            if (roots.size() == 1)
                layer.root = roots.front();
            else {
                layer.root =
                    builder->allocate(roots.empty() ? Kind::Unknown : Kind::Union, {}, {}, {});
                if (!roots.empty()) {
                    builder->schemas_[layer.root].composition_ = Composition::AnyOf;
                    builder->schemas_[layer.root].alternatives_ = std::move(roots);
                }
            }
        }
    prepareQueryRoots();
    auto& generic = static_cast<simfil::SchemaPool&>(pool);
    types.resize(builder->schemas_.size());
    for (size_t id = 1; id < builder->schemas_.size(); ++id) {
        if (!builder->valid(TypeId(id)) || generic.addSchema(builder->schemas_[id].kind_) != id)
            throw std::invalid_argument("MapSchema has a hole in its type namespace.");
    }
    for (auto [context, query] : queryRoots)
        generic.setBaseSchema(query, context.second);
    for (size_t id = 1; id < builder->schemas_.size(); ++id) {
        auto const& source = builder->schemas_[id];
        TypeData row{};
        row.flags = uint32_t(source.queryRoot_) << 3;
        row.key = text(source.entry_.key_);
        row.pointer = text(source.entry_.jsonPointer_);
        row.metaType = text(source.entry_.metaType_);
        row.zserioType = text(source.zserioType_);
        row.attributeType = text(source.attributeType_);
        row.attributeCode = text(source.attributeTypeCode_);
        generic.setTypeName(TypeId(id), source.typeName_);
        generic.setOpen(TypeId(id), source.open_);
        generic.setNullable(TypeId(id), source.nullable_);
        generic.setComposition(TypeId(id), source.composition_);
        for (auto const& name : source.directFields_) {
            auto children = source.childSchemas_.find(name);
            if (children == source.childSchemas_.end() || children->second.empty())
                generic.addFieldSchema(
                    TypeId(id),
                    name,
                    simfil::NoTypeId,
                    source.multimapFields_.contains(name));
            else
                for (auto child : children->second)
                    generic.addFieldSchema(
                        TypeId(id),
                        name,
                        child,
                        source.multimapFields_.contains(name));
            if (auto it = source.required_.find(name); it != source.required_.end())
                generic.setFieldRequired(TypeId(id), name, it->second);
            if (source.contextFields_.contains(name))
                generic.setFieldRole(TypeId(id), name, simfil::Schema::FieldRole::Context);
        }
        // Legacy imports also keep the implicit alias in childSchemas_, but only
        // an actual declared field may override the native lookup-only alias.
        if (simfil::Schema::kindNameId(source.kind_) ==
                simfil::Schema::kindNameId(LayerSchema::FeatureKind) &&
            source.childSchemas_.contains("properties") &&
            std::ranges::find(source.directFields_, "attributes") == source.directFields_.end())
            generic.addFieldAlias(TypeId(id), "attributes", "properties");
        for (auto child : source.elementSchemas_)
            generic.addElementSchema(TypeId(id), child);
        for (auto child : source.alternatives_)
            generic.addAlternative(TypeId(id), child);
        for (auto const& symbol : source.directEnumSymbols_)
            generic.addEnumSymbol(TypeId(id), symbol);
        for (auto const& value : source.enumValues_)
            generic.addEnumValue(TypeId(id), value);
        row.owners.begin = owners.size();
        for (auto const& owner : source.attributeOwners_)
            owners.push_back(
                {intern(owner.featureType_),
                 intern(owner.attributeLayerName_),
                 intern(owner.attributeName_),
                 owner.featureLayer_.empty() ? StringId{0} : intern(owner.featureLayer_)});
        row.owners.count = owners.size() - row.owners.begin;
        auto extra = source.annotations_.is_null() ? nlohmann::json::object() : source.annotations_;
        compileScalarMetadata(TypeId(id), source, extra);
        row.annotations = annotation(extra);
        types[id] = row;
    }
    for (auto const& [key, id] : builder->idsByKey_)
        aliases.push_back({text(key), id, 0});
    for (size_t i = 0; i < layers.size(); ++i) {
        for (auto const& [key, id] : layers[i].buildAliases)
            aliases.push_back({text(key), id, uint16_t(i + 1)});
        layers[i].buildAliases.clear();
        layers[i].importedFingerprint.clear();
    }
    for (size_t i = 0; i < descriptorKeys.size(); ++i)
        descriptorKeys[i] = intern(std::array<std::string_view, 12>{
            "name",
            "layerType",
            "root",
            "featureTypes",
            "id",
            "kind",
            "typename",
            "fields",
            "items",
            "alternatives",
            "enum",
            "open"}[i]);
    builder.reset();
    pool.finalizeSchema();
    textIndex.clear();
    textIndex.rehash(0);
}

void MapSchema::Impl::index()
{
    entries.clear();
    globalAliases.clear();
    for (size_t i = 0; i < descriptorKeys.size(); ++i) {
        descriptorKeys[i] = pool.strings()->get(std::array<std::string_view, 12>{
            "name",
            "layerType",
            "root",
            "featureTypes",
            "id",
            "kind",
            "typename",
            "fields",
            "items",
            "alternatives",
            "enum",
            "open"}[i]);
        if (!descriptorKeys[i])
            throw std::invalid_argument("Missing schema descriptor vocabulary.");
    }
    for (auto& layer : layers)
        layer.aliases.clear();
    entries.resize(types.size());
    for (auto const& alias : aliases) {
        auto& index = alias.layer ? layers.at(alias.layer - 1).aliases : globalAliases;
        auto [it, inserted] = index.emplace(text(alias.key), alias.id);
        if (!inserted && it->second != alias.id)
            throw std::invalid_argument("Conflicting stored schema aliases.");
    }
    for (size_t id = 1; id < types.size(); ++id) {
        auto const& row = types[id];
        entries[id] = {TypeId(id), text(row.key), text(row.pointer), text(row.metaType)};
    }
    // Resolve forward contracts once for each produced layer. Referenced-only
    // identity types do not make that layer a source of incoming relations.
    size_t work = 0;
    for (auto& layer : layers) {
        if (layer.relationsDeclared) {
            layer.relationsByTarget.clear();
            std::set<std::pair<std::string, std::string>> names;
            for (size_t i = 0; i < layer.relations.size(); ++i) {
                auto const& value = layer.relations[i];
                if (!pool.layer(layer.name)->producesFeatureType(value.ownerType_) ||
                    value.name_.empty() || value.targetTypes_.empty() ||
                    !names.emplace(value.ownerType_, value.name_).second ||
                    unsigned(value.locality_) >
                        unsigned(LayerSchema::RelationDeclaration::Locality::MayCrossContainer))
                    throw std::invalid_argument("Invalid native forward relation declaration.");
                for (auto const& target : value.targetTypes_) {
                    if (target.empty())
                        throw std::invalid_argument("Empty relation target type.");
                    layer.relationsByTarget[target].push_back(i);
                }
            }
            continue;
        }
        layer.relations.clear();
        layer.relationsByTarget.clear();
        layer.relationsKnown = false;
        std::vector<TypeId> pending{layer.root};
        auto produced = producedRoots(layer);
        auto hasInventory = [&](TypeId id)
        {
            if (!valid(id))
                return false;
            auto value = annotation(types[id].annotations);
            auto meta = schema_detail::mapgetMetadata(value);
            return meta && meta->contains("relationDeclarations") &&
                meta->value("relationDeclarationsKnown", true);
        };
        bool layerWide = valid(layer.root) && metaType(layer.root) != "Feature" &&
            simfil::Schema::kindNameId(pool.type(layer.root)->kind()) !=
                simfil::Schema::kindNameId(LayerSchema::FeatureKind) &&
            hasInventory(layer.root);
        layer.relationsKnown = layerWide ||
            (!produced.empty() && std::ranges::all_of(produced, hasInventory));
        pending.insert(pending.end(), produced.begin(), produced.end());
        std::vector<bool> seen(types.size());
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!valid(id) || seen[id])
                continue;
            seen[id] = true;
            ++work;
            if (work > 64 * 1024 * 1024)
                throw std::length_error("MapSchema relation-index work budget exceeded.");
            auto annotations = annotation(types[id].annotations);
            auto meta = annotations.find("x-mapget");
            if (meta != annotations.end())
                if (auto declarations = meta->find("relationDeclarations");
                    declarations != meta->end())
                {
                    if (!declarations->is_array())
                        throw std::invalid_argument("relationDeclarations must be an array.");
                    for (auto const& input : *declarations) {
                        LayerSchema::RelationDeclaration value{
                            input.at("ownerType").get<std::string>(),
                            input.at("name").get<std::string>(),
                            input.at("targetTypes").get<std::vector<std::string>>(),
                            input.value("complete", false)};
                        auto locality = input.value("locality", "unknown");
                        using Locality = LayerSchema::RelationDeclaration::Locality;
                        if (locality == "same-container")
                            value.locality_ = Locality::SameContainer;
                        else if (locality == "may-cross-container")
                            value.locality_ = Locality::MayCrossContainer;
                        else if (locality != "unknown")
                            throw std::invalid_argument("Invalid relation locality.");
                        if (!pool.layer(layer.name)->producesFeatureType(value.ownerType_))
                            continue;
                        if (value.name_.empty() || value.targetTypes_.empty())
                            throw std::invalid_argument("Incomplete forward relation declaration.");
                        auto old = std::ranges::find_if(
                            layer.relations,
                            [&](auto const& d)
                            { return d.ownerType_ == value.ownerType_ && d.name_ == value.name_; });
                        if (old != layer.relations.end()) {
                            if (old->targetTypes_ != value.targetTypes_ ||
                                old->complete_ != value.complete_ ||
                                old->locality_ != value.locality_)
                                throw std::invalid_argument(
                                    "Conflicting forward relation declaration.");
                            continue;
                        }
                        auto index = layer.relations.size();
                        for (auto const& target : value.targetTypes_)
                            layer.relationsByTarget[target].push_back(index);
                        layer.relations.push_back(std::move(value));
                    }
                }
            auto append = [&](TypeId child)
            {
                if (++work > 64 * 1024 * 1024)
                    throw std::length_error("MapSchema relation-index work budget exceeded.");
                pending.push_back(child);
            };
            auto const& domain = *pool.type(id);
            domain.forEachEnumerableField(
                [&](auto, auto children)
                {
                    for (auto child : children)
                        append(child);
                });
            domain.forEachElementSchema(append);
            for (auto child : domain.alternatives())
                append(child);
        }
    }
}

void MapSchema::Impl::validate() const
{
    (void)MapFormat::fromJson(format.toJson());
    std::set<std::string_view> constituentIds;
    for (auto const& [id, format] : constituents) {
        constexpr std::string_view prefix = "mapget-schema:sha256:";
        if (!id.starts_with(prefix) || id.size() != prefix.size() + 64 ||
            id.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos ||
            !constituentIds.insert(id).second)
            throw std::invalid_argument("Invalid or duplicate constituent schema identity.");
        (void)MapFormat::fromJson(format.toJson());
    }
    if (types.size() != pool.simfil::SchemaPool::typeCount() + 1)
        throw std::invalid_argument("Map metadata and generic type namespaces disagree.");
    auto type = [&](TypeId id)
    {
        if (!id || id >= types.size() || !pool.type(id))
            throw std::invalid_argument("Dangling MapSchema type reference.");
    };
    size_t ownerEnd = 0;
    for (size_t id = 1; id < types.size(); ++id) {
        auto const& d = types[id];
        if (d.flags & ~8u)
            throw std::invalid_argument("Invalid map-type flags.");
        for (auto address :
             {d.key, d.pointer, d.metaType, d.zserioType, d.attributeType, d.attributeCode})
            text(address);
        if (d.owners.count && d.owners.begin != ownerEnd)
            throw std::invalid_argument("Attribute owner slices must be canonical.");
        for (auto const& owner : d.owners.in(owners)) {
            name(owner.feature);
            name(owner.layer);
            name(owner.name);
            if (owner.featureLayer)
                name(owner.featureLayer);
        }
        ownerEnd += d.owners.count;
        if (d.annotations)
            (void)annotation(d.annotations);
        if (auto base = pool.baseSchema(TypeId(id))) {
            if (base >= id || !(d.flags & 8) || pool.baseSchema(base) || (types[base].flags & 8) ||
                simfil::Schema::kindNameId(pool.type(base)->kind()) !=
                    simfil::Schema::kindNameId(LayerSchema::AttributeKind))
                throw std::invalid_argument(
                    "Query overlays must refer directly to a non-overlay attribute payload.");
        }
    }
    if (ownerEnd != owners.size())
        throw std::invalid_argument("Unreferenced attribute ownership storage.");
    for (auto const& alias : aliases) {
        type(alias.id);
        text(alias.key);
        if (alias.layer > layers.size())
            throw std::invalid_argument("Unknown schema alias layer.");
    }
    std::set<TypeId> overlays;
    for (auto const& [context, root] : queryRoots) {
        type(context.first);
        type(context.second);
        type(root);
        if (pool.baseSchema(root) != context.second || !overlays.insert(root).second ||
            simfil::Schema::kindNameId(pool.type(context.first)->kind()) !=
                simfil::Schema::kindNameId(LayerSchema::FeatureKind))
            throw std::invalid_argument("Invalid query overlay binding.");
    }
    for (size_t id = 1; id < types.size(); ++id)
        if (bool(pool.baseSchema(TypeId(id))) != overlays.contains(TypeId(id)))
            throw std::invalid_argument(
                "Query overlay bindings must cover every overlay exactly once.");
    std::set<std::string> names;
    for (auto const& layer : layers) {
        if (layer.name.empty() || !names.insert(layer.name).second ||
            unsigned(layer.type) > unsigned(LayerType::SourceData))
            throw std::invalid_argument("Invalid or duplicate schema layer.");
        if (layer.root)
            type(layer.root);
        if (layer.features.size() != layer.featureRoots.size() ||
            layer.features.size() != layer.produced.size())
            throw std::invalid_argument("Invalid feature identity column lengths.");
        std::set<std::string> types;
        for (size_t i = 0; i < layer.features.size(); ++i) {
            auto const& feature = layer.features[i];
            (void)FeatureTypeInfo::fromJson(feature.toJson());
            if (!types.insert(feature.name_).second || layer.produced[i] > 1)
                throw std::invalid_argument("Invalid or duplicate feature declaration.");
            type(layer.featureRoots[i]);
        }
    }
}

void MapSchema::Impl::validatePool() const
{
    // Schema payloads use primitive values only. Never walk arbitrary embedded
    // arrays/objects (including malicious cycles) via ModelPool::checkForErrors.
    auto const& model = std::as_const(pool);
    auto stats = model.memoryUsageStats();
    if (model.objectMemberStorage().size() != size_t(simfil::FirstRegularArrayIndex) ||
        model.arrayMemberStorage().size() != size_t(simfil::FirstRegularArrayIndex) ||
        model.objectMemberStorage().singleton_handle_count() ||
        model.arrayMemberStorage().singleton_handle_count() || stats.objectSchemas.logicalBytes ||
        stats.arraySchemas.logicalBytes || stats.qualifiedScalars.logicalBytes ||
        stats.bitmasks.logicalBytes || stats.schemaBindings.logicalBytes)
        throw std::invalid_argument(
            "MapSchema payload may not contain generic compound/qualified values.");
    if (model.numRoots() != layers.size())
        throw std::invalid_argument("Invalid MapSchema root count.");
    for (size_t i = 0; i < layers.size(); ++i) {
        auto root = model.root(i);
        if (!root || (*root)->addr() != ModelNodeAddress{Layers, i})
            throw std::invalid_argument("MapSchema roots must exactly identify its layer views.");
    }
}

/** Serialize canonical columns only. Derived sets/views/indices are rebuilt once.
 * Every variable length is bounded independently of the envelope byte limit.
 */
template <class S, class T, class F>
void MapSchema::Impl::container(S& s, std::vector<T>& data, size_t limit, size_t& memory, F&& item)
{
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        auto position = s.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(s.adapter(), count, limit, std::true_type{});
        // Every item here has at least one wire byte. Reject truncated lengths
        // and charge all nested allocations before bitsery resizes the container.
        if (count > memory / sizeof(T) ||
            !simfil::detail::check_bitsery_payload_available(s.adapter(), count))
            throw std::length_error("MapSchema container exceeds its input/allocation budget.");
        memory -= count * sizeof(T);
        s.adapter().currentReadPos(position);
    }
    s.container(data, limit, std::forward<F>(item));
}
template <class S>
void MapSchema::Impl::string(S& s, std::string& value, size_t limit, size_t& memory)
{
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        auto position = s.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(s.adapter(), count, limit, std::true_type{});
        if (count > memory || !simfil::detail::check_bitsery_payload_available(s.adapter(), count))
            throw std::length_error("MapSchema text exceeds its input/allocation budget.");
        memory -= count;
        s.adapter().currentReadPos(position);
    }
    s.text1b(value, limit);
}

template <class S>
void MapSchema::Impl::readWrite(S& s, size_t budget)
{
    size_t memory = budget > SIZE_MAX / 4 ? SIZE_MAX : budget * 4;
    auto text = [&](auto& a, std::string& v)
    {
        string(a, v, budget, memory);
    };
    auto vector = [&]<class T>(auto& a, std::vector<T>& values, size_t limit)
    {
        container(
            a,
            values,
            limit,
            memory,
            [](auto& b, T& v)
            {
                if constexpr (std::is_same_v<T, uint8_t>)
                    b.value1b(v);
                else if constexpr (std::is_same_v<T, TypeId>)
                    b.value2b(v);
                else
                    b.object(v);
            });
    };
    auto mapFormat = [&](auto& archive, MapFormat& value)
    {
        text(archive, value.name_);
        text(archive, value.version_);
        std::vector<std::pair<std::string, std::string>> subsystems;
        if constexpr (!simfil::detail::bitsery_input_archive<S>)
            for (auto const& pair : value.subsystems_)
                subsystems.push_back(pair);
        container(
            archive,
            subsystems,
            UINT16_MAX,
            memory,
            [&](auto& a, auto& pair)
            {
                text(a, pair.first);
                text(a, pair.second);
            });
        if constexpr (simfil::detail::bitsery_input_archive<S>)
            for (auto const& pair : subsystems)
                if (!value.subsystems_.emplace(pair).second)
                    throw std::invalid_argument("Duplicate format subsystem.");
    };
    mapFormat(s, format);
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        uint64_t length = 0;
        s.value8b(length);
        if (length > (size_t(simfil::MaxTypeId) + 1) * sizeof(TypeData) || length > budget ||
            length > memory || length % sizeof(TypeData))
            throw std::length_error("MapSchema type column exceeds its namespace/byte budget.");
        memory -= length;
        if (!types.read_payload_from_bitsery(s.adapter(), size_t(length)))
            throw std::invalid_argument("Truncated MapSchema type column.");
    }
    else
        s.object(types);
    vector(s, owners, std::min(MaxEdges, budget / sizeof(OwnerData)));
    vector(s, aliases, std::min(MaxEdges, budget / sizeof(AliasData)));
    container(
        s,
        layers,
        UINT16_MAX,
        memory,
        [&](auto& a, Layer& layer)
        {
            text(a, layer.name);
            a.value4b(layer.type);
            a.value2b(layer.root);
            container(
                a,
                layer.features,
                UINT16_MAX,
                memory,
                [&](auto& b, FeatureTypeInfo& info)
                {
                    text(b, info.name_);
                    container(
                        b,
                        info.uniqueIdCompositions_,
                        UINT16_MAX,
                        memory,
                        [&](auto& c, auto& composition)
                        {
                            container(
                                c,
                                composition,
                                16,
                                memory,
                                [&](auto& d, IdPart& part)
                                {
                                    text(d, part.idPartLabel_);
                                    text(d, part.description_);
                                    d.value4b(part.datatype_);
                                    d.boolValue(part.isSynthetic_);
                                    d.boolValue(part.isOptional_);
                                });
                        });
                });
            vector(a, layer.featureRoots, UINT16_MAX);
            vector(a, layer.produced, UINT16_MAX);
            a.boolValue(layer.relationsKnown);
            container(
                a,
                layer.relations,
                MaxEdges,
                memory,
                [&](auto& b, LayerSchema::RelationDeclaration& relation)
                {
                    text(b, relation.ownerType_);
                    text(b, relation.name_);
                    container(
                        b,
                        relation.targetTypes_,
                        UINT16_MAX,
                        memory,
                        [&](auto& c, std::string& target) { text(c, target); });
                    b.boolValue(relation.complete_);
                    b.value4b(relation.locality_);
                });
            if constexpr (simfil::detail::bitsery_input_archive<S>)
                layer.relationsDeclared = true;
        });
    std::vector<std::array<TypeId, 3>> queries;
    if constexpr (!simfil::detail::bitsery_input_archive<S>)
        for (auto [context, root] : queryRoots)
            queries.push_back({context.first, context.second, root});
    container(
        s,
        queries,
        UINT16_MAX,
        memory,
        [](auto& a, auto& v)
        {
            for (auto& id : v)
                a.value2b(id);
        });
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        for (auto const& v : queries)
            if (!queryRoots.emplace(std::pair{v[0], v[1]}, v[2]).second)
                throw std::invalid_argument("Duplicate query-root binding.");
    }
    container(
        s,
        constituents,
        256,
        memory,
        [&](auto& archive, auto& constituent)
        {
            string(archive, constituent.first, 128, memory);
            mapFormat(archive, constituent.second);
        });
}

void MapSchema::Impl::writeBody(std::ostream& output) const
{
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(output);
    const_cast<Impl*>(this)->readWrite(s, UINT32_MAX);
    std::ostringstream generic(std::ios::binary);
    pool.writeSchemaPayload(generic);
    auto data = generic.str();
    s.text1b(data, UINT32_MAX);
    s.adapter().flush();
    if (!output)
        throw std::runtime_error("Failed to write MapSchema payload.");
}
void MapSchema::Impl::readBody(std::vector<uint8_t> const& input, size_t budget)
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
    builder.reset();
    readWrite(s, budget);
    std::string generic;
    size_t memory = budget;
    string(s, generic, budget, memory);
    if (s.adapter().error() != bitsery::ReaderError::NoError ||
        !s.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Truncated, malformed or trailing MapSchema data.");
    pool.readSchemaPayload(
        std::span(reinterpret_cast<uint8_t const*>(generic.data()), generic.size()),
        budget);
}

void MapSchema::finalize()
{
    if (finalized())
        return;
    requireMutable();
    // After packing has started, failures poison only this unpublished builder.
    // Callers retry getOrBuild with a fresh graph, never publish partially frozen state.
    schema_->compile();
    schema_->validate();
    schema_->index();
    for (auto const& name : layerNames())
        addRoot(layer(name));
    schema_->validatePool();
    std::ostringstream body(std::ios::binary);
    schema_->writeBody(body);
    schema_->identity = "mapget-schema:sha256:" + picosha2::hash256_hex_string(body.str());
    ModelPool::freeze();
    schema_->frozen = true;
    schema_->summarize();
}
tl::expected<void, simfil::Error> MapSchema::write(std::ostream& output)
{
    return std::as_const(*this).write(output);
}
tl::expected<void, simfil::Error> MapSchema::write(std::ostream& output) const
{
    try {
        if (!finalized())
            throw std::logic_error("Cannot serialize an unfinished MapSchema.");
        std::ostringstream data(std::ios::binary);
        schema_->writeBody(data);
        auto payload = data.str();
        bitsery::Serializer<bitsery::OutputStreamAdapter> s(output);
        uint32_t magic = 0x4843534d;  // MSCH
        uint32_t version = Impl::CodecVersion;
        s.value4b(magic);
        s.value4b(version);
        auto id = schema_->identity;
        s.text1b(id, 128);
        s.text1b(payload, UINT32_MAX);
        s.adapter().flush();
        if (!output)
            throw std::runtime_error("Failed to write MapSchema envelope.");
        return {};
    }
    catch (std::exception const& e) {
        return tl::unexpected(simfil::Error{simfil::Error::EncodeDecodeError, e.what()});
    }
}
MapSchema::ConstPtr
MapSchema::read(std::span<uint8_t const> bytes, std::string_view expectedId, size_t maxBytes)
{
    if (bytes.size() > maxBytes || bytes.size() < 8)
        throw std::length_error("MapSchema artifact exceeds its byte budget or is truncated.");
    std::vector<uint8_t> input(bytes.begin(), bytes.end());
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
    uint32_t magic = 0, version = 0;
    s.value4b(magic);
    s.value4b(version);
    if (magic != 0x4843534d || version != Impl::CodecVersion)
        throw std::invalid_argument("Unsupported MapSchema binary format.");
    std::string identity, payload;
    size_t memory = bytes.size();
    Impl::string(s, identity, 128, memory);
    Impl::string(s, payload, maxBytes, memory);
    if (s.adapter().error() != bitsery::ReaderError::NoError ||
        !s.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Malformed MapSchema envelope.");
    if (!expectedId.empty() && identity != expectedId)
        throw std::invalid_argument("MapSchema identity does not match the requested artifact.");
    if (identity != "mapget-schema:sha256:" + picosha2::hash256_hex_string(payload))
        throw std::invalid_argument("MapSchema content checksum mismatch.");
    // Every input is verified before consulting the cache; a warm artifact must
    // never conceal a truncated body or a wrong expected identity.
    return getOrBuild(
        identity,
        [identity, payload = std::move(payload), maxBytes]
        {
            auto result = std::make_shared<MapSchema>();
            result->schema_
                ->readBody(std::vector<uint8_t>(payload.begin(), payload.end()), maxBytes);
            result->schema_->validate();
            result->schema_->validatePool();
            result->schema_->index();
            // Reject noncanonical identities, hidden data and duplicate dictionary IDs.
            std::ostringstream canonical(std::ios::binary);
            result->schema_->writeBody(canonical);
            if (canonical.str() != payload)
                throw std::invalid_argument("Noncanonical MapSchema payload.");
            result->schema_->identity = identity;
            result->ModelPool::freeze();
            result->schema_->frozen = true;
            result->schema_->summarize();
            return result;
        });
}

MapSchema::ConstPtr MapSchema::getOrBuild(
    std::string key,
    std::function<ConstPtr()> builder,
    std::function<bool()> cancel)
{
    // Four distinct builds may run concurrently; no second unbounded work queue.
    // Workers retain the cache owner so process-static teardown cannot free a
    // mutex beneath detached construction. Completed entries hold only weak owners.
    static auto cache = std::make_shared<Impl::BuildCache>();
    auto& activeKeys = Impl::activeBuildKeys();
    if (key.empty() || !builder)
        throw std::invalid_argument("Schema construction requires a key and builder.");
    if (std::ranges::find(activeKeys, key) != activeKeys.end())
        throw std::logic_error("Reentrant construction of the same schema artifact.");
    bool nested = !activeKeys.empty();
    std::shared_future<ConstPtr> future;
    for (;;) {
        if (cancel && cancel())
            throw std::runtime_error("Schema wait cancelled.");
        std::unique_lock lock(cache->mutex);
        for (auto it = cache->ready.begin(); it != cache->ready.end();)
            if (it->second.expired())
                it = cache->ready.erase(it);
            else
                ++it;
        if (auto found = cache->ready.find(key); found != cache->ready.end())
            if (auto owner = found->second.lock())
                return owner;
        if (auto found = cache->running.find(key); found != cache->running.end()) {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
            throw std::logic_error("Reentrant construction of the same schema artifact.");
#else
            future = found->second;
            break;
#endif
        }
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        if (!nested && cache->running.size() >= 4) {
            cache->changed.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
#endif
        auto promise = std::make_shared<std::promise<ConstPtr>>();
        future = promise->get_future().share();
        cache->running.emplace(key, future);
        auto build = [cache = cache, key, promise, build = std::move(builder)]
        {
            auto& activeKeys = Impl::activeBuildKeys();
            activeKeys.push_back(key);
            auto leave = [](auto* keys)
            {
                keys->pop_back();
            };
            std::unique_ptr<std::vector<std::string>, decltype(leave)> active(&activeKeys, leave);
            try {
                auto owner = build();
                if (!owner || !owner->finalized())
                    throw std::invalid_argument("Schema builder did not return a finalized owner.");
                std::lock_guard lock(cache->mutex);
                if (cache->ready.size() >= 256)
                    cache->ready.erase(cache->ready.begin());
                cache->ready[key] = owner;
                cache->running.erase(key);
                promise->set_value(owner);
            }
            catch (...) {
                std::lock_guard lock(cache->mutex);
                cache->running.erase(key);
                promise->set_exception(std::current_exception());
            }
            cache->changed.notify_all();
        };
        try {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
            // The browser build has no pthread runtime. It still shares the
            // completed immutable artifact but constructs it on this call stack.
            lock.unlock();
            build();
#else
            // A network builder may decode a second, content-addressed key. Reuse
            // this worker instead of waiting for a fifth slot behind four parents.
            if (nested) {
                lock.unlock();
                build();
            }
            else
                std::thread(std::move(build)).detach();
#endif
        }
        catch (...) {
            cache->running.erase(key);
            cache->changed.notify_all();
            throw;
        }
        break;
    }
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
        if (cancel && cancel())
            throw std::runtime_error("Schema wait cancelled.");
    if (cancel && cancel())
        throw std::runtime_error("Schema wait cancelled.");
    return future.get();
}

simfil::Schema const* MapSchema::type(TypeId id) const
{
    if (!simfil::SchemaPool::finalized())
        throw std::logic_error("Schema query requires a finalized graph.");
    return simfil::SchemaPool::type(id);
}
std::shared_ptr<simfil::SchemaBinding const> MapSchema::binding() const
{
    if (!finalized())
        throw std::logic_error("Schema binding requires a finalized graph.");
    return simfil::SchemaPool::binding(std::string(schemaId()));
}
simfil::ModelNode::Ptr MapSchema::typeNode(TypeId id) const
{
    if (!type(id))
        return {};
    return simfil::SchemaPool::typeNode(id);
}
std::vector<std::string> MapSchema::checkForErrors() const
{
    std::vector<std::string> errors;
    try {
        schema_->validate();
        schema_->validatePool();
    }
    catch (std::exception const& e) {
        errors.push_back(e.what());
    }
    return errors;
}
tl::expected<void, simfil::Error>
MapSchema::resolve(simfil::ModelNode const& node, ResolveFn const& callback) const
{
    if (node.addr().column() == Impl::Layers) {
        if (node.addr().index() >= schema_->layers.size())
            return tl::unexpected(simfil::Error{
                simfil::Error::IndexOutOfRange,
                "Unknown LayerSchema address."});
        auto view = LayerSchema::Ptr::make(shared_from_this(), node.addr());
        callback(*view);
        return {};
    }
    return ModelPool::resolve(node, callback);
}
LayerSchema::Ptr
resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const& pool, simfil::ModelNode const& node)
{
    if (node.addr().column() != MapSchema::Impl::Layers ||
        node.addr().index() >= pool.schema_->layers.size())
        throw std::invalid_argument("Cannot resolve node as a LayerSchema.");
    return LayerSchema::Ptr::make(pool.shared_from_this(), node.addr());
}

schema_detail::SchemaBuilder MapSchema::Impl::exportBuilder() const
{
    schema_detail::SchemaBuilder result;
    result.schemas_.resize(types.size());
    result.entriesById_.resize(types.size());
    auto scalarJson = [](simfil::Schema::NumericValue const& value)
    {
        return std::visit([](auto v) { return nlohmann::json(v); }, value);
    };
    for (size_t i = 1; i < types.size(); ++i) {
        auto const& row = types[i];
        auto const& view = *pool.type(i);
        auto& out = result.schemas_[i];
        out.id_ = TypeId(i);
        out.kind_ = view.kind();
        out.entry_ = {
            TypeId(i),
            std::string(text(row.key)),
            std::string(text(row.pointer)),
            std::string(text(row.metaType))};
        result.entriesById_[i] = out.entry_;
        out.typeName_ = view.typeName();
        out.zserioType_ = text(row.zserioType);
        out.attributeType_ = text(row.attributeType);
        out.attributeTypeCode_ = text(row.attributeCode);
        out.queryRoot_ = row.flags & 8;
        out.open_ = view.open();
        out.nullable_ = view.nullable();
        out.composition_ = view.composition();
        for (auto alternative : view.alternatives())
            out.alternatives_.push_back(alternative);
        view.forEachElementSchema([&](TypeId child) { out.elementSchemas_.push_back(child); });
        view.forEachDirectField(
            [&](StringId field, auto children)
            {
                auto n = std::string(name(field));
                out.directFields_.push_back(n);
                out.childSchemas_[n] = {children.begin(), children.end()};
                // The generic pool represents an unknown declaration as a zero
                // edge; legacy JSON uses unknownEdge rather than a dangling ref 0.
                if (children.size() == 1 && children.front() == simfil::NoTypeId)
                    out.childSchemas_[n].clear();
                if (pool.fieldMultimap(TypeId(i), field))
                    out.multimapFields_.insert(n);
                if (auto required = view.fieldRequired(field))
                    out.required_[n] = *required;
                if (view.fieldRole(field) == simfil::Schema::FieldRole::Context)
                    out.contextFields_.insert(n);
            });
        for (auto symbol : view.directEnumSymbols())
            out.directEnumSymbols_.push_back(std::string(name(symbol)));
        out.enumValues_ = {view.enumValues().begin(), view.enumValues().end()};
        out.attributeOwners_ = attributeOwners(TypeId(i));
        out.annotations_ = annotation(row.annotations);
        if (auto numeric = view.numericDomain()) {
            auto& json = out.annotations_["x-mapget"]["numericDomain"];
            json = {
                {"encoding", numeric->encoding},
                {"unit", numeric->unit},
                {"scale", numeric->scale},
                {"offset", numeric->offset},
                {"complete", numeric->complete}};
            if (numeric->minimum)
                json["minimum"] = scalarJson(*numeric->minimum);
            if (numeric->maximum)
                json["maximum"] = scalarJson(*numeric->maximum);
            for (auto const& range : numeric->ordinaryRanges)
                json["ordinaryRanges"].push_back(
                    {{"minimum", scalarJson(range.min)}, {"maximum", scalarJson(range.max)}});
        }
        view.forEachNamedValue(
            [&](auto const& value)
            {
                static constexpr std::string_view roles[]{"constant", "special", "flag"};
                static constexpr std::string_view
                    meanings[]{"ordinary", "unknown", "variable", "unbounded", "saturated"};
                out.annotations_["x-mapget"]["namedValues"].push_back(
                    {{"name", value.name},
                     {"value", std::visit([](auto v) { return nlohmann::json(v); }, value.value)},
                     {"role", roles[unsigned(value.role)]},
                     {"meaning", meanings[unsigned(value.meaning)]},
                     {"deprecated", value.deprecated},
                     {"removed", value.removed}});
                if (value.rawValue)
                    out.annotations_["x-mapget"]["namedValues"].back()["rawValue"] =
                        std::visit([](auto raw) { return nlohmann::json(raw); }, *value.rawValue);
            });
        if (view.isBitmask())
            out.annotations_["x-mapget"]["bitWidth"] = view.bitWidth();
    }
    for (auto const& [key, id] : globalAliases)
        result.idsByKey_.emplace(key, id);
    return result;
}

nlohmann::json LayerSchema::toJsonSchema() const
{
    auto const& graph = *mapSchema()->schema_;
    auto compiled = graph.builder ? *graph.builder : graph.exportBuilder();
    auto const& layer = graph.layerAt(addr_.index());
    for (auto& type : compiled.schemas_)
        std::erase_if(
            type.attributeOwners_,
            [&](auto const& owner)
            { return !owner.featureLayer_.empty() && owner.featureLayer_ != layer.name; });
    if (graph.builder)
        for (auto const& [key, id] : layer.buildAliases)
            compiled.idsByKey_[key] = id;
    else
        for (auto const& [key, id] : layer.aliases)
            compiled.idsByKey_[std::string(key)] = id;
    std::vector<TypeId> roots = layer.featureRoots;
    if (layer.root)
        roots.push_back(layer.root);
    // Layer-local aliases can name converter-only or reference-only contracts.
    if (graph.builder)
        for (auto const& [_, id] : layer.buildAliases)
            roots.push_back(id);
    else
        for (auto const& [_, id] : layer.aliases)
            roots.push_back(id);
    auto json = compiled.exportTypes(roots);
    // Native forward declarations are encoded only at the JSON interchange boundary.
    if (layer.relationsDeclared) {
        auto declarations = nlohmann::json::array();
        for (auto const& relation : layer.relations) {
            static constexpr std::string_view locality[]{
                "unknown",
                "same-container",
                "may-cross-container"};
            declarations.push_back(
                {{"ownerType", relation.ownerType_},
                 {"name", relation.name_},
                 {"targetTypes", relation.targetTypes_},
                 {"complete", relation.complete_},
                 {"locality", locality[unsigned(relation.locality_)]}});
        }
        for (size_t i = 0; i < layer.featureRoots.size(); ++i) {
            auto root = layer.featureRoots[i];
            auto& metadata = json["definitions"][std::to_string(root)]["x-mapget"];
            metadata["relationDeclarations"] = nlohmann::json::array();
            for (auto const& declaration : declarations)
                if (declaration["ownerType"] == layer.features[i].name_)
                    metadata["relationDeclarations"].push_back(declaration);
            metadata["relationDeclarationsKnown"] = layer.relationsKnown;
        }
    }
    json.erase("anyOf");
    if (layer.root)
        json["$ref"] = "#/definitions/" + std::to_string(layer.root);
    else
        for (auto root : graph.producedRoots(layer))
            json["anyOf"].push_back({{"$ref", "#/definitions/" + std::to_string(root)}});
    return json;
}

LayerSchema::Entry const* LayerSchema::getSchema(std::string_view key) const
{
    auto id = schemaId(key);
    if (!id)
        id = featureSchema(key);
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder)
        throw std::logic_error(
            "getSchema() requires finalization; use schemaId() during construction.");
    return id && id < graph.entries.size() ? &graph.entries[id] : nullptr;
}
bool LayerSchema::hasSchema(TypeId id) const
{
    return mapSchema()->schema_->valid(id);
}
Kind LayerSchema::kind(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder)
        return graph.builder->kind(id);
    return graph.valid(id) ? graph.pool.type(id)->kind() : Kind::Unknown;
}
std::string_view LayerSchema::typeName(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        std::string_view(graph.builder->schemas_[id].typeName_) :
        graph.pool.type(id)->typeName();
}
Composition LayerSchema::composition(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return Composition::None;
    return graph.builder ?
        graph.builder->schemas_[id].composition_ :
        graph.pool.type(id)->composition();
}
std::span<TypeId const> LayerSchema::alternatives(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        std::span<TypeId const>(graph.builder->schemas_[id].alternatives_) :
        graph.pool.type(id)->alternatives();
}
std::span<simfil::ScalarValueType const> LayerSchema::enumValues(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        std::span<simfil::ScalarValueType const>(graph.builder->schemas_[id].enumValues_) :
        graph.pool.type(id)->enumValues();
}
bool LayerSchema::open(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return true;
    return graph.builder ? graph.builder->schemas_[id].open_ : graph.pool.type(id)->open();
}
std::optional<bool> LayerSchema::nullable(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ? graph.builder->schemas_[id].nullable_ : graph.pool.type(id)->nullable();
}
std::optional<bool> LayerSchema::fieldRequired(TypeId id, std::string_view field) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    if (graph.builder) {
        auto const& required = graph.builder->schemas_[id].required_;
        if (auto it = required.find(field); it != required.end())
            return it->second;
        return {};
    }
    return graph.pool.type(id)->fieldRequired(graph.pool.strings()->get(field));
}
bool LayerSchema::reachabilityComplete(TypeId id) const
{
    auto const& graph = *mapSchema()->schema_;
    return !graph.builder && graph.valid(id) && graph.pool.type(id)->reachabilityComplete();
}

simfil::Schema::FieldRole LayerSchema::fieldRole(TypeId id, std::string_view field) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return simfil::Schema::FieldRole::Data;
    if (graph.builder)
        return graph.builder->schemas_[id].contextFields_.contains(field) ?
            simfil::Schema::FieldRole::Context :
            simfil::Schema::FieldRole::Data;
    return graph.pool.type(id)->fieldRole(graph.pool.strings()->get(field));
}
bool LayerSchema::canHaveField(TypeId id, std::string_view field) const
{
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder || !graph.valid(id))
        return true;
    return graph.pool.type(id)->canHaveField(graph.pool.strings()->get(field));
}
bool LayerSchema::canHaveEnumSymbol(TypeId id, std::string_view symbol) const
{
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder || !graph.valid(id))
        return false;
    return graph.pool.type(id)->canHaveEnumSymbol(graph.pool.strings()->get(symbol));
}
std::span<std::string_view const> LayerSchema::directFields(TypeId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ? g.pool.fieldNames(id) : std::span<std::string_view const>{};
}
std::span<std::string_view const> LayerSchema::nestedFields(TypeId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ?
        g.pool.fieldNames(id, true) :
        std::span<std::string_view const>{};
}
std::span<std::string_view const> LayerSchema::directEnumSymbols(TypeId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ? g.pool.enumNames(id) : std::span<std::string_view const>{};
}
std::span<std::string_view const> LayerSchema::nestedEnumSymbols(TypeId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ?
        g.pool.enumNames(id, true) :
        std::span<std::string_view const>{};
}
std::string_view LayerSchema::attributeTypeCode(TypeId id) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return {};
    return g.builder ?
        std::string_view(g.builder->schemas_[id].attributeTypeCode_) :
        g.text(g.types[id].attributeCode);
}
void LayerSchema::forEachDirectField(
    TypeId id,
    std::function<void(std::string_view, std::span<TypeId const>)> const& fn) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return;
    if (g.builder)
        g.builder->forEachDirectField(id, fn);
    else
        g.pool.type(id)
            ->forEachDirectField([&](StringId name, auto children) { fn(g.name(name), children); });
}
void LayerSchema::forEachElementSchema(TypeId id, std::function<void(TypeId)> const& fn) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return;
    if (g.builder)
        g.builder->forEachElementSchema(id, fn);
    else
        g.pool.type(id)->forEachElementSchema(fn);
}
TypeId LayerSchema::attributeQuerySchema(std::string_view feature, TypeId attribute) const
{
    auto const& roots = mapSchema()->schema_->queryRoots;
    auto it = roots.find({featureSchema(feature), attribute});
    return it == roots.end() ? 0 : it->second;
}
TypeId LayerSchema::featureSchema(std::string_view feature) const
{
    return schemaId(featureKey(feature));
}
TypeId LayerSchema::featurePropertiesSchema(std::string_view feature) const
{
    return schemaId(featurePropertiesKey(feature));
}
TypeId LayerSchema::attributeLayerMapSchema(std::string_view feature) const
{
    return schemaId(attributeLayerMapKey(feature));
}

std::string_view MapSchema::Impl::metaType(TypeId id) const
{
    if (!valid(id))
        return {};
    return builder ?
        std::string_view(builder->schemas_[id].entry_.metaType_) :
        text(types[id].metaType);
}
std::vector<LayerSchema::AttributePathOwner> MapSchema::Impl::attributeOwners(TypeId id) const
{
    if (!valid(id))
        return {};
    if (builder)
        return builder->schemas_[id].attributeOwners_;
    std::vector<LayerSchema::AttributePathOwner> result;
    for (auto const& owner : types[id].owners.in(owners))
        result.push_back(
            {std::string(name(owner.feature)),
             std::string(name(owner.layer)),
             std::string(name(owner.name)),
             id,
             owner.featureLayer ? std::string(name(owner.featureLayer)) : std::string{}});
    return result;
}
TypeId
MapSchema::Impl::childSchema(TypeId root, std::string_view field, std::optional<Kind> preferred)
    const
{
    if (builder)
        return builder->childSchema(root, field, preferred);
    std::vector<TypeId> pending{root};
    std::vector<bool> seen(types.size());
    TypeId fallback = 0;
    auto fieldId = pool.strings()->get(field);
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto const& view = *pool.type(id);
        for (auto alternate : view.alternatives())
            pending.push_back(alternate);
        if (hasAffinity(view.kind(), simfil::ValueType::Array) &&
            view.composition() == Composition::None)
            view.forEachElementSchema([&](TypeId item) { pending.push_back(item); });
        TypeId selected = 0;
        view.forEachDirectField(
            [&](StringId name, auto children)
            {
                if (name != view.canonicalField(fieldId))
                    return;
                for (auto child : children) {
                    if (!child)
                        continue;
                    if (!fallback)
                        fallback = child;
                    if (!selected &&
                        (!preferred || matchesKind(pool.type(child)->kind(), *preferred)))
                        selected = child;
                }
            });
        if (selected || fallback)
            return selected ? selected : fallback;
    }
    return 0;
}
TypeId
LayerSchema::childSchema(TypeId root, std::string_view field, std::optional<Kind> preferred) const
{
    return mapSchema()->schema_->childSchema(root, field, preferred);
}

std::vector<LayerSchema::AttributePathOwner> LayerSchema::attributeOwners(TypeId type) const
{
    return mapSchema()->schema_->attributeOwners(type);
}

LayerSchema::PathOwner LayerSchema::ownerForPath(
    std::string_view featureType,
    simfil::TypeId rootSchema,
    std::span<const std::string> fieldPath) const
{
    auto const& graph = *mapSchema()->schema_;
    auto uniqueOwner = [&](TypeId id, std::string_view feature) -> std::optional<AttributePathOwner>
    {
        std::optional<AttributePathOwner> result;
        for (auto const& owner : graph.attributeOwners(id))
            if (owner.featureType_ == feature &&
                (owner.featureLayer_.empty() || owner.featureLayer_ == name())) {
                if (result)
                    return std::nullopt;
                result = owner;
            }
        return result;
    };
    auto currentSchema = rootSchema;
    if (!graph.valid(currentSchema)) {
        return {};
    }

    auto attributeOwner = uniqueOwner(rootSchema, featureType);
    auto currentLayerName = std::optional<std::string>{};
    auto enteredAttributeBranch = graph.metaType(rootSchema) == "AttributeLayerMap" ||
        graph.metaType(rootSchema) == "AttributeContainer" ||
        graph.metaType(rootSchema) == "Attribute";

    for (auto const& fieldName : fieldPath) {
        if (fieldName.empty()) {
            continue;
        }

        auto const parentSchema = currentSchema;
        auto const parentMetaType = graph.metaType(parentSchema);
        auto pendingAttributeName = std::optional<std::string>{};

        if (parentMetaType == "AttributeLayerMap") {
            // This edge selects the concrete attribute layer, but not yet a
            // concrete attribute within that layer.
            enteredAttributeBranch = true;
            currentLayerName = fieldName;
        }
        else if (parentMetaType == "AttributeContainer") {
            // This edge selects the attribute object inside the active layer.
            enteredAttributeBranch = true;
            pendingAttributeName = fieldName;
        }

        currentSchema = graph.childSchema(currentSchema, fieldName, std::nullopt);
        if (currentSchema == simfil::NoTypeId) {
            return {};
        }

        auto const currentMetaType = graph.metaType(currentSchema);
        if (currentMetaType == "AttributeLayerMap" || currentMetaType == "AttributeContainer") {
            enteredAttributeBranch = true;
        }
        if (currentMetaType == "Attribute") {
            enteredAttributeBranch = true;
            // If the schema id is intentionally shared, the path edge still
            // contains enough context to identify the selected attribute.
            auto attributeName =
                pendingAttributeName.value_or(std::string(attributeTypeCode(currentSchema)));
            if (currentLayerName && !attributeName.empty()) {
                attributeOwner = AttributePathOwner{
                    std::string(featureType),
                    *currentLayerName,
                    std::move(attributeName),
                    currentSchema,
                    std::string(name())};
            }
            else if (!attributeOwner)
                attributeOwner = uniqueOwner(currentSchema, featureType);
        }
        else if (auto owner = uniqueOwner(currentSchema, featureType); !attributeOwner && owner) {
            attributeOwner = *owner;
        }
    }

    if (attributeOwner) {
        return {PathOwnerKind::Attribute, *attributeOwner};
    }

    if (enteredAttributeBranch) {
        return {};
    }

    if (rootSchema == featureSchema(featureType) ||
        rootSchema == featurePropertiesSchema(featureType))
    {
        return {PathOwnerKind::Feature, {}};
    }

    return {};
}

void LayerSchema::setJsonSchemaAnnotations(simfil::TypeId id, nlohmann::json annotations)
{
    mutableOwner().setJsonSchemaAnnotations(id, std::move(annotations));
}

void LayerSchema::addFieldSchema(
    simfil::TypeId parent,
    std::string fieldName,
    simfil::TypeId child,
    bool multimap)
{
    mutableOwner().addFieldSchema(parent, std::move(fieldName), child, multimap);
}

void LayerSchema::addElementSchema(simfil::TypeId parent, simfil::TypeId child)
{
    mutableOwner().addElementSchema(parent, child);
}

void LayerSchema::addEnumSymbol(simfil::TypeId schemaId, std::string symbolName)
{
    mutableOwner().addEnumSymbol(schemaId, std::move(symbolName));
}

void LayerSchema::addEnumSymbols(simfil::TypeId schemaId, std::span<const std::string> symbolNames)
{
    mutableOwner().addEnumSymbols(schemaId, symbolNames);
}

void LayerSchema::addEnumValue(simfil::TypeId schemaId, simfil::ScalarValueType value)
{
    mutableOwner().addEnumValue(schemaId, std::move(value));
}

void LayerSchema::setComposition(simfil::TypeId schemaId, Composition composition)
{
    mutableOwner().setComposition(schemaId, composition);
}

void LayerSchema::addAlternative(simfil::TypeId schemaId, simfil::TypeId alternative)
{
    mutableOwner().addAlternative(schemaId, alternative);
}

void LayerSchema::setTypeName(simfil::TypeId schemaId, std::string name)
{
    mutableOwner().setTypeName(schemaId, std::move(name));
}

void LayerSchema::setOpen(simfil::TypeId schemaId, bool open)
{
    mutableOwner().setOpen(schemaId, open);
}

void LayerSchema::setNullable(simfil::TypeId schemaId, std::optional<bool> nullable)
{
    mutableOwner().setNullable(schemaId, nullable);
}

void LayerSchema::setFieldRequired(
    simfil::TypeId schemaId,
    std::string field,
    std::optional<bool> required)
{
    mutableOwner().setFieldRequired(schemaId, std::move(field), required);
}

void LayerSchema::setZserioType(simfil::TypeId schemaId, std::string zserioType)
{
    mutableOwner().setZserioType(schemaId, std::move(zserioType));
}

void LayerSchema::setFieldRole(
    simfil::TypeId schemaId,
    std::string field,
    simfil::Schema::FieldRole role)
{
    mutableOwner().setFieldRole(schemaId, std::move(field), role);
}

void LayerSchema::setAttributeMetadata(
    simfil::TypeId schemaId,
    AttributePathOwner owner,
    std::string attributeType,
    std::string zserioType)
{
    owner.featureLayer_ = name();
    mutableOwner().setAttributeMetadata(
        schemaId,
        std::move(owner),
        std::move(attributeType),
        std::move(zserioType));
}

std::string LayerSchema::featureKey(std::string_view featureType)
{
    return "Feature:" + std::string(featureType);
}

std::string LayerSchema::featurePropertiesKey(std::string_view featureType)
{
    return "FeatureProperties:" + std::string(featureType);
}

std::string LayerSchema::attributeLayerMapKey(std::string_view featureType)
{
    return "AttributeLayerMap:" + std::string(featureType);
}

std::string LayerSchema::attributeContainerKey(
    std::string_view featureType,
    std::string_view attributeLayerName)
{
    return "AttributeContainer:" + std::string(featureType) + ":" + std::string(attributeLayerName);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode)
{
    return "Attribute:" + std::string(featureType) + ":" + std::string(attributeLayerName) + ":" +
        std::string(attributeTypeCode);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode,
    std::string_view sourceDefinitionKey)
{
    std::string result = "AttributeSource:";
    for (auto part : {featureType, attributeLayerName, attributeTypeCode, sourceDefinitionKey}) {
        if (part.empty())
            throw std::invalid_argument("Qualified attribute keys require all four context parts.");
        result += std::to_string(part.size());
        result += ':';
        result += part;
    }
    return result;
}

std::vector<LayerSchema::NamedSchemaPath>
MapSchema::Impl::scalarPaths(TypeId root, std::string_view attribute) const
{
    if (builder)
        return builder->scalarFieldPathsForAttribute(root, attribute);
    if (!valid(root) ||
        std::ranges::none_of(
            types,
            [&](auto const& row)
            {
                return text(row.attributeCode) == attribute ||
                    std::ranges::any_of(
                           row.owners.in(owners),
                           [&](auto const& owner) { return name(owner.name) == attribute; });
            }))
        return {};

    // Probe each node once before enumerating paths. Bare query operands often
    // are not attribute codes; a shared DAG must not expand exponentially merely
    // to prove that a code (or a selected payload scalar) is absent.
    constexpr uint8_t hasAttribute = 1, hasScalar = 2;
    std::vector<std::vector<TypeId>> parents(types.size());
    std::vector<bool> seen(types.size());
    std::vector<uint8_t> reachable(types.size());
    std::vector<TypeId> pending{root};
    seen[root] = true;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        auto const& row = types[id];
        if (text(row.attributeCode) == attribute ||
            std::ranges::any_of(
                row.owners.in(owners),
                [&](auto const& owner) { return name(owner.name) == attribute; }))
            reachable[id] |= hasAttribute;
        auto const& domain = *pool.type(id);
        if (matchesKind(domain.kind(), Kind::Value))
            reachable[id] |= hasScalar;
        auto addChild = [&](TypeId child)
        {
            if (!valid(child))
                return;
            parents[child].push_back(id);
            if (!seen[child]) {
                seen[child] = true;
                pending.push_back(child);
            }
        };
        domain.forEachEnumerableField(
            [&](auto, auto children)
            {
                for (auto child : children)
                    addChild(child);
            });
        domain.forEachElementSchema(addChild);
        for (auto child : domain.alternatives())
            addChild(child);
    }
    for (size_t id = 1; id < types.size(); ++id)
        if (reachable[id])
            pending.push_back(TypeId(id));
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        for (auto parent : parents[id]) {
            auto added = reachable[id] & ~reachable[parent];
            if (added) {
                reachable[parent] |= added;
                pending.push_back(parent);
            }
        }
    }

    std::vector<LayerSchema::NamedSchemaPath> result;
    LayerSchema::NamedSchemaPath path;
    std::vector<bool> active(types.size());
    size_t work = 0;
    // An attribute shorthand chooses its first payload scalar, not every scalar
    // in the record. Keep this precedence identical to the transient compiler.
    std::function<bool(TypeId, bool, bool, std::string_view)> visit =
        [&](TypeId id, bool selected, bool first, std::string_view boundName)
    {
        if (!valid(id) || active[id])
            return false;
        auto const& row = types[id];
        auto const& view = *pool.type(id);
        auto matches =
            (boundName.empty() ?
                 text(row.attributeCode) == attribute :
                 boundName == attribute && metaType(id) == "Attribute");
        if (id == root && boundName.empty())
            matches = matches ||
                std::ranges::any_of(
                          row.owners.in(owners),
                          [&](auto const& owner) { return name(owner.name) == attribute; });
        selected = selected || matches;
        // Reachability deliberately includes data metadata edges: only the original
        // path-local walk below decides exclusions and first-payload precedence.
        if (!(reachable[id] & hasScalar) || (!selected && !(reachable[id] & hasAttribute)))
            return false;
        if (++work > 100000 || path.size() > 256)
            throw std::length_error("Schema path traversal budget exceeded.");
        if (selected && matchesKind(view.kind(), Kind::Value)) {
            result.push_back(path);
            return true;
        }
        active[id] = true;
        bool found = false;
        for (auto child : view.alternatives()) {
            found = visit(child, selected, first || matches, boundName) || found;
            if (selected && found)
                break;
        }
        view.forEachEnumerableField(
            [&](StringId field, auto children)
            {
                if (selected && found)
                    return;
                auto name = this->name(field);
                if ((first || matches) &&
                    schema_detail::isAttributeScalarShorthandMetadataField(name))
                    return;
                path.push_back({simfil::SchemaPathSegment::Kind::Field, std::string(name)});
                for (auto child : children) {
                    auto useSite = metaType(id) == "AttributeContainer" ? name : boundName;
                    found = visit(child, selected, false, useSite) || found;
                    if (selected && found)
                        break;
                }
                path.pop_back();
            });
        view.forEachElementSchema(
            [&](TypeId child)
            {
                if (selected && found)
                    return;
                path.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, {}});
                found = visit(child, selected, false, boundName) || found;
                path.pop_back();
            });
        active[id] = false;
        return found;
    };
    visit(root, false, false, {});
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::vector<LayerSchema::NamedSchemaPath>
LayerSchema::scalarFieldPathsForAttribute(TypeId root, std::string_view attribute) const
{
    if (attribute.empty())
        return {};
    return mapSchema()->schema_->scalarPaths(root, attribute);
}
std::vector<LayerSchema::AttributePathOwner> LayerSchema::attributeScopes() const
{
    auto const& g = *mapSchema()->schema_;
    std::vector<AttributePathOwner> result;
    std::vector<TypeId> pending;
    for (auto const& feature : featureTypes())
        pending.push_back(featureSchema(feature));
    std::set<TypeId> seen;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!g.valid(id) || !seen.insert(id).second)
            continue;
        for (auto const& owner : g.attributeOwners(id))
            if (producesFeatureType(owner.featureType_) &&
                (owner.featureLayer_.empty() || owner.featureLayer_ == name()))
                result.push_back(owner);
        forEachDirectField(
            id,
            [&](auto field, auto children)
            {
                if (fieldRole(id, field) == simfil::Schema::FieldRole::Data)
                    pending.insert(pending.end(), children.begin(), children.end());
            });
        forEachElementSchema(id, [&](auto child) { pending.push_back(child); });
        auto a = alternatives(id);
        pending.insert(pending.end(), a.begin(), a.end());
    }
    auto key = [](auto const& o)
    {
        return std::tie(
            o.featureType_,
            o.attributeLayerName_,
            o.attributeName_,
            o.attributeSchema_);
    };
    std::ranges::sort(result, {}, key);
    result.erase(
        std::unique(
            result.begin(),
            result.end(),
            [&](auto const& a, auto const& b) { return key(a) == key(b); }),
        result.end());
    return result;
}
std::vector<std::string> LayerSchema::constantTypeNames(TypeId root, std::string_view symbol) const
{
    auto const& g = *mapSchema()->schema_;
    if (g.builder)
        return g.builder->constantTypeNames(root, symbol);
    std::vector<std::string> result;
    std::vector<TypeId> pending{root};
    std::vector<bool> seen(g.types.size());
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!g.valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto const& d = g.types[id];
        if (d.attributeType &&
            (g.text(d.attributeCode) == symbol ||
             std::ranges::any_of(
                 g.attributeOwners(id),
                 [&](auto const& owner) { return owner.attributeName_ == symbol; })))
            result.emplace_back(g.text(d.attributeType));
        auto typeName = g.pool.type(id)->typeName();
        if (std::ranges::find(directEnumSymbols(id), symbol) != directEnumSymbols(id).end() &&
            !typeName.empty())
            result.emplace_back(typeName);
        g.pool.type(id)->forEachNamedValue(
            [&](auto const& value)
            {
                if (value.name == symbol && !typeName.empty())
                    result.emplace_back(typeName);
            });
        forEachDirectField(
            id,
            [&](auto field, auto children)
            {
                if (fieldRole(id, field) == simfil::Schema::FieldRole::Data)
                    pending.insert(pending.end(), children.begin(), children.end());
            });
        forEachElementSchema(id, [&](auto child) { pending.push_back(child); });
        auto a = alternatives(id);
        pending.insert(pending.end(), a.begin(), a.end());
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
void LayerSchema::setNumericDomain(TypeId id, simfil::Schema::NumericDomain const& d)
{
    mutableOwner().setNumericDomain(id, d);
}
void LayerSchema::addNamedValue(TypeId id, simfil::Schema::NamedValue const& v)
{
    mutableOwner().addNamedValue(id, v);
}
void LayerSchema::setBitWidth(TypeId id, uint8_t width)
{
    mutableOwner().setBitWidth(id, width);
}
std::optional<simfil::Schema::NumericDomain> LayerSchema::numericDomain(TypeId id) const
{
    return mapSchema()->numericDomain(id);
}
void LayerSchema::forEachNamedValue(
    TypeId id,
    std::function<void(simfil::Schema::NamedValue const&)> const& fn) const
{
    mapSchema()->forEachNamedValue(id, fn);
}
uint8_t LayerSchema::bitWidth(TypeId id) const
{
    return mapSchema()->bitWidth(id);
}

bool LayerSchema::RelationDeclaration::permitsIncoming(
    std::string_view owner,
    std::string_view name,
    std::string_view target) const
{
    return ownerType_ == owner && name_ == name &&
        std::ranges::find(targetTypes_, target) != targetTypes_.end();
}
nlohmann::json
LayerSchema::featureTypeContract(std::string_view name, std::function<void(size_t)> const& reserve)
    const
{
    auto chargeString = [&](std::string_view value)
    {
        if (!reserve)
            return;
        if (value.size() > (std::numeric_limits<size_t>::max() - 2) / 6)
            throw std::length_error("Feature contract string is too large.");
        reserve(2 + value.size() * 6);  // Quotes and worst-case escaping, before allocating JSON.
    };
    auto const& identity = *getTypeInfo(name);
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    if (reserve)
        reserve(256);
    chargeString(identity.name_);
    auto result = nlohmann::json{
        {"name", identity.name_},
        {"uniqueIdCompositions", nlohmann::json::array()},
        {"produced", producesFeatureType(name)},
        {"typeId", featureSchema(name)},
        {"forwardRelationsKnown", layer.relationsKnown},
        {"forwardRelations", nlohmann::json::array()}};
    for (auto const& composition : identity.uniqueIdCompositions_) {
        if (reserve)
            reserve(8);
        nlohmann::json parts;
        for (auto const& part : composition) {
            if (reserve)
                reserve(128);
            chargeString(part.idPartLabel_);
            chargeString(part.description_);
            parts.push_back(part.toJson());
        }
        result["uniqueIdCompositions"].push_back(std::move(parts));
    }
    // Read the frozen relation index directly. A layer-wide vector copy would allocate
    // unrelated declarations before the caller can stop a focused inspection.
    for (auto const& declaration : layer.relations) {
        if (reserve)
            reserve(0);
        if (declaration.ownerType_ != name)
            continue;
        if (reserve)
            reserve(128);
        chargeString(declaration.name_);
        auto targets = nlohmann::json::array();
        for (auto const& target : declaration.targetTypes_) {
            chargeString(target);
            targets.push_back(target);
        }
        auto locality = declaration.locality_ == RelationDeclaration::Locality::SameContainer ?
            "same-container" :
            declaration.locality_ == RelationDeclaration::Locality::MayCrossContainer ?
            "may-cross-container" :
            "unknown";
        result["forwardRelations"].push_back(
            {{"name", declaration.name_},
             {"targetTypes", std::move(targets)},
             {"complete", declaration.complete_},
             {"locality", locality}});
    }
    return result;
}

std::vector<LayerSchema::RelationDeclaration> LayerSchema::relationDeclarations() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).relations;
}
std::vector<LayerSchema::RelationDeclaration>
LayerSchema::relationsTo(std::string_view target) const
{
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    std::vector<RelationDeclaration> result;
    if (auto found = layer.relationsByTarget.find(target); found != layer.relationsByTarget.end())
        for (auto i : found->second)
            result.push_back(layer.relations[i]);
    return result;
}
bool LayerSchema::hasRelationDeclarations() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).relationsKnown;
}

/** A layer remains a small native object even without feature roots (e.g. an
 * open SourceData decoder). Domain descriptors share the graph through SchemaModel.
 */
simfil::ValueType LayerSchema::type() const
{
    return simfil::ValueType::Object;
}
uint32_t LayerSchema::size() const
{
    return 3;
}
StringId LayerSchema::keyAt(int64_t i) const
{
    if (i < 0 || i >= size())
        return 0;
    return mapSchema()->schema_->descriptorKeys[size_t(i)];
}
simfil::ModelNode::Ptr LayerSchema::at(int64_t i) const
{
    if (i == 0)
        return simfil::model_ptr<simfil::ValueNode>::make(simfil::ScalarValueType{name()}, model_);
    if (i == 1)
        return simfil::model_ptr<simfil::ValueNode>::make(
            simfil::ScalarValueType{int64_t(layerType())},
            model_);
    if (i == 2)
        return rootSchema() ? mapSchema()->typeNode(rootSchema()) : simfil::ModelNode::Ptr{};
    return {};
}
simfil::ModelNode::Ptr LayerSchema::get(StringId const& field) const
{
    for (size_t i = 0; i < size(); ++i)
        if (keyAt(i) == field)
            return at(i);
    return {};
}
bool LayerSchema::iterate(IterCallback const& callback) const
{
    for (size_t i = 0; i < size(); ++i)
        if (auto node = at(i); node && !callback(*node))
            return false;
    return true;
}

MemoryUsageBreakdown MapSchema::memoryUsage() const
{
    if (schema_->memorySummary)
        return *schema_->memorySummary;
    MemoryUsageBreakdown result;
    result.add("identity", stringMemoryUsage(schema_->identity));
    result.add(
        "format",
        stringMemoryUsage(schema_->format.name_) + stringMemoryUsage(schema_->format.version_));
    result.add(
        "subsystems",
        {schema_->format.subsystems_.size() *
             sizeof(decltype(schema_->format.subsystems_)::value_type),
         schema_->format.subsystems_.size() *
             (sizeof(decltype(schema_->format.subsystems_)::value_type) + 3 * sizeof(void*))});
    for (auto const& [name, version] : schema_->format.subsystems_)
        result.add("subsystem-text", stringMemoryUsage(name) + stringMemoryUsage(version));
    result.add(
        "query-root-index",
        {schema_->queryRoots.size() * sizeof(decltype(schema_->queryRoots)::value_type),
         schema_->queryRoots.size() *
             (sizeof(decltype(schema_->queryRoots)::value_type) + 3 * sizeof(void*))});
    result.add("object", {sizeof(*this) + sizeof(Impl), sizeof(*this) + sizeof(Impl)});
    result.add("model-pool", memoryUsageStats().total());
    result.add("dictionary", strings()->memoryUsage());
    result.add("type-column", schema_->types.memory_usage());
    result.add("attribute-owners", vectorMemoryUsage(schema_->owners));
    result.add("aliases", vectorMemoryUsage(schema_->aliases));
    result.add("entries", vectorMemoryUsage(schema_->entries));
    result.add("layers", vectorMemoryUsage(schema_->layers));
    result.add("generic-schema", schemaMemoryUsage());
    result.add("constituents", vectorMemoryUsage(schema_->constituents));
    for (auto const& [identity, format] : schema_->constituents) {
        result.add(
            "constituent-text",
            stringMemoryUsage(identity) + stringMemoryUsage(format.name_) +
                stringMemoryUsage(format.version_));
        result.add(
            "constituent-subsystems",
            {format.subsystems_.size() * sizeof(decltype(format.subsystems_)::value_type),
             format.subsystems_.size() *
                 (sizeof(decltype(format.subsystems_)::value_type) + 3 * sizeof(void*))});
        for (auto const& [name, version] : format.subsystems_)
            result.add(
                "constituent-subsystem-text",
                stringMemoryUsage(name) + stringMemoryUsage(version));
    }
    result.add(
        "contributor-remaps",
        {schema_->contributorRemaps.size() *
             sizeof(decltype(schema_->contributorRemaps)::value_type),
         schema_->contributorRemaps.size() *
             (sizeof(decltype(schema_->contributorRemaps)::value_type) + 3 * sizeof(void*))});
    for (auto const& [identity, remap] : schema_->contributorRemaps)
        result
            .add("contributor-remap-data", stringMemoryUsage(identity) + vectorMemoryUsage(remap));
    for (auto const& layer : schema_->layers) {
        result.add("layer-names", stringMemoryUsage(layer.name));
        result.add("import-fingerprint", stringMemoryUsage(layer.importedFingerprint));
        result.add("relations", vectorMemoryUsage(layer.relations));
        for (auto const& relation : layer.relations)
            result.add(
                "relation-text",
                stringMemoryUsage(relation.ownerType_) + stringMemoryUsage(relation.name_) +
                    stringVectorMemoryUsage(relation.targetTypes_));
        result.add(
            "relation-target-index",
            {layer.relationsByTarget.size() * sizeof(decltype(layer.relationsByTarget)::value_type),
             layer.relationsByTarget.size() *
                 (sizeof(decltype(layer.relationsByTarget)::value_type) + 3 * sizeof(void*))});
        for (auto const& [target, indices] : layer.relationsByTarget)
            result.add(
                "relation-target-data",
                stringMemoryUsage(target) + vectorMemoryUsage(indices));
        result.add("feature-definitions", vectorMemoryUsage(layer.features));
        result.add("feature-roots", vectorMemoryUsage(layer.featureRoots));
        result.add("feature-production", vectorMemoryUsage(layer.produced));
        for (auto const& feature : layer.features) {
            result.add("feature-name", stringMemoryUsage(feature.name_));
            result.add("id-compositions", vectorMemoryUsage(feature.uniqueIdCompositions_));
            for (auto const& composition : feature.uniqueIdCompositions_) {
                result.add("id-parts", vectorMemoryUsage(composition));
                for (auto const& part : composition) {
                    result.add("id-part-names", stringMemoryUsage(part.idPartLabel_));
                    result.add("id-part-descriptions", stringMemoryUsage(part.description_));
                }
            }
        }
        result.add(
            "layer-alias-index",
            {layer.aliases.size() * sizeof(decltype(layer.aliases)::value_type),
             layer.aliases.size() *
                 (sizeof(decltype(layer.aliases)::value_type) + 3 * sizeof(void*))});
    }
    result.add(
        "global-alias-index",
        {schema_->globalAliases.size() * sizeof(decltype(schema_->globalAliases)::value_type),
         schema_->globalAliases.size() *
             (sizeof(decltype(schema_->globalAliases)::value_type) + 3 * sizeof(void*))});
    return result;
}

void MapSchema::Impl::summarize()
{
    auto counts = pool.schemaStatistics();
    size_t featureTypes = 0, relations = 0;
    for (auto const& layer : layers) {
        featureTypes += layer.features.size();
        relations += layer.relations.size();
    }
    memorySummary = pool.memoryUsage();
    statistics = {
        {"counts",
         {{"types", types.empty() ? 0 : types.size() - 1},
          {"physicalTypes", types.empty() ? 0 : types.size() - 1 - queryRoots.size()},
          {"queryContexts", queryRoots.size()},
          {"fields", counts.fields},
          {"fieldEdges", counts.fieldEdges},
          {"contextFields", counts.contextFields},
          {"elementEdges", counts.elementEdges},
          {"alternativeEdges", counts.alternativeEdges},
          {"layers", layers.size()},
          {"featureTypes", featureTypes},
          {"relationDeclarations", relations},
          {"internedNames", pool.strings()->size()}}},
        {"limits",
         {{"typeId", simfil::MaxTypeId}, {"nameId", std::numeric_limits<StringId>::max()}}},
        {"memory", memorySummary->toJson()},
        {"constituents", pool.constituents()}};
}

nlohmann::json MapSchema::constituents() const
{
    auto result = nlohmann::json::array();
    for (auto const& [identity, format] : schema_->constituents)
        result.push_back({{"schemaId", identity}, {"mapFormat", format.toJson()}});
    return result;
}

nlohmann::json MapSchema::statistics() const
{
    if (!finalized())
        throw std::logic_error("MapSchema statistics require a finalized graph.");
    return schema_->statistics;
}

std::span<TypeId const> MapSchema::schemaRemap(std::string_view sourceIdentity) const
{
    if (!finalized())
        throw std::logic_error("Schema translations require a finalized artifact.");
    auto found = schema_->contributorRemaps.find(sourceIdentity);
    if (found == schema_->contributorRemaps.end())
        throw std::invalid_argument(
            "No construction-time translation for the source schema artifact.");
    return found->second;
}

MapSchema::ConstPtr MapSchema::compose(MapFormat format, std::span<ConstPtr const> inputs)
{
    format = MapFormat::fromJson(format.toJson());
    if (inputs.empty() || inputs.size() > 256)
        throw std::invalid_argument("Schema composition requires 1..256 immutable constituents.");
    std::vector<ConstPtr> owners;
    std::set<std::string_view> identities;
    for (auto const& owner : inputs) {
        if (!owner || !owner->finalized())
            throw std::invalid_argument("Schema composition requires finalized input owners.");
        if (identities.insert(owner->schemaId()).second)
            owners.push_back(owner);
    }
    if (owners.size() == 1 && owners.front()->format() == format)
        return owners.front();
    auto key = "compose-v1:" + format.toJson().dump();
    for (auto const& owner : owners)
        key += ":" + std::string(owner->schemaId());
    // Do not share this cache key with the native reader: decoded artifacts have
    // the same content identity but deliberately no producer translation tables.
    return getOrBuild(
        std::move(key),
        [format = std::move(format), owners = std::move(owners)]
        {
            auto result = std::make_shared<MapSchema>(format);
            Impl::CompositionBuilder(*result->schema_, owners).build();
            result->finalize();
            return result;
        });
}

TypeId MapSchema::remapSchema(std::string_view sourceIdentity, TypeId id) const
{
    if (sourceIdentity == schemaId()) {
        if (id && !schema_->valid(id))
            throw std::invalid_argument("Source type is absent from the effective artifact.");
        return id;
    }
    auto remap = schemaRemap(sourceIdentity);
    if (id >= remap.size() || (id && !remap[id]))
        throw std::invalid_argument("Source type is absent from the contributor translation.");
    return remap[id];
}
MemoryUsageBreakdown LayerSchema::memoryUsage() const
{
    // This is a borrowing view. Unique-owner accounting belongs to MapSchema.
    MemoryUsageBreakdown result;
    result.add("view", {sizeof(*this), sizeof(*this)});
    return result;
}

}  // namespace mapget
