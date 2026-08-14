#include "mapget/model/geojson-import.h"

#include "mapget/model/attr.h"
#include "mapget/model/attrlayer.h"
#include "mapget/model/attrpoint.h"
#include "mapget/model/feature.h"
#include "mapget/model/featureid.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/geometry.h"
#include "mapget/model/relation.h"
#include "mapget/model/sourcedatareference.h"
#include "mapget/model/validity.h"
#include "mapget/log.h"
#include "simfil/model/json.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace mapget
{

namespace
{
using AttrPointSequenceRegistry = std::vector<model_ptr<AttrPointSequence>>;

/** Raise a consistently prefixed import error. */
[[noreturn]] void raiseImport(std::string const& message)
{
    raiseFmt("TileFeatureLayer::fromJson: {}", message);
}

/** Parse mapget validity direction labels, including legacy aliases. */
[[nodiscard]] std::optional<Validity::Direction> directionFromJson(nlohmann::json const& json)
{
    if (!json.is_string()) {
        return std::nullopt;
    }

    auto const value = json.get<std::string>();
    if (value == "POSITIVE") {
        return Validity::Positive;
    }
    if (value == "NEGATIVE") {
        return Validity::Negative;
    }
    if (value == "COMPLETE" || value == "BOTH") {
        return Validity::Both;
    }
    if (value == "NONE") {
        return Validity::None;
    }
    if (value == "EMPTY") {
        return Validity::Empty;
    }
    return std::nullopt;
}

/** Parse transition endpoint labels used by feature-transition validities. */
[[nodiscard]] std::optional<Validity::TransitionEnd> transitionEndFromJson(nlohmann::json const& json)
{
    if (!json.is_string()) {
        return std::nullopt;
    }
    auto const value = json.get<std::string>();
    if (value == "START") {
        return Validity::Start;
    }
    if (value == "END") {
        return Validity::End;
    }
    return std::nullopt;
}

/** Parse the offset encoding used by position/range validities. */
[[nodiscard]] std::optional<Validity::GeometryOffsetType> offsetTypeFromJson(nlohmann::json const& json)
{
    if (!json.is_string()) {
        return std::nullopt;
    }
    auto const value = json.get<std::string>();
    if (value == "GeoPosOffset") {
        return Validity::GeoPosOffset;
    }
    if (value == "BufferOffset") {
        return Validity::BufferOffset;
    }
    if (value == "RelativeLengthOffset") {
        return Validity::RelativeLengthOffset;
    }
    if (value == "MetricLengthOffset") {
        return Validity::MetricLengthOffset;
    }
    return std::nullopt;
}

/** Parse a GeoJSON position into mapget's Point type. */
[[nodiscard]] Point pointFromCoordinateJson(nlohmann::json const& json)
{
    if (!json.is_array() || json.size() < 2 || json.size() > 3) {
        raiseImport("Expected GeoJSON coordinate array with 2 or 3 numeric elements.");
    }
    return Point{
        json.at(0).get<double>(),
        json.at(1).get<double>(),
        json.size() >= 3 ? json.at(2).get<double>() : 0.0};
}

/** Resolve a compact `$mapgetAttrPointSequence` token against imported definitions. */
[[nodiscard]] model_ptr<AttrPointSequence> importAttrPointSequenceReference(
    nlohmann::json const& json,
    AttrPointSequenceRegistry const& sequences)
{
    if (!json.is_object() || json.size() != 1 ||
        !json.contains("$mapgetAttrPointSequence") ||
        !json.at("$mapgetAttrPointSequence").is_number_integer())
    {
        raiseImport(
            "AttrPointSequence reference must be encoded as "
            "{'$mapgetAttrPointSequence': <non-negative integer>}.");
    }
    auto const encodedIndex = json.at("$mapgetAttrPointSequence").get<int64_t>();
    if (encodedIndex < 0 ||
        static_cast<uint64_t>(encodedIndex) > std::numeric_limits<uint32_t>::max())
    {
        raiseImport("AttrPointSequence reference index is outside uint32 range.");
    }
    auto const index = static_cast<uint32_t>(encodedIndex);
    if (index >= sequences.size()) {
        raiseImport(fmt::format(
            "AttrPointSequence reference {} is out of range ({} definitions).",
            index,
            sequences.size()));
    }
    return sequences[index];
}

/** Decode a JSON scalar into the storage type expected by an id-part definition. */
[[nodiscard]] std::variant<int64_t, std::string> jsonToIdPartValue(
    nlohmann::json const& json,
    IdPart const& idPart)
{
    if (json.is_number_integer()) {
        return json.get<int64_t>();
    }
    if (json.is_number_unsigned()) {
        auto const value = json.get<uint64_t>();
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            raiseImport(fmt::format("Id part '{}' exceeds int64_t range.", idPart.idPartLabel_));
        }
        return static_cast<int64_t>(value);
    }
    if (json.is_string()) {
        return json.get<std::string>();
    }
    raiseImport(fmt::format(
        "Id part '{}' must be encoded as an integer or string JSON value.",
        idPart.idPartLabel_));
}

/** Validate an id-part value with the same rules used by binary feature creation. */
void validateIdPartValue(
    IdPart const& idPart,
    std::variant<int64_t, std::string>& value)
{
    std::string error;
    if (!idPart.validate(value, &error)) {
        raiseImport(error);
    }
}

/** Detect compact references to a feature-local top-level relation. */
[[nodiscard]] bool isRelationReferenceJson(nlohmann::json const& json)
{
    return json.is_object() &&
           json.size() == 1 &&
           json.contains("$mapgetRelation");
}

/** Recursively check whether generic JSON import needs mapget-specific relation-reference handling. */
[[nodiscard]] bool containsRelationReferenceJson(nlohmann::json const& json)
{
    if (isRelationReferenceJson(json)) {
        return true;
    }
    if (json.is_array()) {
        return std::any_of(
            json.begin(),
            json.end(),
            [](auto const& child) { return containsRelationReferenceJson(child); });
    }
    if (json.is_object()) {
        return std::any_of(
            json.begin(),
            json.end(),
            [](auto const& child) { return containsRelationReferenceJson(child); });
    }
    return false;
}

/** Delegate generic object/array/scalar import to simfil's shared JSON builder unless relation refs require feature context. */
[[nodiscard]] simfil::ModelNode::Ptr importGenericNode(
    TileFeatureLayer& tile,
    nlohmann::json const& json,
    model_ptr<Feature> const& feature = {})
{
    if (isRelationReferenceJson(json)) {
        if (!feature) {
            raiseImport("$mapgetRelation can only be imported inside feature properties.");
        }
        auto const& relationIndexJson = json.at("$mapgetRelation");
        if (!relationIndexJson.is_number_integer() && !relationIndexJson.is_number_unsigned()) {
            raiseImport("$mapgetRelation must be encoded as an integer.");
        }
        uint64_t relationIndex = 0;
        if (relationIndexJson.is_number_unsigned()) {
            relationIndex = relationIndexJson.get<uint64_t>();
        }
        else {
            auto const signedIndex = relationIndexJson.get<int64_t>();
            if (signedIndex < 0) {
                raiseImport("$mapgetRelation must not be negative.");
            }
            relationIndex = static_cast<uint64_t>(signedIndex);
        }
        if (relationIndex >= Relation::InvalidFeatureRelationIndex) {
            raiseImport("$mapgetRelation exceeds the supported relation-reference range.");
        }
        auto relation = feature->getRelation(static_cast<uint32_t>(relationIndex));
        if (!relation) {
            raiseImport(fmt::format("Feature has no relation at index {}.", relationIndex));
        }
        return tile.newRelationReference(relation);
    }

    if (containsRelationReferenceJson(json)) {
        if (json.is_array()) {
            auto array = tile.newArray(json.size(), true);
            for (auto const& item : json) {
                array->append(importGenericNode(tile, item, feature));
            }
            return array;
        }
        if (json.is_object()) {
            auto object = tile.newObject(json.size());
            for (auto const& [key, value] : json.items()) {
                auto result = object->addField(key, importGenericNode(tile, value, feature));
                if (!result) {
                    raiseImport(result.error().message);
                }
            }
            return object;
        }
    }

    auto node = simfil::json::buildModelNode(json, tile);
    if (!node) {
        raiseImport(node.error().message);
    }
    return *node;
}

/** Find either `_sourceData` or the legacy `sourceData` alias on a JSON object. */
[[nodiscard]] nlohmann::json const* findSourceDataJson(nlohmann::json const& json)
{
    if (!json.is_object()) {
        return nullptr;
    }
    if (auto it = json.find("_sourceData"); it != json.end()) {
        return &(*it);
    }
    if (auto it = json.find("sourceData"); it != json.end()) {
        return &(*it);
    }
    return nullptr;
}

/** Read an optional logical geometry name without interpreting presentation stages. */
[[nodiscard]] std::optional<std::string> geometryNameFromJson(
    nlohmann::json const& json)
{
    if (!json.contains("geometryName")) {
        return std::nullopt;
    }
    if (!json.at("geometryName").is_string()) {
        raiseImport("geometryName must be a string.");
    }
    auto name = json.at("geometryName").get<std::string>();
    if (name.empty()) {
        raiseImport("geometryName must not be empty.");
    }
    return name;
}

/** Restrict best-effort synthetic ids to integer id-part slots. */
[[nodiscard]] bool isIntegerIdPart(IdPartDataType datatype)
{
    switch (datatype) {
    case IdPartDataType::I32:
    case IdPartDataType::U32:
    case IdPartDataType::I64:
    case IdPartDataType::U64:
        return true;
    case IdPartDataType::UUID128:
    case IdPartDataType::STR:
        return false;
    }
    return false;
}

/**
 * Synthesize full feature-id parts for permissive GeoJSON import.
 *
 * The tile id is injected when the composition contains a `tileId` part, and
 * the first remaining integer slot is filled with the feature's collection index.
 */
[[nodiscard]] KeyValuePairs bestEffortFullFeatureIdParts(
    TileFeatureLayer const& tile,
    std::string_view typeId,
    uint32_t fallbackFeatureIndex)
{
    auto const* typeInfo = tile.layerInfo()->getTypeInfo(typeId, false);
    if (!typeInfo || typeInfo->uniqueIdCompositions_.empty()) {
        raiseImport(fmt::format(
            "Could not resolve feature type '{}' for best-effort import.",
            typeId));
    }

    auto const& composition = typeInfo->uniqueIdCompositions_.front();
    KeyValuePairs parts;
    parts.reserve(composition.size());
    bool usedFeatureIndex = false;

    for (auto const& idPart : composition) {
        if (idPart.idPartLabel_ == "tileId") {
            if (!isIntegerIdPart(idPart.datatype_)) {
                raiseImport("Best-effort GeoJSON import requires an integer tileId id part.");
            }
            parts.emplace_back(idPart.idPartLabel_, static_cast<int64_t>(tile.tileId().value()));
            continue;
        }

        if (!usedFeatureIndex && isIntegerIdPart(idPart.datatype_)) {
            // Best-effort mode needs one deterministic per-feature slot to keep ids stable.
            parts.emplace_back(idPart.idPartLabel_, static_cast<int64_t>(fallbackFeatureIndex));
            usedFeatureIndex = true;
            continue;
        }

        if (idPart.isOptional_) {
            // Optional suffix parts may be dropped because no source field exists to recover them.
            continue;
        }

        raiseImport(fmt::format(
            "Could not synthesize best-effort feature ids for type '{}'. "
            "Provide explicit id fields for required part '{}'.",
            typeId,
            idPart.idPartLabel_));
    }

    if (!usedFeatureIndex) {
        raiseImport(fmt::format(
            "Could not synthesize best-effort feature ids for type '{}'. "
            "The primary id composition has no usable per-feature integer part.",
            typeId));
    }

    return parts;
}

/** Read all required id fields for strict import from the feature JSON object. */
[[nodiscard]] KeyValuePairs fullFeatureIdPartsFromFields(
    nlohmann::json const& featureJson,
    std::vector<IdPart> const& composition)
{
    KeyValuePairs result;
    result.reserve(composition.size());

    for (auto const& idPart : composition) {
        auto it = featureJson.find(idPart.idPartLabel_);
        if (it == featureJson.end()) {
            if (idPart.isOptional_) {
                continue;
            }
            raiseImport(fmt::format(
                "Feature is missing non-optional id field '{}' required by the primary composition.",
                idPart.idPartLabel_));
        }
        auto value = jsonToIdPartValue(*it, idPart);
        validateIdPartValue(idPart, value);
        result.emplace_back(idPart.idPartLabel_, std::move(value));
    }

    return result;
}

/** Import source-data references from the JSON representation used by toJson(). */
[[nodiscard]] std::optional<model_ptr<SourceDataReferenceCollection>> importSourceDataReferences(
    TileFeatureLayer& tile,
    nlohmann::json const& json)
{
    if (json.is_null()) {
        return std::nullopt;
    }
    if (!json.is_array()) {
        raiseImport("sourceData must be encoded as an array.");
    }

    std::vector<QualifiedSourceDataReference> refs;
    refs.reserve(json.size());
    for (auto const& item : json) {
        if (!item.is_object()) {
            raiseImport("Every sourceData entry must be an object.");
        }
        if (!item.contains("address") || !item.contains("layerId") || !item.contains("qualifier")) {
            raiseImport("Every sourceData entry must contain address, layerId, and qualifier.");
        }

        uint64_t address = 0;
        if (item.at("address").is_number_unsigned()) {
            address = item.at("address").get<uint64_t>();
        }
        else if (item.at("address").is_number_integer()) {
            auto const signedAddress = item.at("address").get<int64_t>();
            if (signedAddress < 0) {
                raiseImport("sourceData address must not be negative.");
            }
            address = static_cast<uint64_t>(signedAddress);
        }
        else {
            raiseImport("sourceData address must be an integer.");
        }

        auto layerId = item.at("layerId").get<std::string>();
        auto qualifier = item.at("qualifier").get<std::string>();
        auto layerIdId = tile.strings()->emplace(layerId);
        if (!layerIdId) {
            raiseImport(layerIdId.error().message);
        }
        auto qualifierId = tile.strings()->emplace(qualifier);
        if (!qualifierId) {
            raiseImport(qualifierId.error().message);
        }

        refs.push_back(QualifiedSourceDataReference{
            SourceDataAddress{address},
            *layerIdId,
            *qualifierId,
        });
    }

    return tile.newSourceDataReferenceCollection(refs);
}

struct ParsedFeatureReferenceJson
{
    ParsedFeatureId featureId_;
    std::optional<std::string> externalMapId_;
};

/** Parse a local or external feature reference from JSON. */
[[nodiscard]] ParsedFeatureReferenceJson parseFeatureReferenceJson(
    TileFeatureLayer& tile,
    nlohmann::json const& json)
{
    std::string canonicalId;
    std::optional<std::string> externalMapId;

    if (json.is_string()) {
        canonicalId = json.get<std::string>();
    }
    else if (json.is_object()) {
        auto const idIt = json.find("id");
        if (idIt == json.end() || !idIt->is_string()) {
            raiseImport("Feature reference objects must contain a string `id` field.");
        }
        canonicalId = idIt->get<std::string>();

        if (auto mapIdIt = json.find("mapId"); mapIdIt != json.end()) {
            if (!mapIdIt->is_string()) {
                raiseImport("Feature reference `mapId` must be a string.");
            }
            externalMapId = mapIdIt->get<std::string>();
        }
    }
    else {
        raiseImport("Feature references must be encoded as canonical feature-id strings or `{id, mapId}` objects.");
    }

    ParsedFeatureReferenceJson parsed;
    std::string error;
    if (!parseFeatureIdString(canonicalId, *tile.layerInfo(), parsed.featureId_, &error)) {
        raiseImport(error);
    }
    parsed.externalMapId_ = std::move(externalMapId);
    return parsed;
}

/** Parse a canonical feature-id string into a detached FeatureId node. */
[[nodiscard]] model_ptr<FeatureId> importFeatureReferenceId(
    TileFeatureLayer& tile,
    nlohmann::json const& json)
{
    auto parsed = parseFeatureReferenceJson(tile, json);
    auto partsView = castToKeyValueView(parsed.featureId_.keyValuePairs_);
    auto externalMapId = parsed.externalMapId_
        ? std::optional<std::string_view>(*parsed.externalMapId_)
        : std::nullopt;
    return tile.newFeatureId(parsed.featureId_.typeId_, partsView, externalMapId);
}

[[nodiscard]] model_ptr<Geometry> importStandaloneGeometry(
    TileFeatureLayer& tile,
    nlohmann::json const& geometryJson,
    GeoJsonImportOptions const& options);

/** Apply metadata that is shared by all geometry encodings. */
void applyGeometryDecorations(
    TileFeatureLayer& tile,
    model_ptr<Geometry> geometry,
    nlohmann::json const& geometryJson,
    GeoJsonImportOptions const& options)
{
    if (!geometry) {
        return;
    }

    if (auto name = geometryNameFromJson(geometryJson)) {
        geometry->setName(*name);
    }
    if (auto sourceDataJson = findSourceDataJson(geometryJson)) {
        if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
            geometry->setSourceDataReferences(*refs);
        }
    }
}

