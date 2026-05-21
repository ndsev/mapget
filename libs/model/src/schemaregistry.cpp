#include "mapget/model/schemaregistry.h"
#include "mapget/model/simfilutil.h"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace mapget
{

namespace
{

using Kind = simfil::Schema::Kind;

/** Escape one JSON Pointer path token. */
std::string pointerToken(std::string_view token)
{
    std::string result;
    result.reserve(token.size());
    for (char c : token) {
        if (c == '~') {
            result += "~0";
        }
        else if (c == '/') {
            result += "~1";
        }
        else {
            result += c;
        }
    }
    return result;
}

/** Return true if a JSON Schema `type` accepts the requested scalar. */
bool hasType(nlohmann::json const& schema, std::string_view type)
{
    auto typeIt = schema.find("type");
    if (typeIt == schema.end()) {
        return false;
    }
    if (typeIt->is_string()) {
        return typeIt->get_ref<const std::string&>() == type;
    }
    if (typeIt->is_array()) {
        return std::any_of(typeIt->begin(), typeIt->end(), [&](auto const& item) {
            return item.is_string() && item.template get_ref<const std::string&>() == type;
        });
    }
    return false;
}

/** Return the x-mapget metadata object if present. */
nlohmann::json const* mapgetMetadata(nlohmann::json const& schema)
{
    auto it = schema.find("x-mapget");
    if (it == schema.end() || !it->is_object()) {
        return nullptr;
    }
    return &*it;
}

/** Read a string field from a metadata object. */
std::string metadataString(nlohmann::json const* metadata, std::string_view key)
{
    if (!metadata) {
        return {};
    }
    auto it = metadata->find(std::string(key));
    if (it == metadata->end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/** Local traversal context used to derive stable x-mapget keys. */
struct BuildContext
{
    std::string featureType_;
    std::string attributeLayerName_;
};

/** Return the first JSON Schema combiner present on the object. */
std::string_view combinerKey(nlohmann::json const& schema)
{
    for (std::string_view key : {"oneOf", "anyOf", "allOf"}) {
        auto it = schema.find(std::string(key));
        if (it != schema.end() && it->is_array()) {
            return key;
        }
    }
    return {};
}

/** Return whether this schema branch should be treated as an object schema. */
bool isObjectSchema(nlohmann::json const& schema)
{
    return schema.contains("properties") || hasType(schema, "object");
}

/** Return whether this schema branch should be treated as an array schema. */
bool isArraySchema(nlohmann::json const& schema)
{
    return schema.contains("items") || hasType(schema, "array");
}

/** Return whether a oneOf object/array wrapper represents a mapget multimap view. */
bool isMapgetMultimap(nlohmann::json const& schema)
{
    auto it = schema.find("x-mapget-multimap");
    return it != schema.end() && it->is_boolean() && it->get<bool>();
}

/** Build a registry key from known x-mapget annotations. */
std::string annotatedKey(
    nlohmann::json const& schema,
    std::string_view pointer,
    BuildContext const& context)
{
    auto const* metadata = mapgetMetadata(schema);
    auto explicitKey = metadataString(metadata, "schemaKey");
    if (!explicitKey.empty()) {
        return explicitKey;
    }

    auto metaType = metadataString(metadata, "metaType");
    auto featureType = metadataString(metadata, "featureType");
    if (featureType.empty()) {
        featureType = context.featureType_;
    }

    if (metaType == "Feature" && !featureType.empty()) {
        return "Feature:" + featureType;
    }
    if (metaType == "FeatureProperties" && !featureType.empty()) {
        return "FeatureProperties:" + featureType;
    }
    if (metaType == "AttributeLayerMap" && !featureType.empty()) {
        return "AttributeLayerMap:" + featureType;
    }
    if (metaType == "AttributeContainer" && !featureType.empty() && !context.attributeLayerName_.empty()) {
        return "AttributeContainer:" + featureType + ":" + context.attributeLayerName_;
    }
    if (metaType == "Attribute" && !featureType.empty() && !context.attributeLayerName_.empty()) {
        auto attributeTypeCode = metadataString(metadata, "attributeTypeCode");
        if (!attributeTypeCode.empty()) {
            return "Attribute:" + featureType + ":" + context.attributeLayerName_ + ":" + attributeTypeCode;
        }
    }

    return std::string(pointer);
}

/** Resolve local JSON Schema references of the form `#/...`. */
nlohmann::json const& resolveLocalRef(nlohmann::json const& root, std::string const& ref)
{
    if (ref.empty() || ref.front() != '#') {
        throw std::runtime_error("SchemaRegistry only supports local JSON Schema references.");
    }
    auto pointer = ref.substr(1);
    return root.at(nlohmann::json::json_pointer(pointer));
}

/** Convert a local JSON Schema reference into a canonical registry pointer. */
std::string refToPointer(std::string const& ref)
{
    return ref.empty() || ref.front() != '#' ? ref : ref;
}

/** Return the deterministic key used by feature schema annotations. */
std::string featureKey(std::string_view featureType)
{
    return "Feature:" + std::string(featureType);
}

/** Return the deterministic key used by Feature.properties annotations. */
std::string featurePropertiesKey(std::string_view featureType)
{
    return "FeatureProperties:" + std::string(featureType);
}

/** Return the deterministic key used by Feature.properties.layer annotations. */
std::string attributeLayerMapKey(std::string_view featureType)
{
    return "AttributeLayerMap:" + std::string(featureType);
}

} // namespace

struct SchemaRegistry::Impl
{
    /** Logical object/array schema independent of any StringPool numbering. */
    struct LogicalSchema
    {
        simfil::SchemaId id_ = simfil::NoSchemaId;
        Kind kind_ = Kind::Object;
        Entry entry_;
        std::vector<std::string> directFields_;
        std::map<std::string, std::vector<simfil::SchemaId>, std::less<>> childSchemas_;
        std::vector<simfil::SchemaId> elementSchemas_;
        std::vector<std::string> flatFields_;
        bool finalized_ = false;
    };

    Impl()
    {
        schemas_.push_back({});
        entriesById_.push_back({});
    }

    std::vector<LogicalSchema> schemas_;
    std::vector<Entry> entriesById_;
    std::map<std::string, simfil::SchemaId, std::less<>> idsByKey_;

    [[nodiscard]] bool valid(simfil::SchemaId id) const
    {
        return id != simfil::NoSchemaId && id < schemas_.size();
    }

    [[nodiscard]] Kind kind(simfil::SchemaId id) const
    {
        return valid(id) ? schemas_[id].kind_ : Kind::Object;
    }

    simfil::SchemaId allocate(Kind kind, std::string key, std::string pointer, std::string metaType)
    {
        if (schemas_.size() > simfil::MaxSchemaId) {
            throw std::runtime_error("SchemaRegistry exhausted the uint16 SchemaId domain.");
        }

        auto id = static_cast<simfil::SchemaId>(schemas_.size());
        Entry entry{id, key, pointer, std::move(metaType)};
        LogicalSchema schema;
        schema.id_ = id;
        schema.kind_ = kind;
        schema.entry_ = entry;
        schemas_.push_back(std::move(schema));
        entriesById_.push_back(std::move(entry));
        registerKey(key, id);
        registerKey(pointer, id);
        return id;
    }

    void registerKey(std::string const& key, simfil::SchemaId id)
    {
        if (!key.empty() && id != simfil::NoSchemaId) {
            idsByKey_.emplace(key, id);
        }
    }

    void addDirectField(simfil::SchemaId parent, std::string_view fieldName)
    {
        if (!valid(parent)) {
            return;
        }
        auto& fields = schemas_[parent].directFields_;
        fields.emplace_back(fieldName);
    }

    void addChild(simfil::SchemaId parent, std::string_view fieldName, simfil::SchemaId child)
    {
        if (!valid(parent) || !valid(child)) {
            return;
        }
        auto& children = schemas_[parent].childSchemas_[std::string(fieldName)];
        if (std::ranges::find(children, child) == children.end()) {
            children.push_back(child);
        }
    }

    void addElementSchema(simfil::SchemaId parent, simfil::SchemaId child)
    {
        if (!valid(parent) || !valid(child)) {
            return;
        }
        auto& children = schemas_[parent].elementSchemas_;
        if (std::ranges::find(children, child) == children.end()) {
            children.push_back(child);
        }
    }

    void finalizeAll()
    {
        for (size_t id = 1; id < schemas_.size(); ++id) {
            finalize(static_cast<simfil::SchemaId>(id));
        }
    }

    void finalize(simfil::SchemaId id)
    {
        if (!valid(id) || schemas_[id].finalized_) {
            return;
        }

        std::vector<std::string> fields;
        std::vector<simfil::SchemaId> visited;
        collectFields(id, visited, fields);
        std::ranges::sort(fields);
        auto duplicates = std::ranges::unique(fields);
        fields.erase(duplicates.begin(), duplicates.end());
        schemas_[id].flatFields_ = std::move(fields);
        schemas_[id].finalized_ = true;
    }

    void collectFields(
        simfil::SchemaId id,
        std::vector<simfil::SchemaId>& visited,
        std::vector<std::string>& fields) const
    {
        if (!valid(id) || std::ranges::find(visited, id) != visited.end()) {
            return;
        }
        visited.push_back(id);

        auto const& schema = schemas_[id];
        fields.insert(fields.end(), schema.directFields_.begin(), schema.directFields_.end());
        for (auto const& [_, children] : schema.childSchemas_) {
            for (auto child : children) {
                collectFields(child, visited, fields);
            }
        }
        for (auto child : schema.elementSchemas_) {
            collectFields(child, visited, fields);
        }
    }

    [[nodiscard]] bool canHaveField(simfil::SchemaId id, std::string_view fieldName)
    {
        if (!valid(id)) {
            return true;
        }
        finalize(id);
        auto const& fields = schemas_[id].flatFields_;
        return std::ranges::binary_search(fields, fieldName);
    }

    [[nodiscard]] simfil::SchemaId childSchema(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<Kind> preferredKind) const
    {
        std::vector<simfil::SchemaId> visited;
        return childSchema(parent, fieldName, preferredKind, visited);
    }

    [[nodiscard]] simfil::SchemaId childSchema(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<Kind> preferredKind,
        std::vector<simfil::SchemaId>& visited) const
    {
        if (!valid(parent)) {
            return simfil::NoSchemaId;
        }
        if (std::ranges::find(visited, parent) != visited.end()) {
            return simfil::NoSchemaId;
        }
        visited.push_back(parent);

        auto const& schema = schemas_[parent];
        if (schema.kind_ == Kind::Array) {
            for (auto child : schema.elementSchemas_) {
                auto resolved = childSchema(child, fieldName, preferredKind, visited);
                if (resolved != simfil::NoSchemaId) {
                    return resolved;
                }
            }
            return simfil::NoSchemaId;
        }

        auto fieldIt = schema.childSchemas_.find(fieldName);
        if (fieldIt == schema.childSchemas_.end()) {
            return simfil::NoSchemaId;
        }

        for (auto id : fieldIt->second) {
            if (!preferredKind || kind(id) == *preferredKind) {
                return id;
            }
        }
        return fieldIt->second.empty() ? simfil::NoSchemaId : fieldIt->second.front();
    }
};

namespace
{

/** Recursive JSON Schema compiler used by SchemaRegistry construction. */
class RegistryBuilder
{
public:
    RegistryBuilder(SchemaRegistry::Impl& registry, nlohmann::json const& root)
        : registry_(registry), root_(root)
    {
    }

    void buildAll()
    {
        build(root_, "#", {}, std::nullopt);
        registry_.finalizeAll();
    }

private:
    simfil::SchemaId build(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::optional<Kind> preferredKind)
    {
        if (schema.is_boolean() || !schema.is_object()) {
            return simfil::NoSchemaId;
        }

        auto refIt = schema.find("$ref");
        if (refIt != schema.end() && refIt->is_string()) {
            auto ref = refIt->get<std::string>();
            return build(resolveLocalRef(root_, ref), refToPointer(ref), std::move(context), preferredKind);
        }

        if (auto combiner = combinerKey(schema); !combiner.empty()) {
            return buildCombined(schema, std::string(combiner), std::move(pointer), std::move(context), preferredKind);
        }

        auto* metadata = mapgetMetadata(schema);
        auto metaType = metadataString(metadata, "metaType");
        auto featureType = metadataString(metadata, "featureType");
        if (!featureType.empty()) {
            context.featureType_ = std::move(featureType);
        }

        auto const key = annotatedKey(schema, pointer, context);
        auto const memoKey = pointer + "|" + (preferredKind ? (*preferredKind == Kind::Object ? "o" : "a") : "n");
        if (auto memoIt = memo_.find(memoKey); memoIt != memo_.end()) {
            registry_.registerKey(key, memoIt->second);
            return memoIt->second;
        }

        if (isArraySchema(schema) && (!preferredKind || *preferredKind == Kind::Array) && !isObjectSchema(schema)) {
            return buildArray(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        if (isObjectSchema(schema) && (!preferredKind || *preferredKind == Kind::Object)) {
            return buildObject(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        if (isArraySchema(schema) && (!preferredKind || *preferredKind == Kind::Array)) {
            return buildArray(schema, std::move(pointer), std::move(context), std::move(key), std::move(metaType), memoKey);
        }
        return simfil::NoSchemaId;
    }

    simfil::SchemaId buildCombined(
        nlohmann::json const& schema,
        std::string const& combiner,
        std::string pointer,
        BuildContext context,
        std::optional<Kind> preferredKind)
    {
        auto const& branches = schema.at(combiner);
        std::vector<simfil::SchemaId> branchIds;
        branchIds.reserve(branches.size());
        for (size_t i = 0; i < branches.size(); ++i) {
            auto branchPointer = pointer + "/" + combiner + "/" + std::to_string(i);
            branchIds.push_back(build(branches[i], branchPointer, context, preferredKind));
        }

        auto choose = [&](std::optional<Kind> kind) {
            for (auto id : branchIds) {
                if (id != simfil::NoSchemaId && (!kind || registry_.kind(id) == *kind)) {
                    return id;
                }
            }
            return simfil::NoSchemaId;
        };

        if (auto selected = choose(preferredKind); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        if (auto selected = choose(Kind::Object); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        if (auto selected = choose(Kind::Array); selected != simfil::NoSchemaId) {
            registerCombinedAliases(schema, pointer, context, selected);
            return selected;
        }
        return simfil::NoSchemaId;
    }

    void registerCombinedAliases(
        nlohmann::json const& schema,
        std::string const& pointer,
        BuildContext const& context,
        simfil::SchemaId selected)
    {
        registry_.registerKey(pointer, selected);
        auto key = annotatedKey(schema, pointer, context);
        registry_.registerKey(key, selected);
    }

    simfil::SchemaId buildObject(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::string key,
        std::string metaType,
        std::string const& memoKey)
    {
        auto id = registry_.allocate(
            Kind::Object,
            std::move(key),
            pointer,
            metaType);
        memo_[memoKey] = id;

        auto propertiesIt = schema.find("properties");
        if (propertiesIt == schema.end() || !propertiesIt->is_object()) {
            return id;
        }

        for (auto const& [fieldName, childSchemaJson] : propertiesIt->items()) {
            registry_.addDirectField(id, fieldName);

            auto childContext = context;
            if (metaType == "AttributeLayerMap") {
                childContext.attributeLayerName_ = fieldName;
            }

            auto const childPointer = pointer + "/properties/" + pointerToken(fieldName);
            auto addChildSchema = [&](std::optional<Kind> preferredKind) {
                auto childId = build(
                    childSchemaJson,
                    childPointer,
                    childContext,
                    preferredKind);
                if (childId != simfil::NoSchemaId) {
                    registry_.addChild(id, fieldName, childId);
                }
            };

            if (isMapgetMultimap(childSchemaJson)) {
                // Simfil sees the logical object view. The array branch is only
                // a JSON serialization detail for duplicate field names.
                addChildSchema(Kind::Object);
            }
            else {
                addChildSchema(std::nullopt);
            }
        }
        return id;
    }

    simfil::SchemaId buildArray(
        nlohmann::json const& schema,
        std::string pointer,
        BuildContext context,
        std::string key,
        std::string metaType,
        std::string const& memoKey)
    {
        auto id = registry_.allocate(
            Kind::Array,
            std::move(key),
            pointer,
            std::move(metaType));
        memo_[memoKey] = id;

        auto itemsIt = schema.find("items");
        if (itemsIt == schema.end()) {
            return id;
        }

        auto childId = build(*itemsIt, pointer + "/items", std::move(context), std::nullopt);
        if (childId != simfil::NoSchemaId) {
            registry_.addElementSchema(id, childId);
        }
        return id;
    }

    SchemaRegistry::Impl& registry_;
    nlohmann::json const& root_;
    std::map<std::string, simfil::SchemaId> memo_;
};

/** SIMFIL schema adapter that resolves StringIds without inserting schema field names. */
class BoundSchema final : public simfil::Schema
{
public:
    BoundSchema(
        std::shared_ptr<SchemaRegistry const> registry,
        std::shared_ptr<simfil::StringPool const> strings,
        simfil::SchemaId id)
        : registry_(std::move(registry)), strings_(std::move(strings)), id_(id)
    {
    }

    /** Return the object/array kind for the stable mapget SchemaId. */
    auto kind() const -> Kind override
    {
        return registry_ ? registry_->kind(id_) : Kind::Object;
    }

    /** Resolve the field id through the datasource-owned pool and match by name. */
    auto canHaveField(simfil::StringId fieldId) const -> bool override
    {
        if (!registry_ || !strings_) {
            return true;
        }
        auto fieldName = strings_->resolve(fieldId);
        return !fieldName || registry_->canHaveField(id_, *fieldName);
    }

    /** This adapter intentionally avoids materializing StringIds for schema-only field names. */
    auto nestedFields() const& -> std::span<const simfil::StringId> override
    {
        return {};
    }

private:
    /** Runtime pruning calls canHaveField directly, so no recursive StringId cache is built here. */
    auto collectNestedFields(
        const std::function<Schema*(simfil::SchemaId)>&,
        SchemaIdStack&,
        std::vector<simfil::StringId>&) const -> void override
    {
    }

    std::shared_ptr<SchemaRegistry const> registry_;
    std::shared_ptr<simfil::StringPool const> strings_;
    simfil::SchemaId id_ = simfil::NoSchemaId;
};

} // namespace

SchemaRegistry::SchemaRegistry(nlohmann::json const& schema)
    : impl_(std::make_shared<Impl>())
{
    if (!schema.is_null()) {
        RegistryBuilder(*impl_, schema).buildAll();
    }
}

std::shared_ptr<SchemaRegistry> SchemaRegistry::fromJson(nlohmann::json const& schema)
{
    if (schema.is_null()) {
        return nullptr;
    }
    return std::make_shared<SchemaRegistry>(schema);
}

SchemaRegistry::Entry const* SchemaRegistry::getSchema(std::string_view keyOrFeatureType) const
{
    auto exact = impl_->idsByKey_.find(keyOrFeatureType);
    auto id = exact == impl_->idsByKey_.end() ? featureSchema(keyOrFeatureType) : exact->second;
    if (id == simfil::NoSchemaId || id >= impl_->entriesById_.size()) {
        return nullptr;
    }
    return &impl_->entriesById_[id];
}

simfil::SchemaId SchemaRegistry::schemaId(std::string_view key) const
{
    auto it = impl_->idsByKey_.find(key);
    if (it == impl_->idsByKey_.end()) {
        return simfil::NoSchemaId;
    }
    return it->second;
}

simfil::Schema::Kind SchemaRegistry::kind(simfil::SchemaId schemaId) const
{
    return impl_->kind(schemaId);
}

bool SchemaRegistry::canHaveField(simfil::SchemaId schemaId, std::string_view fieldName) const
{
    return impl_->canHaveField(schemaId, fieldName);
}

simfil::SchemaId SchemaRegistry::featureSchema(std::string_view featureType) const
{
    return schemaId(featureKey(featureType));
}

simfil::SchemaId SchemaRegistry::featurePropertiesSchema(std::string_view featureType) const
{
    return schemaId(featurePropertiesKey(featureType));
}

simfil::SchemaId SchemaRegistry::attributeLayerMapSchema(std::string_view featureType) const
{
    return schemaId(attributeLayerMapKey(featureType));
}

simfil::SchemaId SchemaRegistry::childSchema(
    simfil::SchemaId parent,
    std::string_view fieldName,
    std::optional<simfil::Schema::Kind> preferredKind) const
{
    return impl_->childSchema(parent, fieldName, preferredKind);
}

void installSchemaRegistry(
    simfil::Environment& env,
    std::shared_ptr<SchemaRegistry const> registry,
    std::shared_ptr<simfil::StringPool const> strings)
{
    auto schemas = std::make_shared<std::map<simfil::SchemaId, std::unique_ptr<BoundSchema>>>();
    env.querySchemaCallback = [registry = std::move(registry),
                               strings = std::move(strings),
                               schemas = std::move(schemas)](simfil::SchemaId schemaId) {
        if (!registry || schemaId == simfil::NoSchemaId) {
            return static_cast<simfil::Schema const*>(nullptr);
        }
        auto [it, inserted] = schemas->try_emplace(schemaId);
        if (inserted) {
            it->second = std::make_unique<BoundSchema>(registry, strings, schemaId);
        }
        return static_cast<simfil::Schema const*>(it->second.get());
    };
}

} // namespace mapget
