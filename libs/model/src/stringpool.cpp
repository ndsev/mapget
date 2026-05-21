#include "stringpool.h"
#include "mapget/log.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

namespace mapget
{

StringPool::StringPool(const std::string_view& nodeId) : nodeId_(nodeId) {
    addStaticKey(IdStr, "id");
    addStaticKey(TypeIdStr, "typeId");
    addStaticKey(LodStr, "lod");
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
    addStaticKey(AttributePathStr, "attributePath");
    addStaticKey(ValidityIndexStr, "validityIndex");
    addStaticKey(ValidityCountStr, "validityCount");
}

tl::expected<void, simfil::Error>
StringPool::write(std::ostream& outputStream, simfil::StringId offset) const
{
    // Write the node id which identifies the string pool.
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    s.text1b(nodeId_, std::numeric_limits<uint32_t>::max());
    return simfil::StringPool::write(outputStream, offset);
}

std::string StringPool::readDataSourceNodeId(
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
    std::string stringPoolNodeId;
    s.text1b(stringPoolNodeId, std::numeric_limits<uint32_t>::max());
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raiseFmt(
            "Failed to read StringPool node id: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error()));
    }
    if (bytesRead != nullptr) {
        *bytesRead = s.adapter().currentReadPos();
    }
    return stringPoolNodeId;
}

}