/** Import a GeoJSON Polygon, preserving explicit hole-ring boundaries. */
[[nodiscard]] model_ptr<Geometry> importPolygonGeometry(
    TileFeatureLayer& tile,
    nlohmann::json const& coords)
{
    if (!coords.is_array() || coords.empty()) {
        raiseImport("Polygon coordinates must contain at least one linear ring.");
    }

    size_t pointCount = 0;
    std::vector<uint32_t> ringStarts;
    ringStarts.reserve(coords.size());
    for (auto const& ring : coords) {
        if (!ring.is_array() || ring.empty()) {
            raiseImport("Polygon rings must be non-empty coordinate arrays.");
        }
        if (ring.size() > std::numeric_limits<uint32_t>::max() - pointCount) {
            raiseImport("Polygon has too many vertices.");
        }
        ringStarts.push_back(static_cast<uint32_t>(pointCount));
        pointCount += ring.size();
    }

    auto geometry = tile.newGeometry(GeomType::Polygon, pointCount, true);
    for (auto const& ring : coords) {
        for (auto const& coord : ring) {
            geometry->append(pointFromCoordinateJson(coord));
        }
    }
    if (ringStarts.size() > 1) {
        geometry->setPolygonRingStarts(ringStarts);
    }
    return geometry;
}

