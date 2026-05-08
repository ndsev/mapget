#include "mapget/model/geojson-import.h"

#include "mapget/model/attr.h"
#include "mapget/model/attrlayer.h"
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

/** Delegate generic object/array/scalar import to simfil's shared JSON builder. */
[[nodiscard]] simfil::ModelNode::Ptr importGenericNode(
    TileFeatureLayer& tile,
    nlohmann::json const& json)
{
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

/** Resolve a geometry stage label back to its layer-specific numeric stage. */
[[nodiscard]] std::optional<uint32_t> stageFromGeometryName(
    TileFeatureLayer const& tile,
    nlohmann::json const& json,
    bool strict)
{
    auto const* layerInfo = tile.layerInfo().get();
    if (!layerInfo) {
        return std::nullopt;
    }
    if (!json.contains("geometryName")) {
        return layerInfo->highFidelityStage_;
    }
    if (!json.at("geometryName").is_string()) {
        raiseImport("geometryName must be a string.");
    }
    auto const label = json.at("geometryName").get<std::string>();
    auto const& labels = layerInfo->stageLabels_;
    std::optional<uint32_t> matchedStage;
    for (uint32_t i = 0; i < labels.size(); ++i) {
        if (labels[i] != label) {
            continue;
        }
        if (matchedStage) {
            if (strict) {
                raiseImport(fmt::format(
                    "Ambiguous geometryName '{}' for layer '{}'.",
                    label,
                    layerInfo->layerId_));
            }
            // In best-effort mode ambiguous labels are ignored rather than guessed.
            return std::nullopt;
        }
        matchedStage = i;
    }
    if (!matchedStage) {
        if (strict) {
            raiseImport(fmt::format(
                "Unknown geometryName '{}' for layer '{}'.",
                label,
                layerInfo->layerId_));
        }
        return std::nullopt;
    }
    return matchedStage;
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

    if (tile.tileId().value_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        raiseImport("tileId exceeds signed integer range needed for best-effort id synthesis.");
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
            parts.emplace_back(idPart.idPartLabel_, static_cast<int64_t>(tile.tileId().value_));
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

/** Resolve a canonical feature-id string to a feature already created in this tile. */
[[nodiscard]] model_ptr<Feature> resolveLocalFeatureReference(
    TileFeatureLayer& tile,
    nlohmann::json const& json)
{
    auto parsed = parseFeatureReferenceJson(tile, json);
    if (parsed.externalMapId_ && *parsed.externalMapId_ != tile.mapId()) {
        raiseImport(fmt::format(
            "Feature transition references must stay local to map '{}'.",
            tile.mapId()));
    }

    if (auto feature = tile.find(parsed.featureId_.typeId_, parsed.featureId_.keyValuePairs_)) {
        return feature;
    }
    auto referenceJson = json.is_string() ? json.get<std::string>() : json.dump();
    raiseImport(fmt::format("Could not resolve local feature reference '{}'.", referenceJson));
    return {};
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

    if (auto stage = stageFromGeometryName(tile, geometryJson, options.strict_)) {
        geometry->setStage(*stage);
    }
    if (auto sourceDataJson = findSourceDataJson(geometryJson)) {
        if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
            geometry->setSourceDataReferences(*refs);
        }
    }
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

        auto const& coords = geometryJson.at("coordinates");
        if (!coords.is_array() || coords.empty()) {
            raiseImport("Polygon coordinates must contain at least one linear ring.");
        }
        if (coords.size() > 1 && options.strict_) {
            // Native mapget polygons do not preserve hole rings, so strict mode rejects them.
            raiseImport("Polygon holes are not supported by mapget polygon geometries.");
        }
        auto const& outerRing = coords.at(0);
        auto geometry = tile.newGeometry(GeomType::Polygon, outerRing.size(), true);
        for (auto const& coord : outerRing) {
            geometry->append(pointFromCoordinateJson(coord));
        }
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
            // Best-effort mode degrades each polygon to its outer ring because mapget polygons have no holes.
            auto polyJson = nlohmann::json::object({
                {"type", "Polygon"},
                {"coordinates", nlohmann::json::array({polygon.at(0)})},
            });
            if (auto stage = stageFromGeometryName(tile, geometryJson, options.strict_)) {
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
    GeoJsonImportOptions const& options)
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
        if (auto stage = stageFromGeometryName(tile, value, options.strict_)) {
            validity->setGeometryStage(*stage);
        }

        if (value.contains("from") || value.contains("to")) {
            // Transition validities are the only variant that references two local features.
            if (!value.contains("from") || !value.contains("to") ||
                !value.contains("fromConnectedEnd") || !value.contains("toConnectedEnd") ||
                !value.contains("transitionNumber")) {
                raiseImport("Feature transition validities must define from/to ids, connected ends, and transitionNumber.");
            }
            auto fromFeature = resolveLocalFeatureReference(tile, value.at("from"));
            auto toFeature = resolveLocalFeatureReference(tile, value.at("to"));
            auto fromEnd = transitionEndFromJson(value.at("fromConnectedEnd"));
            auto toEnd = transitionEndFromJson(value.at("toConnectedEnd"));
            if (!fromEnd || !toEnd) {
                raiseImport("Invalid transition end in feature transition validity.");
            }
            validity->setFeatureTransition(
                fromFeature,
                *fromEnd,
                toFeature,
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
        auto result = attribute->addField(key, importGenericNode(tile, value));
        if (!result) {
            raiseImport(result.error().message);
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
            if (!layerValue.is_object()) {
                raiseImport(fmt::format("Attribute layer '{}' must be a JSON object.", layerName));
            }
            auto attrLayer = feature->attributeLayers()->newLayer(layerName);
            for (auto const& [attrName, attrValue] : layerValue.items()) {
                auto attribute = attrLayer->newAttribute(attrName);
                importAttributeObject(tile, feature, attribute, attrValue, options, deferredValidities);
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
                    auto result = attribute->addField(attrName, importGenericNode(tile, attrValue));
                    if (!result) {
                        raiseImport(result.error().message);
                    }
                }
            }
            continue;
        }

        auto result = feature->attributes()->addField(key, importGenericNode(tile, value));
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
    GeoJsonImportOptions const& options)
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
        importValidityCollection(tile, feature, relation->sourceValidity(), relationJson.at("sourceValidity"), options);
    }
    if (relationJson.contains("targetValidity")) {
        importValidityCollection(tile, feature, relation->targetValidity(), relationJson.at("targetValidity"), options);
    }

    feature->addRelation(relation);
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
            raiseImport("GLB attachment import is not supported yet.");
        }
        // Strict mode treats top-level metadata mismatches as caller/configuration errors.
        if (geoJson.contains("mapgetTileId") && geoJson.at("mapgetTileId").get<uint64_t>() != tile.tileId().value_) {
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
        if (featureJson.contains("properties")) {
            importProperties(tile, feature, featureJson.at("properties"), options, deferredValidities);
        }
        if (featureJson.contains("relations")) {
            deferredRelations.push_back(DeferredRelation{feature, featureJson.at("relations")});
        }
    }

    // Attribute validities may reference local features, so resolve them after all features exist.
    for (auto& deferred : deferredValidities) {
        importValidityCollection(
            tile,
            deferred.hostFeature_,
            deferred.attribute_->validity(),
            deferred.validityJson_,
            options);
    }

    // Relations are imported last for the same reason: all target ids must already be present.
    for (auto& deferred : deferredRelations) {
        if (!deferred.relationJson_.is_array()) {
            raiseImport("Feature relations must be encoded as an array.");
        }
        for (auto const& relationJson : deferred.relationJson_) {
            importRelation(tile, deferred.feature_, relationJson, options);
        }
    }
}

void TileFeatureLayer::fromJson(nlohmann::json const& json, GeoJsonImportOptions const& options)
{
    importGeoJson(*this, json, options);
}

}
