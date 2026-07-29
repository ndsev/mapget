#include "stringpool.h"
#include "mapget/log.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

namespace mapget
{

StringPool::StringPool(const std::string_view& stringPoolId) : stringPoolId_(stringPoolId) {
    addStaticKey(IdStr, "id");
    addStaticKey(TypeIdStr, "typeId");
    addStaticKey(MapIdStr, "mapId");
    addStaticKey(LayerIdStr, "layerId");
    addStaticKey(LayerStr, "layer");
    addStaticKey(RelationsStr, "relations");
    addStaticKey(DirectionStr, "direction");
    addStaticKey(ValidityStr, "validity");
    addStaticKey(PropertiesStr, "properties");
    addStaticKey(NameStr, "name");
    addStaticKey(TargetStr, "target");
    addStaticKey(SourceValidityStr, "sourceValidity");
    addStaticKey(TargetValidityStr, "targetValidity");
    addStaticKey(LonStr, "lon");
    addStaticKey(LatStr, "lat");
    addStaticKey(GeometryStr, "geometry");
    addStaticKey(GeometryNameStr, "geometryName");
    addStaticKey(GeometriesStr, "geometries");
    addStaticKey(TypeStr, "type");
    addStaticKey(OffsetTypeStr, "offsetType");
    addStaticKey(CoordinatesStr, "coordinates");
    addStaticKey(ElevationStr, "elevation");
    addStaticKey(SourceDataStr, "_sourceData");
    addStaticKey(AddressStr, "address");
    addStaticKey(QualifierStr, "qualifier");
    addStaticKey(StartStr, "start");
    addStaticKey(EndStr, "end");
    addStaticKey(PointStr, "point");
    addStaticKey(FeatureIdStr, "featureId");
    addStaticKey(AabbStr, "aabb");
    addStaticKey(OriginStr, "origin");
    addStaticKey(SizeStr, "size");
    addStaticKey(GltfNodeIndexStr, "gltfNodeIndex");
    addStaticKey(FromStr, "from");
    addStaticKey(ToStr, "to");
    addStaticKey(ConnectedEndStr, "connectedEnd");
    addStaticKey(FromConnectedEndStr, "fromConnectedEnd");
    addStaticKey(ToConnectedEndStr, "toConnectedEnd");
    addStaticKey(TransitionNumberStr, "transitionNumber");
    addStaticKey(AttributeIndexStr, "attributeIndex");
    addStaticKey(ValuesStr, "values");
    addStaticKey(MatchStr, "match");
    addStaticKey(ValidityIndexStr, "validityIndex");
    addStaticKey(ValidityCountStr, "validityCount");
    addStaticKey(OverlayNameStr, "$name");
    addStaticKey(OverlayFeatureStr, "$feature");
    addStaticKey(OverlayFeaturesStr, "$features");
    addStaticKey(OverlaySourceStr, "$source");
    addStaticKey(OverlayTargetStr, "$target");
    addStaticKey(OverlayTwowayStr, "$twoway");
    addStaticKey(OverlayLayerStr, "$layer");
    addStaticKey(OverlayValidityIndexStr, "$validityIndex");
    addStaticKey(OverlayValidityCountStr, "$validityCount");
    addStaticKey(OverlayHasValidityStr, "$hasValidity");
    addStaticKey(CallsStr, "calls");
    addStaticKey(TotalUsStr, "totalus");
    addStaticKey(MapgetRelationStr, "$mapgetRelation");
    addStaticKey(AttributesStr, "attributes");
    addStaticKey(ChannelIdStr, "channelId");
    addStaticKey(ScopeStr, "scope");
    addStaticKey(GeometryTypesStr, "geometryTypes");
    addStaticKey(FeatureFieldsStr, "featureFields");
    addStaticKey(EntryFieldsStr, "entryFields");
    addStaticKey(FeatureEntriesStr, "featureEntries");
    addStaticKey(AttributeValidityEntriesStr, "attributeValidityEntries");
    addStaticKey(RelationEntriesStr, "relationEntries");
    addStaticKey(GroupEntriesStr, "groupEntries");
    addStaticKey(HostValuesStr, "hostValues");
    addStaticKey(AttributeLayerStr, "attributeLayer");
    addStaticKey(AttributeNameStr, "attributeName");
    addStaticKey(HasValidityStr, "hasValidity");
    addStaticKey(RelationIdStr, "relationId");
    addStaticKey(ProvenanceStr, "provenance");
    addStaticKey(TwowayStr, "twoway");
    addStaticKey(SourceStr, "source");
    addStaticKey(SourceGeometryStr, "sourceGeometry");
    addStaticKey(TargetGeometryStr, "targetGeometry");
    addStaticKey(GroupKeyStr, "groupKey");
    addStaticKey(RepresentativeFeatureIdStr, "representativeFeatureId");
    addStaticKey(MemberFeatureIdsStr, "memberFeatureIds");
    addStaticKey(FilterIdStr, "filterId");
    addStaticKey(GenerationStr, "generation");
    addStaticKey(SourceFeatureCountStr, "sourceFeatureCount");
    addStaticKey(OverlayAttributeIndexStr, "$attributeIndex");
    addStaticKey(OverlayRelationIndexStr, "$relationIndex");
}

tl::expected<void, simfil::Error>
StringPool::write(std::ostream& outputStream, simfil::StringId offset) const
{
    // Write the node id which identifies the string pool.
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    s.text1b(stringPoolId_, std::numeric_limits<uint32_t>::max());
    return simfil::StringPool::write(outputStream, offset);
}

std::string StringPool::readDataSourceStringPoolId(
    const std::vector<uint8_t>& input,
    size_t offset,
    size_t* bytesRead)
{
    if (offset > input.size()) {
        raise("Failed to read StringPool node id: invalid input offset.");
    }

    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(offset),
        input.end()));

    // Read the node id which identifies the string pool.
    std::string stringPoolStringPoolId;
    s.text1b(stringPoolStringPoolId, std::numeric_limits<uint32_t>::max());
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raiseFmt(
            "Failed to read StringPool node id: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error()));
    }
    if (bytesRead != nullptr) {
        *bytesRead = s.adapter().currentReadPos();
    }
    return stringPoolStringPoolId;
}

}