/** Import one standalone geometry object outside of feature-collection splitting logic. */
[[nodiscard]] model_ptr<Geometry> importStandaloneGeometry(
    TileFeatureLayer& tile,
    nlohmann::json const& geometryJson,
    GeoJsonImportOptions const& options)
{
    if (!geometryJson.is_object()) {
        raiseImport("Geometry must be a JSON object.");
    }
    if (!geometryJson.contains("type") || !geometryJson.at("type").is_string()) {
        raiseImport("Geometry object is missing string field 'type'.");
    }

    auto const type = geometryJson.at("type").get<std::string>();

    if (type == "Point") {
        auto geometry = tile.newGeometry(GeomType::Points, 1, true);
        geometry->append(pointFromCoordinateJson(geometryJson.at("coordinates")));
        applyGeometryDecorations(tile, geometry, geometryJson, options);
        return geometry;
    }

    if (type == "MultiPoint") {
        auto const& coords = geometryJson.at("coordinates");
        auto geometry = tile.newGeometry(GeomType::Points, coords.size(), true);
        for (auto const& coord : coords) {
            geometry->append(pointFromCoordinateJson(coord));
        }
        applyGeometryDecorations(tile, geometry, geometryJson, options);
        return geometry;
    }

    if (type == "LineString") {
        auto const& coords = geometryJson.at("coordinates");
        auto geometry = tile.newGeometry(GeomType::Line, coords.size(), true);
        for (auto const& coord : coords) {
            geometry->append(pointFromCoordinateJson(coord));
        }
        applyGeometryDecorations(tile, geometry, geometryJson, options);
        return geometry;
    }

    if (type == "MultiLineString") {
        raiseImport("Standalone validity geometries do not support MultiLineString values.");
    }

    if (type == "Polygon") {
        if (geometryJson.contains("gltfNodeIndex")) {
            raiseImport("GLTF-backed geometry import is not supported yet.");
        }
        if (geometryJson.contains("aabb")) {
            auto const& aabb = geometryJson.at("aabb");
            auto geometry = tile.newGeometry(GeomType::AABB, 2, true);
            geometry->setAabb(
                pointFromCoordinateJson(aabb.at("origin")),
                pointFromCoordinateJson(aabb.at("size")));
            applyGeometryDecorations(tile, geometry, geometryJson, options);
            return geometry;
        }

        auto geometry = importPolygonGeometry(tile, geometryJson.at("coordinates"));
        applyGeometryDecorations(tile, geometry, geometryJson, options);
        return geometry;
    }

    if (type == "MultiPolygon") {
        auto const& polygons = geometryJson.at("coordinates");
        if (!options.strict_) {
            raiseImport("Standalone generic MultiPolygon import requires a feature context.");
        }
        auto geometry = tile.newGeometry(GeomType::Mesh, polygons.size() * 3, true);
        for (auto const& polygon : polygons) {
            if (!polygon.is_array() || polygon.size() != 1 || !polygon.at(0).is_array()) {
                raiseImport("Strict mapget mesh import expects every MultiPolygon item to contain exactly one ring.");
            }
            auto const& ring = polygon.at(0);
            std::vector<Point> triangle;
            triangle.reserve(ring.size());
            for (auto const& coord : ring) {
                triangle.push_back(pointFromCoordinateJson(coord));
            }
            if (!triangle.empty() && triangle.front() == triangle.back()) {
                // GeoJSON triangle rings are closed, while mapget mesh triangles are stored open.
                triangle.pop_back();
            }
            if (triangle.size() != 3) {
                raiseImport("Strict mapget mesh import expects triangular MultiPolygon rings.");
            }
            for (auto const& point : triangle) {
                geometry->append(point);
            }
        }
        applyGeometryDecorations(tile, geometry, geometryJson, options);
        return geometry;
    }

    if (type == "GeometryCollection") {
        raiseImport("Standalone validity geometries do not support GeometryCollection values.");
    }

    raiseImport(fmt::format("Unsupported geometry type '{}'.", type));
    return {};
}

