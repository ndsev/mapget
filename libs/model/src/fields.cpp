#include "fields.h"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>

namespace mapget
{

Fields::Fields(const std::string_view& nodeId) : nodeId_(nodeId) {
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
    addStaticKey(GeometriesStr, "geometries");
    addStaticKey(TypeStr, "type");
    addStaticKey(CoordinatesStr, "coordinates");
    addStaticKey(ElevationStr, "elevation");
}

void Fields::write(std::ostream& outputStream, simfil::FieldId offset) const
{
    // Write the node id which identifies the fields dictionary
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    s.text1b(nodeId_, std::numeric_limits<uint32_t>::max());
    simfil::Fields::write(outputStream, offset);
}

std::string Fields::readDataSourceNodeId(std::istream& inputStream) {
    // Read the node id which identifies the fields dictionary
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(inputStream);
    std::string fieldsDictNodeId;
    s.text1b(fieldsDictNodeId, std::numeric_limits<uint32_t>::max());
    return fieldsDictNodeId;
}

}
