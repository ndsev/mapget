#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "simfil/model/schema.h"

namespace mapget
{

/**
 * Stable schema registry built from a LayerInfo JSON Schema.
 *
 * The JSON Schema remains the authoritative interchange format. This registry
 * extracts x-mapget annotated object/array branches that SIMFIL needs for
 * wildcard field pruning and assigns deterministic SchemaId values by schema
 * traversal order. It deliberately does not own or mutate a StringPool; runtime
 * SIMFIL integration resolves StringIds back to datasource-owned strings.
 */
class SchemaRegistry
{
public:
    /** Opaque storage for compiled schemas and lookup tables. */
    struct Impl;

    /** Human-readable registry entry for a compiled JSON Schema branch. */
    struct Entry
    {
        simfil::SchemaId id_ = simfil::NoSchemaId;
        std::string key_;
        std::string jsonPointer_;
        std::string metaType_;
    };

    /** Parse all supported schema branches and assign deterministic SchemaIds. */
    explicit SchemaRegistry(nlohmann::json const& schema);

    /** Convenience factory returning nullptr for a missing/null schema. */
    [[nodiscard]] static std::shared_ptr<SchemaRegistry> fromJson(nlohmann::json const& schema);

    /** Return an entry by exact key, or by feature type name as a fallback. */
    [[nodiscard]] Entry const* getSchema(std::string_view keyOrFeatureType) const;

    /** Resolve a registry key to the serialized SchemaId domain. */
    [[nodiscard]] simfil::SchemaId schemaId(std::string_view key) const;

    /** Return the kind of a compiled schema, defaulting to Object for unknown ids. */
    [[nodiscard]] simfil::Schema::Kind kind(simfil::SchemaId schemaId) const;

    /** Return true if the schema can contain the field directly or through descendants. */
    [[nodiscard]] bool canHaveField(simfil::SchemaId schemaId, std::string_view fieldName) const;

    /** Return true if the schema can contain the enum-like string symbol through descendants. */
    [[nodiscard]] bool canHaveEnumSymbol(simfil::SchemaId schemaId, std::string_view symbolName) const;

    /** Resolve the Feature object schema for a concrete mapget feature type. */
    [[nodiscard]] simfil::SchemaId featureSchema(std::string_view featureType) const;

    /** Resolve the Feature.properties object schema for a concrete feature type. */
    [[nodiscard]] simfil::SchemaId featurePropertiesSchema(std::string_view featureType) const;

    /** Resolve the Feature.properties.layer object schema for a concrete feature type. */
    [[nodiscard]] simfil::SchemaId attributeLayerMapSchema(std::string_view featureType) const;

    /** Resolve a child schema from a parent object/array schema and field name. */
    [[nodiscard]] simfil::SchemaId childSchema(
        simfil::SchemaId parent,
        std::string_view fieldName,
        std::optional<simfil::Schema::Kind> preferredKind = std::nullopt) const;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace mapget