/** Import feature geometry, expanding GeoJSON aggregate types into mapget's geometry list. */
void importFeatureGeometry(
    TileFeatureLayer& tile,
    model_ptr<Feature> feature,
    nlohmann::json const& geometryJson,
    GeoJsonImportOptions const& options)
{
    if (geometryJson.is_null()) {
        return;
    }
    if (!geometryJson.is_object()) {
        raiseImport("Feature geometry must be an object or null.");
    }

    if (!geometryJson.contains("type") || !geometryJson.at("type").is_string()) {
        raiseImport("Feature geometry must contain a string field 'type'.");
    }
    auto const type = geometryJson.at("type").get<std::string>();
    if (type == "GeometryCollection") {
        if (!geometryJson.contains("geometries") || !geometryJson.at("geometries").is_array()) {
            raiseImport("GeometryCollection is missing array-valued field 'geometries'.");
        }
        for (auto const& child : geometryJson.at("geometries")) {
            auto geometry = importStandaloneGeometry(tile, child, GeoJsonImportOptions{options.strict_, options.fallbackFeatureType_, options.objectPropertiesAsAttributeLayers_});
            feature->addGeometry(geometry);
        }
        return;
    }

    if (type == "MultiLineString") {
        auto const& lines = geometryJson.at("coordinates");
        for (auto const& line : lines) {
            // mapget represents multi-lines as multiple line geometries on one feature.
            auto lineJson = nlohmann::json::object({
                {"type", "LineString"},
                {"coordinates", line},
            });
            if (geometryJson.contains("geometryName")) {
                lineJson["geometryName"] = geometryJson.at("geometryName");
            }
            if (auto sourceDataJson = findSourceDataJson(geometryJson)) {
                lineJson["_sourceData"] = *sourceDataJson;
            }
            auto geometry = importStandaloneGeometry(tile, lineJson, options);
            feature->addGeometry(geometry);
        }
        return;
    }

    if (type == "MultiPolygon" && !options.strict_) {
        auto const& polygons = geometryJson.at("coordinates");
        for (auto const& polygon : polygons) {
            if (!polygon.is_array() || polygon.empty() || !polygon.at(0).is_array()) {
                raiseImport("Generic MultiPolygon import expects every polygon to contain at least one ring.");
            }
            auto polyJson = nlohmann::json::object({
                {"type", "Polygon"},
                {"coordinates", polygon},
            });
            if (geometryJson.contains("geometryName")) {
                polyJson["geometryName"] = geometryJson.at("geometryName");
            }
            if (auto sourceDataJson = findSourceDataJson(geometryJson)) {
                polyJson["_sourceData"] = *sourceDataJson;
            }
            auto geometry = importStandaloneGeometry(tile, polyJson, options);
            feature->addGeometry(geometry);
        }
        return;
    }

    auto geometry = importStandaloneGeometry(tile, geometryJson, options);
    feature->addGeometry(geometry);
}

