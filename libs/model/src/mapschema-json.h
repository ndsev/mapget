#pragma once

#include "mapget/model/layerschema.h"
#include "simfil/model/schema-type.h"

namespace mapget::schema_detail
{
using Kind = simfil::SchemaType::Kind;
using Composition = simfil::SchemaType::Composition;

/** Interpret structural affinity without depending on an application kind's name. */
bool hasAffinity(Kind kind, simfil::ValueType type);

/** Match structural selectors while retaining precise scalar kinds. */
bool matchesKind(Kind kind, Kind requested);

/** Recognize a concrete bitmask, allowing null but not mixed scalar affinities. */
bool isBitmaskKind(Kind kind);

/** Escape one JSON Pointer path component. */
std::string pointerToken(std::string_view token);

/** Borrow mapget annotations when the input has the expected object shape. */
nlohmann::json const* mapgetMetadata(nlohmann::json const& schema);

/** Read an optional textual annotation without inventing a value for other types. */
std::string metadataString(nlohmann::json const* metadata, std::string_view key);

/** Exclude Attribute infrastructure from shorthand scalar-value discovery. */
bool isAttributeScalarShorthandMetadataField(std::string_view fieldName);

/** Decode the stable named kind encoding or the explicit historical packed layout. */
std::optional<Kind> jsonKind(nlohmann::json const* metadata, simfil::StringPool const& strings);

/** Export names rather than process-local dictionary or enum numbers. */
nlohmann::json exportKind(simfil::SchemaType const& type);

/** Bound traversal before recursive input copying, hashing, or compilation. */
void validateJsonSchema(nlohmann::json const& root);

/** Keep validation and descriptive metadata, not a second structural schema graph. */
nlohmann::json jsonSchemaAnnotations(nlohmann::json const& schema);

/** Visit only schema-valued validation constraints, never literal const/enum data.
 * The suffix is relative to the enclosing schema and retains escaped map keys.
 * Canonical callers replace each visited object with a graph-local reference. */
template <class F>
void forEachValidationSchema(nlohmann::json& annotations, F&& visit)
{
    for (auto key :
         {"if",
          "then",
          "else",
          "not",
          "contains",
          "additionalProperties",
          "additionalItems",
          "propertyNames"})
    {
        if (auto it = annotations.find(key); it != annotations.end() && it->is_object())
            visit(*it, std::string("/") + key);
    }
    for (auto key : {"patternProperties", "dependencies"}) {
        if (auto it = annotations.find(key); it != annotations.end() && it->is_object()) {
            for (auto& [name, schema] : it->items()) {
                if (schema.is_object())
                    visit(schema, std::string("/") + key + "/" + pointerToken(name));
            }
        }
    }
}
}  // namespace mapget::schema_detail
