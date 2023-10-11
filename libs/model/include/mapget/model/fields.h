#pragma once

#include "simfil/model/fields.h"

namespace mapget
{

/**
 * The Fields class is a case-insensitive dictionary for field names.
 * It acts as a container that automatically populates itself when fields are added to an object.
 * Multiple TileFeatureLayers can share the same Fields dictionary, which helps reduce the size
 * of serialized FeatureLayers.
 *
 * The inherited mapget::Fields provides additional static field IDs that can be used for various purposes.
 *
 * Field IDs managed by the Fields dictionary are stored as uint16 values, which are smaller and faster
 * to work with compared to strings. When querying fields, the Fields dictionary is used to look up the
 * field ID for a given name. Once the ID is obtained, subsequent searches are performed using this 16-bit
 * integer, resulting in improved efficiency.
 *
 * Note: A fields dictionary is always unique per datasource node. Therefore,
 * the Fields object must be constructed with a datasource node id.
 */
struct Fields : public simfil::Fields
{
    enum StaticFieldIds : simfil::FieldId {
        IdStr = NextStaticId,
        TypeIdStr,
        LayerStr,
        ChildrenStr,
        DirectionStr,
        ValidityStr,
        PropertiesStr,
    };

    explicit Fields(const std::string_view& nodeId);

    /**
     * Write is overloaded, because it prepends the stream with
     * this dictionary's data source node id. On the read side, the
     * consumer must call readDataSourceNodeId() before calling read().
     */
    void write(std::ostream& outputStream, simfil::FieldId offset) const override;

    /**
     * Call this before calling read() to figure out which Fields-
     * object to call read() with.
     */
    static std::string readDataSourceNodeId(std::istream& inputStream);

    std::string const nodeId_;
};

}