/** Import one validity or validity list into mapget's MultiValidity container. */
void importValidityCollection(
    TileFeatureLayer& tile,
    model_ptr<Feature> hostFeature,
    model_ptr<MultiValidity> collection,
    nlohmann::json const& json,
    GeoJsonImportOptions const& options,
    AttrPointSequenceRegistry const& attrPointSequences)
{
    auto values = json.is_array() ? json : nlohmann::json::array({json});
    for (auto const& value : values) {
        if (!value.is_object()) {
            raiseImport("Validity entries must be JSON objects.");
        }

        auto validity = tile.newValidity();
        if (value.contains("direction")) {
            auto direction = directionFromJson(value.at("direction"));
            if (!direction) {
                raiseImport(fmt::format("Unknown validity direction '{}'.", value.at("direction").dump()));
            }
            validity->setDirection(*direction);
        }
        if (auto name = geometryNameFromJson(value)) {
            validity->setGeometryName(*name);
        }

        if (value.contains("from") || value.contains("to")) {
            // Transition endpoints are semantic IDs and may point outside the current tile.
            if (!value.contains("from") || !value.contains("to") ||
                !value.contains("fromConnectedEnd") || !value.contains("toConnectedEnd") ||
                !value.contains("transitionNumber")) {
                raiseImport("Feature transition validities must define from/to ids, connected ends, and transitionNumber.");
            }
            auto fromFeatureId = importFeatureReferenceId(tile, value.at("from"));
            auto toFeatureId = importFeatureReferenceId(tile, value.at("to"));
            auto fromEnd = transitionEndFromJson(value.at("fromConnectedEnd"));
            auto toEnd = transitionEndFromJson(value.at("toConnectedEnd"));
            if (!fromEnd || !toEnd) {
                raiseImport("Invalid transition end in feature transition validity.");
            }
            validity->setFeatureTransition(
                fromFeatureId,
                *fromEnd,
                toFeatureId,
                *toEnd,
                value.at("transitionNumber").get<uint32_t>());
            collection->append(validity);
            continue;
        }

        if (value.contains("geometry")) {
            validity->setSimpleGeometry(importStandaloneGeometry(tile, value.at("geometry"), options));
            collection->append(validity);
            continue;
        }


        if (value.contains("attrPointIndex")) {
            auto const& indexJson = value.at("attrPointIndex");
            if (!indexJson.is_object() || !indexJson.contains("sequence") ||
                !indexJson.contains("index"))
            {
                raiseImport("attrPointIndex must define sequence and index fields.");
            }
            validity->setAttrPointIndex(
                importAttrPointSequenceReference(
                    indexJson.at("sequence"),
                    attrPointSequences),
                indexJson.at("index").get<uint32_t>());
            collection->append(validity);
            continue;
        }

        if (value.contains("attrPointIndexRange")) {
            auto const& rangeJson = value.at("attrPointIndexRange");
            if (!rangeJson.is_object() || !rangeJson.contains("sequence") ||
                !rangeJson.contains("start") || !rangeJson.contains("end"))
            {
                raiseImport("attrPointIndexRange must define sequence, start, and end fields.");
            }
            validity->setAttrPointIndexRange(
                importAttrPointSequenceReference(
                    rangeJson.at("sequence"),
                    attrPointSequences),
                rangeJson.at("start").get<uint32_t>(),
                rangeJson.at("end").get<uint32_t>());
            collection->append(validity);
            continue;
        }

        if (value.contains("featureId")) {
            validity->setFeatureId(importFeatureReferenceId(tile, value.at("featureId")));
        }

        auto offsetType = value.contains("offsetType") ? offsetTypeFromJson(value.at("offsetType")) : std::optional<Validity::GeometryOffsetType>{};
        if (value.contains("offsetType") && !offsetType) {
            raiseImport(fmt::format("Unknown validity offsetType '{}'.", value.at("offsetType").dump()));
        }

        if (value.contains("point")) {
            if (offsetType && *offsetType != Validity::GeoPosOffset) {
                // Non-geospatial offsets reuse the `point` field name for historic JSON compatibility.
                validity->setOffsetPoint(*offsetType, value.at("point").get<double>());
            }
            else {
                validity->setOffsetPoint(pointFromCoordinateJson(value.at("point")));
            }
            collection->append(validity);
            continue;
        }

        if (value.contains("start") || value.contains("end")) {
            if (!value.contains("start") || !value.contains("end")) {
                raiseImport("Validity ranges must define both start and end.");
            }
            if (offsetType && *offsetType != Validity::GeoPosOffset) {
                validity->setOffsetRange(
                    *offsetType,
                    value.at("start").get<double>(),
                    value.at("end").get<double>());
            }
            else {
                validity->setOffsetRange(
                    pointFromCoordinateJson(value.at("start")),
                    pointFromCoordinateJson(value.at("end")));
            }
            collection->append(validity);
            continue;
        }

        collection->append(validity);
    }
}

struct DeferredAttributeValidity
{
    model_ptr<Feature> hostFeature_;
    model_ptr<Attribute> attribute_;
    nlohmann::json validityJson_;
};

/** Defer relation import until every feature id in the tile has been created. */
struct DeferredRelation
{
    model_ptr<Feature> feature_;
    nlohmann::json relationJson_;
};

/** Defer property import until top-level relations are available for `$mapgetRelation` references. */
struct DeferredProperties
{
    model_ptr<Feature> feature_;
    nlohmann::json propertiesJson_;
};

