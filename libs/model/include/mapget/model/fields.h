#pragma once

#include "simfil/model/fields.h"

namespace mapget
{

/**
 * The Fields class is a case-insensitive dictionary of uint16_t to field name strings.
 * It populates itself when fields are added to an object. Multiple TileFeatureLayers
 * can share the same Fields dictionary, reducing the size of serialized FeatureLayers.
 *
 * The inherited mapget::Fields contains static field IDs for various purposes.
 *
 * Field IDs are uint16 values, which are smaller and faster to work with than strings.
 * When querying fields by name, the Fields dictionary is used to look up a field ID.
 * Subsequent searches are performed using this 16-bit integer for improved efficiency.
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
        NameStr,
        TargetStr,
        SourceValidityStr,
        TargetValidityStr
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
