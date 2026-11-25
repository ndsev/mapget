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
#include <bit>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <span>
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
using BindValidityReference = std::function<model_ptr<ValidityReference>(uint32_t)>;

/** Raise a consistently prefixed import error. */
[[noreturn]] void raiseImport(std::string const& message)
{
    raiseFmt("PartitionFeatureLayer::fromJson: {}", message);
}

/** Check the JSON type and range before narrowing a wire ordinal. */
[[nodiscard]] uint32_t uint32FromJson(nlohmann::json const& value, std::string_view field)
{
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
    {
        raiseImport(fmt::format("{} must be an integer in uint32 range.", field));
    }
    return value.get<uint32_t>();
}

/** Do not let a JSON boolean or non-finite programmatic input become a position. */
[[nodiscard]] double positionNumber(nlohmann::json const& value)
{
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        raiseImport("A coordinate or validity offset must be a finite number.");
    return value.get<double>();
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
[[nodiscard]] std::optional<Validity::TransitionEnd>
transitionEndFromJson(nlohmann::json const& json)
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
[[nodiscard]] std::optional<Validity::GeometryOffsetType>
offsetTypeFromJson(nlohmann::json const& json)
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
        positionNumber(json.at(0)),
        positionNumber(json.at(1)),
        json.size() >= 3 ? positionNumber(json.at(2)) : 0.0};
}

/** Resolve a compact `$mapgetAttrPointSequence` token against imported definitions. */
[[nodiscard]] model_ptr<AttrPointSequence> importAttrPointSequenceReference(
    nlohmann::json const& json,
    AttrPointSequenceRegistry const& sequences)
{
    if (!json.is_object() || json.size() != 1 || !json.contains("$mapgetAttrPointSequence")) {
        raiseImport(
            "AttrPointSequence reference must be encoded as "
            "{'$mapgetAttrPointSequence': <non-negative integer>}.");
    }
    auto const index =
        uint32FromJson(json.at("$mapgetAttrPointSequence"), "$mapgetAttrPointSequence");
    if (index >= sequences.size()) {
        raiseImport(fmt::format(
            "AttrPointSequence reference {} is out of range ({} definitions).",
            index,
            sequences.size()));
    }
    return sequences[index];
}

/** Decode a JSON scalar into the storage type expected by an id-part definition. */
[[nodiscard]] std::variant<int64_t, std::string>
jsonToIdPartValue(nlohmann::json const& json, FeatureIdentity::Part idPart)
{
    if (json.is_number_unsigned()) {
        auto const value = json.get<uint64_t>();
        if (idPart.datatype() == IdPartDataType::U64)
            return std::bit_cast<int64_t>(value);
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            raiseImport(fmt::format("Id part '{}' exceeds int64_t range.", idPart.name()));
        }
        return static_cast<int64_t>(value);
    }
    if (json.is_number_integer()) {
        return json.get<int64_t>();
    }
    if (json.is_string()) {
        return json.get<std::string>();
    }
    raiseImport(fmt::format(
        "Id part '{}' must be encoded as an integer or string JSON value.",
        idPart.name()));
}

/** Validate an id-part value with the same rules used by binary feature creation. */
void validateIdPartValue(FeatureIdentity::Part idPart, std::variant<int64_t, std::string>& value)
{
    std::string error;
    if (!idPart.validateValue(value, &error)) {
        raiseImport(error);
    }
}

/** Only the exact singleton object is a native reference, not a producer object with extra keys. */
[[nodiscard]] bool isPropertyReference(nlohmann::json const& json)
{
    return json.is_object() && json.size() == 1 &&
        (json.contains("$mapgetRelation") || json.contains("$mapgetValidity") ||
         json.contains("$mapgetAttrPointSequence"));
}

/** A subtree without native references can go through simfil's shared tagged-JSON reader. */
[[nodiscard]] bool containsPropertyReference(nlohmann::json const& json)
{
    return isPropertyReference(json) ||
        (json.is_structured() && std::any_of(json.begin(), json.end(), containsPropertyReference));
}

/** Typed metadata and ignored extension members have no attribute to own a validity token. */
void rejectValidityReferences(nlohmann::json const& json)
{
    if (json.is_object() && json.size() == 1 && json.contains("$mapgetValidity"))
        raiseImport("$mapgetValidity is only valid inside an attribute's payload.");
    if (json.is_structured())
        std::for_each(json.begin(), json.end(), rejectValidityReferences);
}