/** Import one attribute payload object, excluding deferred validity handling. */
void importAttributeObject(
    TileFeatureLayer& tile,
    model_ptr<Feature> hostFeature,
    model_ptr<Attribute> attribute,
    nlohmann::json const& json,
    GeoJsonImportOptions const& options,
    std::vector<DeferredAttributeValidity>& deferredValidities)
{
    if (!json.is_object()) {
        raiseImport(fmt::format("Attribute '{}' must be encoded as an object.", attribute->name()));
    }

    for (auto const& [key, value] : json.items()) {
        if (key == "_multimap") {
            continue;
        }
        if (key == "validity") {
            deferredValidities.push_back(DeferredAttributeValidity{hostFeature, attribute, value});
            continue;
        }
        if (key == "_sourceData" || key == "sourceData") {
            if (auto refs = importSourceDataReferences(tile, value)) {
                attribute->setSourceDataReferences(*refs);
            }
            continue;
        }
        auto result = attribute->addField(key, importGenericNode(tile, value, hostFeature));
        if (!result) {
            raiseImport(result.error().message);
        }
    }
}

/** Import one attribute-layer object, preserving duplicate attribute names and optional instance id metadata. */
void importAttributeLayerObject(
    TileFeatureLayer& tile,
    model_ptr<Feature> feature,
    std::string const& layerName,
    nlohmann::json const& layerJson,
    GeoJsonImportOptions const& options,
    std::vector<DeferredAttributeValidity>& deferredValidities)
{
    if (!layerJson.is_object()) {
        raiseImport(fmt::format("Attribute layer '{}' must be a JSON object.", layerName));
    }

    auto attrLayer = feature->attributeLayers()->newLayer(layerName);
    for (auto const& [attrName, attrValue] : layerJson.items()) {
        if (attrName == "_multimap") {
            continue;
        }
        if (attrName == AttributeLayer::InstanceIdField) {
            if (!attrValue.is_number_unsigned() && !attrValue.is_number_integer()) {
                raiseImport(fmt::format("Attribute layer '{}' id must be an integer.", layerName));
            }
            auto const layerId = attrValue.get<int64_t>();
            if (layerId < 0) {
                raiseImport(fmt::format("Attribute layer '{}' id must not be negative.", layerName));
            }
            attrLayer->setId(static_cast<uint64_t>(layerId));
            continue;
        }

        auto importOneAttribute = [&](nlohmann::json const& attributeJson) {
            auto attribute = attrLayer->newAttribute(attrName);
            importAttributeObject(tile, feature, attribute, attributeJson, options, deferredValidities);
        };

        if (attrValue.is_array()) {
            for (auto const& element : attrValue) {
                importOneAttribute(element);
            }
        }
        else {
            importOneAttribute(attrValue);
        }
    }
}

/** Import feature properties and map attribute-layer payloads into their model containers. */
void importProperties(
    TileFeatureLayer& tile,
    model_ptr<Feature> feature,
    nlohmann::json const& propertiesJson,
    GeoJsonImportOptions const& options,
    std::vector<DeferredAttributeValidity>& deferredValidities)
{
    if (!propertiesJson.is_object()) {
        raiseImport("Feature properties must be encoded as an object.");
    }

    if (auto layerIt = propertiesJson.find("layer"); layerIt != propertiesJson.end()) {
        if (!layerIt->is_object()) {
            raiseImport("properties.layer must be a JSON object.");
        }
        for (auto const& [layerName, layerValue] : layerIt->items()) {
            if (layerName == "_multimap") {
                continue;
            }
            if (layerValue.is_array()) {
                for (auto const& layerElement : layerValue) {
                    importAttributeLayerObject(tile, feature, layerName, layerElement, options, deferredValidities);
                }
            }
            else {
                importAttributeLayerObject(tile, feature, layerName, layerValue, options, deferredValidities);
            }
        }
    }

    for (auto const& [key, value] : propertiesJson.items()) {
        if (key == "layer") {
            // `properties.layer` is reserved for exported attribute layers.
            continue;
        }
        if (!options.strict_ && options.objectPropertiesAsAttributeLayers_ && value.is_object()) {
            // Best-effort mode may reinterpret nested objects as attribute layers for plain GeoJSON.
            auto attrLayer = feature->attributeLayers()->newLayer(key);
            for (auto const& [attrName, attrValue] : value.items()) {
                auto attribute = attrLayer->newAttribute(attrName);
                if (attrValue.is_object()) {
                    importAttributeObject(tile, feature, attribute, attrValue, options, deferredValidities);
                }
                else {
                    auto result = attribute->addField(attrName, importGenericNode(tile, attrValue, feature));
                    if (!result) {
                        raiseImport(result.error().message);
                    }
                }
            }
            continue;
        }

        auto result = feature->attributes()->addField(key, importGenericNode(tile, value, feature));
        if (!result) {
            raiseImport(result.error().message);
        }
    }
}

/** Import one relation object after feature ids are already resolvable. */
void importRelation(
    TileFeatureLayer& tile,
    model_ptr<Feature> feature,
    nlohmann::json const& relationJson,
    GeoJsonImportOptions const& options,
    AttrPointSequenceRegistry const& attrPointSequences)
{
    if (!relationJson.is_object()) {
        raiseImport("Every relation must be encoded as an object.");
    }
    if (!relationJson.contains("name") || !relationJson.contains("target")) {
        raiseImport("Every relation must contain name and target fields.");
    }

    auto relation = tile.newRelation(
        relationJson.at("name").get<std::string>(),
        importFeatureReferenceId(tile, relationJson.at("target")));

    if (auto sourceDataJson = findSourceDataJson(relationJson)) {
        if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
            relation->setSourceDataReferences(*refs);
        }
    }
    if (relationJson.contains("sourceValidity")) {
        importValidityCollection(
            tile,
            feature,
            relation->sourceValidity(),
            relationJson.at("sourceValidity"),
            options,
            attrPointSequences);
    }
    if (relationJson.contains("targetValidity")) {
        importValidityCollection(
            tile,
            feature,
            relation->targetValidity(),
            relationJson.at("targetValidity"),
            options,
            attrPointSequences);
    }

    feature->addRelation(relation);
}