/** A simfil multimap wraps every field, even a singleton array-valued field, in an outer array. */
template <class Fn>
void forPropertyValues(bool multimap, nlohmann::json const& value, Fn&& visit)
{
    if (!multimap) {
        visit(value);
        return;
    }
    if (!value.is_array())
        raiseImport("Expected an array of values for a multimap field.");
    for (auto const& item : value)
        visit(item);
}

/** Decode generic property trees, binding compact references in their original owner scope. */
[[nodiscard]] simfil::ModelNode::Ptr importGenericNode(
    PartitionFeatureLayer& tile,
    nlohmann::json const& json,
    model_ptr<Feature> const& feature,
    BindValidityReference const& bindValidity = {})
{
    if (isPropertyReference(json)) {
        if (auto it = json.find("$mapgetValidity"); it != json.end()) {
            if (!bindValidity)
                raiseImport("$mapgetValidity is only valid inside an attribute's payload.");
            return bindValidity(uint32FromJson(*it, "$mapgetValidity"));
        }
        if (auto it = json.find("$mapgetAttrPointSequence"); it != json.end()) {
            auto index = uint32FromJson(*it, "$mapgetAttrPointSequence");
            if (index >= tile.numAttrPointSequences())
                raiseImport(fmt::format("AttrPointSequence reference {} is out of range.", index));
            return tile.resolve<AttrPointSequenceReference>(simfil::ModelNodeAddress{
                PartitionFeatureLayer::ColumnId::AttrPointSequenceReferences,
                index});
        }
        auto relationIndex = uint32FromJson(json.at("$mapgetRelation"), "$mapgetRelation");
        if (relationIndex >= Relation::InvalidFeatureRelationIndex) {
            raiseImport("$mapgetRelation exceeds the supported relation-reference range.");
        }
        auto relation = feature->getRelation(relationIndex);
        if (!relation) {
            raiseImport(fmt::format("Feature has no relation at index {}.", relationIndex));
        }
        return tile.newRelationReference(relation);
    }

    if (containsPropertyReference(json)) {
        if (json.is_array()) {
            auto array = tile.newArray(json.size(), true);
            for (auto const& item : json) {
                array->append(importGenericNode(tile, item, feature, bindValidity));
            }
            return array;
        }
        if (json.is_object()) {
            bool const multimap = json.contains("_multimap") && json.at("_multimap") == true;
            auto object = tile.newObject(json.size(), !multimap);
            for (auto const& [key, value] : json.items()) {
                if (multimap && key == "_multimap")
                    continue;
                forPropertyValues(
                    multimap,
                    value,
                    [&](nlohmann::json const& item)
                    {
                        auto result = object->addField(
                            key,
                            importGenericNode(tile, item, feature, bindValidity));
                        if (!result)
                            raiseImport(result.error().message);
                    });
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
[[nodiscard]] std::optional<std::string> geometryNameFromJson(nlohmann::json const& json)
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
    case IdPartDataType::U64: return true;
    case IdPartDataType::UUID128:
    case IdPartDataType::STR: return false;
    }
    return false;
}

/**
 * Synthesize full feature-id parts for permissive GeoJSON import.
 *
 * The tile id is injected when the composition contains a `tileId` part, and
 * the first remaining integer slot is filled with the feature's collection index.
 * Synthetic identities therefore depend on input order, not geometry content.
 */
[[nodiscard]] KeyValuePairs bestEffortFullFeatureIdParts(
    PartitionFeatureLayer const& tile,
    std::string_view typeId,
    uint32_t fallbackFeatureIndex)
{
    auto typeInfo = tile.layerInfo()->getTypeInfo(typeId, false);
    if (!typeInfo || typeInfo.compositions().empty()) {
        raiseImport(
            fmt::format("Could not resolve feature type '{}' for best-effort import.", typeId));
    }

    auto const& composition = typeInfo.compositions().front();
    KeyValuePairs parts;
    parts.reserve(composition.parts().size());
    bool usedFeatureIndex = false;

    for (auto idPart : composition.parts()) {
        if (idPart.name() == "tileId") {
            if (!isIntegerIdPart(idPart.datatype())) {
                raiseImport("Best-effort GeoJSON import requires an integer tileId id part.");
            }
            parts.emplace_back(
                idPart.name(),
                tile.partitionId().kind() == PartitionKind::Object ?
                    std::bit_cast<int64_t>(tile.partitionId().objectId()) :
                    static_cast<int64_t>(tile.tileId().value()));
            continue;
        }

        if (!usedFeatureIndex && isIntegerIdPart(idPart.datatype())) {
            // Best-effort mode needs one deterministic per-feature slot to keep ids stable.
            parts.emplace_back(idPart.name(), static_cast<int64_t>(fallbackFeatureIndex));
            usedFeatureIndex = true;
            continue;
        }

        if (idPart.optional()) {
            // Optional suffix parts may be dropped because no source field exists to recover them.
            continue;
        }

        raiseImport(fmt::format(
            "Could not synthesize best-effort feature ids for type '{}'. "
            "Provide explicit id fields for required part '{}'.",
            typeId,
            idPart.name()));
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
    FeatureIdentity::Composition composition)
{
    KeyValuePairs result;
    result.reserve(composition.parts().size());

    for (auto idPart : composition.parts()) {
        auto it = featureJson.find(idPart.name());
        if (it == featureJson.end()) {
            if (idPart.optional()) {
                continue;
            }
            raiseImport(fmt::format(
                "Feature is missing non-optional id field '{}' required by the primary "
                "composition.",
                idPart.name()));
        }
        auto value = jsonToIdPartValue(*it, idPart);
        validateIdPartValue(idPart, value);
        result.emplace_back(idPart.name(), std::move(value));
    }

    return result;
}

/** Import source-data references from the JSON representation used by toJson(). */
[[nodiscard]] std::optional<model_ptr<SourceDataReferenceCollection>>
importSourceDataReferences(PartitionFeatureLayer& tile, nlohmann::json const& json)
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
[[nodiscard]] ParsedFeatureReferenceJson
parseFeatureReferenceJson(PartitionFeatureLayer& tile, nlohmann::json const& json)
{
    std::string canonicalId;
    std::optional<std::string> externalMapId;

    if (json.is_string()) {
        canonicalId = json.get<std::string>();
    }
    else if (json.is_object()) {
        auto const idIt = json.find("id");
        if (auto mapIdIt = json.find("mapId"); mapIdIt != json.end()) {
            if (!mapIdIt->is_string() || mapIdIt->get_ref<std::string const&>().empty()) {
                raiseImport("Feature reference `mapId` must be a non-empty string.");
            }
            externalMapId = mapIdIt->get<std::string>();
        }
        if (idIt != json.end() && idIt->is_object()) {
            auto type = json.find("typeId");
            if (!externalMapId || type == json.end() || !type->is_string() ||
                type->get_ref<std::string const&>().empty())
                raiseImport("Named feature references require non-empty `mapId` and `typeId`.");
            ParsedFeatureReferenceJson result;
            result.externalMapId_ = std::move(externalMapId);
            result.featureId_.typeId_ = type->get<std::string>();
            result.featureId_.keyValuePairs_ = parseFeatureIdPartsJson(*idIt);
            if (*result.externalMapId_ == tile.mapId()) {
                ParsedFeatureId local;
                if (!normalizeFeatureIdParts(
                        result.featureId_.typeId_,
                        result.featureId_.keyValuePairs_,
                        *tile.layerInfo(),
                        local))
                    raiseImport("Named local feature reference does not match an ID composition.");
                result.featureId_ = std::move(local);
            }
            return result;
        }
        if (idIt == json.end() || !idIt->is_string())
            raiseImport(
                "Feature references require a canonical local string or named `id` object.");
        if (externalMapId && *externalMapId != tile.mapId())
            raiseImport("Foreign feature references require `typeId` and named `id` parts.");
        canonicalId = idIt->get<std::string>();
    }
    else {
        raiseImport(
            "Feature references require a canonical local string or `{mapId, typeId, id}` with "
            "named parts.");
    }

    ParsedFeatureReferenceJson parsed;
    std::string error;
    if (!parseFeatureIdString(canonicalId, *tile.layerInfo(), parsed.featureId_, &error)) {
        raiseImport(error);
    }
    parsed.externalMapId_ = std::move(externalMapId);
    return parsed;
}

/** Import a local canonical ID or a self-describing foreign reference. */
[[nodiscard]] model_ptr<FeatureId>
importFeatureReferenceId(PartitionFeatureLayer& tile, nlohmann::json const& json)
{
    auto parsed = parseFeatureReferenceJson(tile, json);
    auto partsView = castToKeyValueView(parsed.featureId_.keyValuePairs_);
    auto externalMapId = parsed.externalMapId_ ?
        std::optional<std::string_view>(*parsed.externalMapId_) :
        std::nullopt;
    return tile.newFeatureId(parsed.featureId_.typeId_, partsView, externalMapId);
}

[[nodiscard]] model_ptr<Geometry> importStandaloneGeometry(
    PartitionFeatureLayer& tile,
    nlohmann::json const& geometryJson,
    GeoJsonImportOptions const& options);

/** Apply metadata that is shared by all geometry encodings. */
void applyGeometryDecorations(
    PartitionFeatureLayer& tile,
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
[[nodiscard]] model_ptr<Geometry>
importPolygonGeometry(PartitionFeatureLayer& tile, nlohmann::json const& coords)
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

/** Import one geometry without aggregate expansion. Nested collections and
 * multi-lines are rejected; strict MultiPolygon input represents a triangle mesh.
 */
[[nodiscard]] model_ptr<Geometry> importStandaloneGeometry(
    PartitionFeatureLayer& tile,
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
                raiseImport(
                    "Strict mapget mesh import expects every MultiPolygon item to contain exactly "
                    "one ring.");
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
    PartitionFeatureLayer& tile,
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
            auto geometry = importStandaloneGeometry(
                tile,
                child,
                GeoJsonImportOptions{
                    options.strict_,
                    options.fallbackFeatureType_,
                    options.objectPropertiesAsAttributeLayers_});
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
                raiseImport(
                    "Generic MultiPolygon import expects every polygon to contain at least one "
                    "ring.");
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

/** Borrow the singleton or array elements from the immutable import document. */
[[nodiscard]] std::span<nlohmann::json const> validityEntries(nlohmann::json const& json)
{
    if (json.is_array())
        return json.get_ref<nlohmann::json::array_t const&>();
    return {&json, 1};
}

/** Append stored validity entries after shared sequence definitions exist. */
void importValidityCollection(
    PartitionFeatureLayer& tile,
    model_ptr<MultiValidity> collection,
    std::span<nlohmann::json const> values,
    GeoJsonImportOptions const& options,
    AttrPointSequenceRegistry const& attrPointSequences)
{
    for (auto const& value : values) {
        if (!value.is_object()) {
            raiseImport("Validity entries must be JSON objects.");
        }

        std::optional<Validity::Direction> direction;
        if (value.contains("direction")) {
            direction = directionFromJson(value.at("direction"));
            if (!direction) {
                raiseImport(
                    fmt::format("Unknown validity direction '{}'.", value.at("direction").dump()));
            }
            if (value.size() == 1) {
                collection->newDirection(*direction);
                continue;
            }
        }
        auto validity = tile.newValidity();
        if (direction)
            validity->setDirection(*direction);
        if (auto tag = value.find("tag"); tag != value.end()) {
            if (!tag->is_string() || tag->get_ref<std::string const&>().empty())
                raiseImport("Validity tag must be a non-empty string.");
            validity->setTag(tag->get_ref<std::string const&>());
        }
        if (auto name = geometryNameFromJson(value)) {
            validity->setGeometryName(*name);
        }

        if (value.contains("from") || value.contains("to")) {
            // Transition endpoints are semantic IDs and may point outside the current tile.
            if (!value.contains("from") || !value.contains("to") ||
                !value.contains("fromConnectedEnd") || !value.contains("toConnectedEnd") ||
                !value.contains("transitionNumber"))
            {
                raiseImport(
                    "Feature transition validities must define from/to ids, connected ends, and "
                    "transitionNumber.");
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
                uint32FromJson(value.at("transitionNumber"), "transitionNumber"));
            collection->append(validity);
            continue;
        }

        if (value.contains("geometry")) {
            validity
                ->setSimpleGeometry(importStandaloneGeometry(tile, value.at("geometry"), options));
            collection->append(validity);
            continue;
        }

        if (value.contains("attrPointIndex")) {
            auto const& indexJson = value.at("attrPointIndex");
            if (!indexJson.is_object() || !indexJson.contains("sequence") ||
                !indexJson.contains("index")) {
                raiseImport("attrPointIndex must define sequence and index fields.");
            }
            validity->setAttrPointIndex(
                importAttrPointSequenceReference(indexJson.at("sequence"), attrPointSequences),
                uint32FromJson(indexJson.at("index"), "attrPointIndex.index"));
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
                importAttrPointSequenceReference(rangeJson.at("sequence"), attrPointSequences),
                uint32FromJson(rangeJson.at("start"), "attrPointIndexRange.start"),
                uint32FromJson(rangeJson.at("end"), "attrPointIndexRange.end"));
            collection->append(validity);
            continue;
        }

        if (value.contains("featureId")) {
            validity->setFeatureId(importFeatureReferenceId(tile, value.at("featureId")));
        }

        auto offsetType = value.contains("offsetType") ?
            offsetTypeFromJson(value.at("offsetType")) :
            std::optional<Validity::GeometryOffsetType>{};
        if (value.contains("offsetType") && !offsetType) {
            raiseImport(
                fmt::format("Unknown validity offsetType '{}'.", value.at("offsetType").dump()));
        }

        if (value.contains("point")) {
            if (offsetType && *offsetType != Validity::GeoPosOffset) {
                // Non-geospatial offsets reuse the `point` field name for historic JSON
                // compatibility.
                validity->setOffsetPoint(*offsetType, positionNumber(value.at("point")));
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
                    positionNumber(value.at("start")),
                    positionNumber(value.at("end")));
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

/** An exported suffix can contain stored points with the same tag as a virtual series.
 * Only discard the suffix the fully populated occurrence can actually regenerate. */
struct DeferredMarkerTail
{
    model_ptr<Feature> hostFeature_;
    model_ptr<Attribute> attribute_;
    std::string_view layerName_;
    std::span<nlohmann::json const> entries_;
    size_t firstCandidate_;

    void finish(
        PartitionFeatureLayer& tile,
        GeoJsonImportOptions const& options,
        AttrPointSequenceRegistry const& sequences)
    {
        auto stored = attribute_->validityOrNull();
        auto const storedCount = stored ? stored->size() : 0;
        auto view = attribute_->validityView(hostFeature_, layerName_);
        auto const markerCount = view.size() - storedCount;
        size_t keep = entries_.size();
        if (markerCount <= keep - firstCandidate_) {
            auto tail = entries_.last(markerCount);
            bool matches = true;
            for (uint32_t i = 0; i < markerCount; ++i) {
                auto marker = view.at(storedCount + i);
                if (!marker || marker->toJson() != tail[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches)
                keep -= markerCount;
        }
        if (storedCount > keep)
            raiseImport("$mapgetValidity cannot reference a record-derived marker.");
        if (keep > storedCount)
            importValidityCollection(
                tile,
                attribute_->validity(),
                entries_.subspan(storedCount, keep - storedCount),
                options,
                sequences);
    }
};

/** Defer relation import until feature IDs, host geometry and shared
 * AttrPointSequence definitions exist.
 */
struct DeferredRelation
{
    model_ptr<Feature> feature_;
    nlohmann::json const& relationJson_;
};

/** Defer property import until top-level relations are available for
 * `$mapgetRelation` references.
 */
struct DeferredProperties
{
    model_ptr<Feature> feature_;
    nlohmann::json const& propertiesJson_;
};

/** Only record-position declarations synthesize tagged points. TaggedValidity reads stored ones. */
[[nodiscard]] bool
mayBeProjectedMarker(nlohmann::json const& entry, std::span<PointSeries const> declarations)
{
    auto tag = entry.find("tag");
    if (tag == entry.end() || !tag->is_string() || !entry.contains("point") ||
        entry.contains("direction"))
        return false;
    return std::ranges::any_of(
        declarations,
        [&](PointSeries const& series)
        {
            return series.position_.kind_ != PointSeries::Position::Kind::TaggedValidity &&
                !series.markerTag_.empty() &&
                series.markerTag_ == tag->get_ref<std::string const&>();
        });
}

/** Import one occurrence's stored validities before binding its property references.
 * JSON member order is not the dependency order: a reference may precede `validity`. */
void importAttributeObject(
    PartitionFeatureLayer& tile,
    model_ptr<Feature> const& hostFeature,
    std::string_view layerName,
    model_ptr<Attribute> attribute,
    nlohmann::json const& json,
    GeoJsonImportOptions const& options,
    AttrPointSequenceRegistry const& sequences,
    std::vector<DeferredMarkerTail>& markerTails)
{
    if (!json.is_object() || isPropertyReference(json)) {
        raiseImport(fmt::format("Attribute '{}' must be encoded as an object.", attribute->name()));
    }

    bool const multimap = json.contains("_multimap") && json.at("_multimap") == true;
    std::span<nlohmann::json const> entries;
    if (auto it = json.find("validity"); it != json.end()) {
        rejectValidityReferences(*it);
        if (multimap && (!it->is_array() || it->size() != 1))
            raiseImport("An attribute multimap must have exactly one validity field.");
        entries = validityEntries(multimap ? it->front() : *it);
        if (entries.empty())
            (void)attribute->validity();  // Explicit [] is not omitted applicability.
    }

    size_t firstCandidate = entries.size();
    if (auto schema = tile.layerSchema(); schema && !entries.empty()) {
        auto declarations =
            schema->pointSeries(hostFeature->typeId(), layerName, attribute->name());
        while (firstCandidate && mayBeProjectedMarker(entries[firstCandidate - 1], declarations))
            --firstCandidate;
    }

    size_t storedCount = 0;
    auto storeThrough = [&](size_t count)
    {
        if (count > storedCount) {
            importValidityCollection(
                tile,
                attribute->validity(),
                entries.subspan(storedCount, count - storedCount),
                options,
                sequences);
            storedCount = count;
        }
    };
    storeThrough(firstCandidate);
    BindValidityReference bindValidity = [&](uint32_t index)
    {
        if (index >= entries.size())
            raiseImport(fmt::format(
                "Attribute '{}' has no validity at index {}.",
                attribute->name(),
                index));
        // A stored point can use a series' tag. Materialize its slot first; finish()
        // rejects links into the actual derived suffix once all raw records are available.
        storeThrough(static_cast<size_t>(index) + 1);
        return tile.newValidityReference(attribute, index);
    };

    for (auto const& [key, value] : json.items()) {
        if ((multimap && key == "_multimap") || key == "validity") {
            continue;
        }
        forPropertyValues(
            multimap,
            value,
            [&](nlohmann::json const& item)
            {
                if (key == "_sourceData" || key == "sourceData") {
                    rejectValidityReferences(item);
                    if (auto refs = importSourceDataReferences(tile, item))
                        attribute->setSourceDataReferences(*refs);
                    return;
                }
                auto result = attribute->addField(
                    key,
                    importGenericNode(tile, item, hostFeature, bindValidity));
                if (!result)
                    raiseImport(result.error().message);
            });
    }
    if (firstCandidate < entries.size())
        markerTails.push_back({hostFeature, attribute, layerName, entries, firstCandidate});
}

/** Import one attribute-layer object, preserving duplicate attribute names and optional instance id
 * metadata. */
void importAttributeLayerObject(
    PartitionFeatureLayer& tile,
    model_ptr<Feature> feature,
    std::string const& layerName,
    nlohmann::json const& layerJson,
    GeoJsonImportOptions const& options,
    AttrPointSequenceRegistry const& sequences,
    std::vector<DeferredMarkerTail>& markerTails)
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
                raiseImport(
                    fmt::format("Attribute layer '{}' id must not be negative.", layerName));
            }
            attrLayer->setId(static_cast<uint64_t>(layerId));
            continue;
        }

        auto importOneAttribute = [&](nlohmann::json const& attributeJson)
        {
            auto attribute = attrLayer->newAttribute(attrName);
            importAttributeObject(
                tile,
                feature,
                layerName,
                attribute,
                attributeJson,
                options,
                sequences,
                markerTails);
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
    PartitionFeatureLayer& tile,
    model_ptr<Feature> feature,
    nlohmann::json const& propertiesJson,
    GeoJsonImportOptions const& options,
    AttrPointSequenceRegistry const& sequences,
    std::vector<DeferredMarkerTail>& markerTails)
{
    if (!propertiesJson.is_object() || isPropertyReference(propertiesJson)) {
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
                    importAttributeLayerObject(
                        tile,
                        feature,
                        layerName,
                        layerElement,
                        options,
                        sequences,
                        markerTails);
                }
            }
            else {
                importAttributeLayerObject(
                    tile,
                    feature,
                    layerName,
                    layerValue,
                    options,
                    sequences,
                    markerTails);
            }
        }
    }

    bool const multimap = propertiesJson.contains("_multimap") &&
        propertiesJson.at("_multimap") == true;
    for (auto const& [key, field] : propertiesJson.items()) {
        if (key == "layer" || (multimap && key == "_multimap")) {
            // `properties.layer` is reserved for exported attribute layers.
            continue;
        }
        forPropertyValues(
            multimap,
            field,
            [&](nlohmann::json const& value)
            {
                if (!options.strict_ && options.objectPropertiesAsAttributeLayers_ &&
                    value.is_object() && !isPropertyReference(value))
                {
                    // Best-effort mode may reinterpret nested objects as attribute layers for plain
                    // GeoJSON.
                    auto attrLayer = feature->attributeLayers()->newLayer(key);
                    for (auto const& [attrName, attrValue] : value.items()) {
                        auto attribute = attrLayer->newAttribute(attrName);
                        if (attrValue.is_object()) {
                            importAttributeObject(
                                tile,
                                feature,
                                key,
                                attribute,
                                attrValue,
                                options,
                                sequences,
                                markerTails);
                        }
                        else {
                            auto result = attribute->addField(
                                attrName,
                                importGenericNode(tile, attrValue, feature));
                            if (!result) {
                                raiseImport(result.error().message);
                            }
                        }
                    }
                    return;
                }

                auto result =
                    feature->attributes()->addField(key, importGenericNode(tile, value, feature));
                if (!result) {
                    raiseImport(result.error().message);
                }
            });
    }
}

/** Import one relation object after feature ids are already resolvable. */
void importRelation(
    PartitionFeatureLayer& tile,
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
            relation->sourceValidity(),
            validityEntries(relationJson.at("sourceValidity")),
            options,
            attrPointSequences);
    }
    if (relationJson.contains("targetValidity")) {
        importValidityCollection(
            tile,
            relation->targetValidity(),
            validityEntries(relationJson.at("targetValidity")),
            options,
            attrPointSequences);
    }

    feature->addRelation(relation);
}

/** Resolve one geometry by stable feature-local ordinal. */
[[nodiscard]] model_ptr<Geometry>
featureGeometryAt(model_ptr<Feature> const& feature, uint32_t geometryIndex)
{
    auto geometries = feature->geomOrNull();
    if (!geometries) {
        raiseImport(fmt::format(
            "AttrPointSequence host feature '{}' has no geometry.",
            feature->id()->toString()));
    }

    uint32_t currentIndex = 0;
    model_ptr<Geometry> result;
    geometries->forEachGeometry(
        [&](model_ptr<Geometry> const& geometry)
        {
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
[[nodiscard]] AttrPointSequenceRegistry
importAttrPointSequences(PartitionFeatureLayer& tile, nlohmann::json const& geoJson)
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
            !sequenceJson.contains("featureId") || !sequenceJson.contains("geometryIndex") ||
            !sequenceJson.contains("attrPoints"))
        {
            raiseImport(
                "Every AttrPointSequence must define id, featureId, geometryIndex, and "
                "attrPoints.");
        }
        auto const expectedId = result.size();
        if (uint32FromJson(sequenceJson.at("id"), "AttrPointSequence id") != expectedId) {
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
            uint32FromJson(sequenceJson.at("geometryIndex"), "geometryIndex"));
        if (auto geometryName = sequenceJson.find("geometryName");
            geometryName != sequenceJson.end()) {
            if (!geometryName->is_string() || geometry->name() != geometryName->get<std::string>())
            {
                raiseImport(
                    "AttrPointSequence geometryName does not match its referenced geometry.");
            }
        }

        auto sequence = tile.newAttrPointSequence(feature, geometry);
        if (!sequenceJson.at("attrPoints").is_array()) {
            raiseImport("AttrPointSequence attrPoints must be an array.");
        }
        for (auto const& pointJson : sequenceJson.at("attrPoints")) {
            if (!pointJson.is_object() || !pointJson.contains("index") ||
                !pointJson.contains("point")) {
                raiseImport("Every AttrPoint must define index and point fields.");
            }
            model_ptr<SourceDataReferenceCollection> pointSourceData;
            if (auto sourceDataJson = findSourceDataJson(pointJson)) {
                if (auto refs = importSourceDataReferences(tile, *sourceDataJson)) {
                    pointSourceData = *refs;
                }
            }
            sequence->appendAttrPoint(
                uint32FromJson(pointJson.at("index"), "AttrPoint index"),
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
    PartitionFeatureLayer const& tile,
    nlohmann::json const& featureJson,
    GeoJsonImportOptions const& options)
{
    if (auto typeIt = featureJson.find("typeId");
        typeIt != featureJson.end() && typeIt->is_string()) {
        return typeIt->get<std::string>();
    }
    if (options.fallbackFeatureType_) {
        return *options.fallbackFeatureType_;
    }
    auto const producedTypes = tile.layerInfo()->layerSchema()->featureTypes();
    if (producedTypes.size() == 1) {
        return producedTypes.front();
    }
    raiseImport(
        "Could not determine feature type. Missing 'typeId' and no fallback feature type was "
        "configured.");
    return {};
}

}  // namespace

/** Import a FeatureCollection into an empty PartitionFeatureLayer. */
void importGeoJson(
    PartitionFeatureLayer& tile,
    nlohmann::json const& geoJson,
    GeoJsonImportOptions const& options)
{
    if (tile.numRoots() != 0) {
        raiseImport("Import requires an empty PartitionFeatureLayer instance.");
    }
    if (tile.getIdPrefix()) {
        // GeoJSON import now treats full ids as the canonical representation.
        raiseImport("Import requires a PartitionFeatureLayer without a preconfigured idPrefix.");
    }
    if (!geoJson.is_object() || geoJson.value("type", "") != "FeatureCollection") {
        raiseImport("GeoJSON root must be a FeatureCollection.");
    }
    if (!geoJson.contains("features") || !geoJson.at("features").is_array()) {
        raiseImport("GeoJSON FeatureCollection is missing an array-valued 'features' field.");
    }
    for (auto const& [key, value] : geoJson.items())
        if (key != "features")
            rejectValidityReferences(value);

    if (options.strict_) {
        if (geoJson.contains("glbAttachment")) {
            raiseImport(
                "Embedded GLB attachment import is not supported; "
                "use glbAttachmentName and the attachment API.");
        }
        // Strict mode treats top-level metadata mismatches as caller/configuration errors.
        if (geoJson.contains("partition") &&
            PartitionId::fromJson(geoJson.at("partition")) != tile.partitionId())
            raiseImport("partition does not match the target layer.");
        if (geoJson.contains("mapgetTileId") &&
            geoJson.at("mapgetTileId").get<int32_t>() != tile.tileId().value())
        {
            raiseImport("mapgetTileId does not match the target tile.");
        }
        if (geoJson.contains("mapId") && geoJson.at("mapId").get<std::string>() != tile.mapId()) {
            raiseImport("mapId does not match the target tile.");
        }
        if (geoJson.contains("mapgetLayerId") &&
            geoJson.at("mapgetLayerId").get<std::string>() != tile.layerInfo()->layerId_)
        {
            raiseImport("mapgetLayerId does not match the target tile layer.");
        }
    }

    if (geoJson.contains("geometryAnchor")) {
        tile.setGeometryAnchor(pointFromCoordinateJson(geoJson.at("geometryAnchor")));
    }
    if (auto attachmentName = geoJson.find("glbAttachmentName"); attachmentName != geoJson.end()) {
        if (!attachmentName->is_string()) {
            raiseImport("glbAttachmentName must be a string.");
        }
        tile.setGlbAttachmentName(attachmentName->get<std::string>());
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
    if (geoJson.contains("warnings")) {
        auto const& warnings = geoJson.at("warnings");
        if (!warnings.is_array())
            raiseImport("warnings must be an array of strings.");
        for (auto const& warning : warnings) {
            if (!warning.is_string())
                raiseImport("warnings must be an array of strings.");
        }
        tile.setWarnings(warnings.get<std::vector<std::string>>());
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

    std::vector<DeferredMarkerTail> markerTails;
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
        for (auto const& [key, value] : featureJson.items())
            if (key != "properties" && key != "attributes")
                rejectValidityReferences(value);

        auto const typeId = determineFeatureType(tile, featureJson, options);
        KeyValuePairs fullIdParts;

        if (options.strict_ || featureJson.contains("typeId")) {
            auto typeInfo = tile.layerInfo()->getTypeInfo(typeId, false);
            if (!typeInfo || typeInfo.compositions().empty()) {
                raiseImport(fmt::format("Could not resolve feature type '{}' for import.", typeId));
            }
            fullIdParts =
                fullFeatureIdPartsFromFields(featureJson, typeInfo.compositions().front());
            if (featureJson.contains("id")) {
                ParsedFeatureId parsed;
                std::string error;
                if (!parseFeatureIdString(
                        featureJson.at("id").get<std::string>(),
                        *tile.layerInfo(),
                        parsed,
                        &error))
                {
                    raiseImport(error);
                }
                if (parsed.typeId_ != typeId || parsed.keyValuePairs_ != fullIdParts) {
                    raiseImport(fmt::format(
                        "Feature id '{}' does not match explicit id-part fields.",
                        featureJson.at("id").get<std::string>()));
                }
            }
            // Strict import reconstructs the full id verbatim instead of reintroducing tile
            // prefixes.
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

    // Shared interwoven types must exist before relation or attribute
    // validities resolve their compact sequence references.
    auto attrPointSequences = importAttrPointSequences(tile, geoJson);

    // All target IDs are now present, and relation validities can resolve shared
    // sequence definitions. Feature-local relation ordinals must exist before properties
    // import relation tokens.
    for (auto& deferred : deferredRelations) {
        if (!deferred.relationJson_.is_array()) {
            raiseImport("Feature relations must be encoded as an array.");
        }
        for (auto const& relationJson : deferred.relationJson_) {
            importRelation(tile, deferred.feature_, relationJson, options, attrPointSequences);
        }
    }

    // Properties are imported after relations so nested `$mapgetRelation`
    // tokens can resolve to the owning feature's canonical relation objects.
    for (auto& deferred : deferredProperties) {
        importProperties(
            tile,
            deferred.feature_,
            deferred.propertiesJson_,
            options,
            attrPointSequences,
            markerTails);
    }

    // A series may select a branch using a host property imported after its attribute layer.
    for (auto& tail : markerTails)
        tail.finish(tile, options, attrPointSequences);
}

void PartitionFeatureLayer::fromJson(
    nlohmann::json const& json,
    GeoJsonImportOptions const& options)
{
    importGeoJson(*this, json, options);
}

}  // namespace mapget