/** Resolve one geometry by stable feature-local ordinal. */
[[nodiscard]] model_ptr<Geometry> featureGeometryAt(
    model_ptr<Feature> const& feature,
    uint32_t geometryIndex)
{
    auto geometries = feature->geomOrNull();
    if (!geometries) {
        raiseImport(fmt::format(
            "AttrPointSequence host feature '{}' has no geometry.",
            feature->id()->toString()));
    }

    uint32_t currentIndex = 0;
    model_ptr<Geometry> result;
    geometries->forEachGeometry([&](model_ptr<Geometry> const& geometry) {
        if (currentIndex == geometryIndex) {
            result = geometry;
            return false;
        }
        ++currentIndex;
        return true;
    });
    if (!result) {
        raiseImport(fmt::format(
            "AttrPointSequence geometryIndex {} is out of range for feature '{}'.",
            geometryIndex,
            feature->id()->toString()));
    }
    return result;
}

/** Import shared AttrPointSequence definitions after every host feature exists. */
[[nodiscard]] AttrPointSequenceRegistry importAttrPointSequences(
    TileFeatureLayer& tile,
    nlohmann::json const& geoJson)
{
    AttrPointSequenceRegistry result;
    auto const definitions = geoJson.find("attrPointSequences");
    if (definitions == geoJson.end()) {
        return result;
    }
    if (!definitions->is_array()) {
        raiseImport("attrPointSequences must be an array.");
    }

    result.reserve(definitions->size());
    for (auto const& sequenceJson : *definitions) {
        if (!sequenceJson.is_object() || !sequenceJson.contains("id") ||
            !sequenceJson.contains("featureId") ||
            !sequenceJson.contains("geometryIndex") ||
            !sequenceJson.contains("attrPoints"))
        {
            raiseImport(
                "Every AttrPointSequence must define id, featureId, geometryIndex, and attrPoints.");
        }
        auto const expectedId = static_cast<uint32_t>(result.size());
        if (sequenceJson.at("id").get<uint32_t>() != expectedId) {
            raiseImport(fmt::format(
                "AttrPointSequence id {} is not the expected contiguous id {}.",
                sequenceJson.at("id").dump(),
                expectedId));
        }
        if (!sequenceJson.at("featureId").is_string()) {
            raiseImport("AttrPointSequence featureId must identify a local feature by string ID.");
        }
        auto feature = tile.find(sequenceJson.at("featureId").get<std::string>());
        if (!feature) {
            raiseImport(fmt::format(
                "AttrPointSequence references missing feature '{}'.",
                sequenceJson.at("featureId").get<std::string>()));
        }
        auto geometry = featureGeometryAt(
            feature,
            sequenceJson.at("geometryIndex").get<uint32_t>());
        if (auto geometryName = sequenceJson.find("geometryName");
            geometryName != sequenceJson.end())
        {
            if (!geometryName->is_string() || geometry->name() != geometryName->get<std::string>()) {
                raiseImport("AttrPointSequence geometryName does not match its referenced geometry.");
            }
        }

        auto sequence = tile.newAttrPointSequence(feature, geometry);
        if (!sequenceJson.at("attrPoints").is_array()) {
            raiseImport("AttrPointSequence attrPoints must be an array.");
        }
        for (auto const& pointJson : sequenceJson.at("attrPoints")) {
            if (!pointJson.is_object() || !pointJson.contains("index") ||
                !pointJson.contains("point"))
            {
                raiseImport("Every AttrPoint must define index and point fields.");
            }
            model_ptr<SourceDataReferenceCollection> pointSourceData;
            if (auto sourceDataJson = findSourceDataJson(pointJson)) {
                if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
                    pointSourceData = *refs;
                }
            }
            sequence->appendAttrPoint(
                pointJson.at("index").get<uint32_t>(),
                pointFromCoordinateJson(pointJson.at("point")),
                pointSourceData);
        }
        if (auto sourceDataJson = findSourceDataJson(sequenceJson)) {
            if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
                sequence->setSourceDataReferences(*refs);
            }
        }
        result.push_back(sequence);
    }
    return result;
}

/** Determine the target feature type for one imported feature. */
[[nodiscard]] std::string determineFeatureType(
    TileFeatureLayer const& tile,
    nlohmann::json const& featureJson,
    GeoJsonImportOptions const& options)
{
    if (auto typeIt = featureJson.find("typeId"); typeIt != featureJson.end() && typeIt->is_string()) {
        return typeIt->get<std::string>();
    }
    if (options.fallbackFeatureType_) {
        return *options.fallbackFeatureType_;
    }
    if (tile.layerInfo()->featureTypes_.size() == 1) {
        return tile.layerInfo()->featureTypes_.front().name_;
    }
    raiseImport("Could not determine feature type. Missing 'typeId' and no fallback feature type was configured.");
    return {};
}

}

/** Import a FeatureCollection into an empty TileFeatureLayer. */
void importGeoJson(
    TileFeatureLayer& tile,
    nlohmann::json const& geoJson,
    GeoJsonImportOptions const& options)
{
    if (tile.numRoots() != 0) {
        raiseImport("Import requires an empty TileFeatureLayer instance.");
    }
    if (tile.getIdPrefix()) {
        // GeoJSON import now treats full ids as the canonical representation.
        raiseImport("Import requires a TileFeatureLayer without a preconfigured idPrefix.");
    }
    if (!geoJson.is_object() || geoJson.value("type", "") != "FeatureCollection") {
        raiseImport("GeoJSON root must be a FeatureCollection.");
    }
    if (!geoJson.contains("features") || !geoJson.at("features").is_array()) {
        raiseImport("GeoJSON FeatureCollection is missing an array-valued 'features' field.");
    }

    if (options.strict_) {
        if (geoJson.contains("glbAttachment")) {
            raiseImport(
                "Embedded GLB attachment import is not supported; "
                "use glbAttachmentName and the attachment API.");
        }
        // Strict mode treats top-level metadata mismatches as caller/configuration errors.
        if (geoJson.contains("mapgetTileId") && geoJson.at("mapgetTileId").get<int32_t>() != tile.tileId().value()) {
            raiseImport("mapgetTileId does not match the target tile.");
        }
        if (geoJson.contains("mapId") && geoJson.at("mapId").get<std::string>() != tile.mapId()) {
            raiseImport("mapId does not match the target tile.");
        }
        if (geoJson.contains("mapgetLayerId") && geoJson.at("mapgetLayerId").get<std::string>() != tile.layerInfo()->layerId_) {
            raiseImport("mapgetLayerId does not match the target tile layer.");
        }
    }

    if (geoJson.contains("geometryAnchor")) {
        tile.setGeometryAnchor(pointFromCoordinateJson(geoJson.at("geometryAnchor")));
    }
    if (auto attachmentName =
            geoJson.find("glbAttachmentName");
        attachmentName != geoJson.end())
    {
        if (!attachmentName->is_string()) {
            raiseImport(
                "glbAttachmentName must be a string.");
        }
        tile.setGlbAttachmentName(
            attachmentName->get<std::string>());
    }
    if (geoJson.contains("timestamp")) {
        auto const& timestampJson = geoJson.at("timestamp");
        if (!timestampJson.is_number_integer()) {
            raiseImport("timestamp must be encoded as integer microseconds since the Unix epoch.");
        }
        tile.setTimestamp(std::chrono::time_point<std::chrono::system_clock>(
            std::chrono::microseconds(timestampJson.get<int64_t>())));
    }
    if (geoJson.contains("ttl")) {
        tile.setTtl(std::chrono::milliseconds(geoJson.at("ttl").get<int64_t>()));
    }
    if (geoJson.contains("error")) {
        auto const& errorJson = geoJson.at("error");
        if (!errorJson.is_object()) {
            raiseImport("error must be encoded as an object.");
        }
        if (errorJson.contains("message")) {
            tile.setError(errorJson.at("message").get<std::string>());
        }
        if (errorJson.contains("code")) {
            tile.setErrorCode(errorJson.at("code").get<int>());
        }
    }

    std::vector<DeferredAttributeValidity> deferredValidities;
    std::vector<DeferredRelation> deferredRelations;
    std::vector<DeferredProperties> deferredProperties;

    uint32_t fallbackFeatureIndex = 0;
    for (auto const& featureJson : geoJson.at("features")) {
        if (!featureJson.is_object()) {
            raiseImport("Every feature entry must be a JSON object.");
        }
        if (featureJson.value("type", "") != "Feature") {
            raiseImport("Every feature entry must have type 'Feature'.");
        }

        auto const typeId = determineFeatureType(tile, featureJson, options);
        KeyValuePairs fullIdParts;

        if (options.strict_ || featureJson.contains("typeId")) {
            auto const* typeInfo = tile.layerInfo()->getTypeInfo(typeId, false);
            if (!typeInfo || typeInfo->uniqueIdCompositions_.empty()) {
                raiseImport(fmt::format("Could not resolve feature type '{}' for import.", typeId));
            }
            fullIdParts = fullFeatureIdPartsFromFields(featureJson, typeInfo->uniqueIdCompositions_.front());
            if (featureJson.contains("id")) {
                ParsedFeatureId parsed;
                std::string error;
                if (!parseFeatureIdString(featureJson.at("id").get<std::string>(), *tile.layerInfo(), parsed, &error)) {
                    raiseImport(error);
                }
                if (parsed.typeId_ != typeId || parsed.keyValuePairs_ != fullIdParts) {
                    raiseImport(fmt::format("Feature id '{}' does not match explicit id-part fields.", featureJson.at("id").get<std::string>()));
                }
            }
            // Strict import reconstructs the full id verbatim instead of reintroducing tile prefixes.
        }
        else {
            fullIdParts = bestEffortFullFeatureIdParts(tile, typeId, fallbackFeatureIndex);
        }
        ++fallbackFeatureIndex;

        // Keep the owned strings alive while the temporary view is handed to newFeature().
        auto feature = tile.newFeature(typeId, castToKeyValueView(fullIdParts));

        if (auto sourceDataJson = findSourceDataJson(featureJson)) {
            if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
                feature->setSourceDataReferences(*refs);
            }
        }
        if (featureJson.contains("geometry")) {
            importFeatureGeometry(tile, feature, featureJson.at("geometry"), options);
        }
        auto propertiesIt = featureJson.find("properties");
        auto attributesIt = featureJson.find("attributes");
        if (propertiesIt != featureJson.end() && attributesIt != featureJson.end()) {
            raiseImport("Feature must not contain both 'properties' and its 'attributes' alias.");
        }
        if (propertiesIt != featureJson.end() || attributesIt != featureJson.end()) {
            // `attributes` is accepted as an import-only alias; JSON export stays
            // GeoJSON-compatible and emits the canonical `properties` key.
            deferredProperties.push_back(DeferredProperties{
                feature,
                propertiesIt != featureJson.end() ? *propertiesIt : *attributesIt});
        }
        if (featureJson.contains("relations")) {
            deferredRelations.push_back(DeferredRelation{feature, featureJson.at("relations")});
        }
    }

    // Shared interwoven domains must exist before relation or attribute
    // validities resolve their compact sequence references.
    auto attrPointSequences = importAttrPointSequences(tile, geoJson);

    // Relations are imported last for the same reason: all target ids must already be present.
    for (auto& deferred : deferredRelations) {
        if (!deferred.relationJson_.is_array()) {
            raiseImport("Feature relations must be encoded as an array.");
        }
        for (auto const& relationJson : deferred.relationJson_) {
            importRelation(
                tile,
                deferred.feature_,
                relationJson,
                options,
                attrPointSequences);
        }
    }

    // Properties are imported after relations so nested `$mapgetRelation`
    // tokens can resolve to the owning feature's canonical relation objects.
    for (auto& deferred : deferredProperties) {
        importProperties(tile, deferred.feature_, deferred.propertiesJson_, options, deferredValidities);
    }

    // Attribute validities may reference local features and property import creates the attributes.
    for (auto& deferred : deferredValidities) {
        importValidityCollection(
            tile,
            deferred.hostFeature_,
            deferred.attribute_->validity(),
            deferred.validityJson_,
            options,
            attrPointSequences);
    }
}

void TileFeatureLayer::fromJson(nlohmann::json const& json, GeoJsonImportOptions const& options)
{
    importGeoJson(*this, json, options);
}

}
