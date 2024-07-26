#pragma once

#include "simfil/model/string-pool.h"

namespace mapget
{

/**
 * The StringPool class is a case-insensitive dictionary of uint16_t to field name strings.
 * Multiple TileFeatureLayers can share the same pool, reducing the size of serialized FeatureLayers.
 *
 * The inherited mapget::StringPool contains static field IDs for various purposes.
 *
 * String IDs are uint16 values, which are smaller and faster to work with than strings.
 * When querying strings by name, the pool is used to look up a field ID.
 * Subsequent searches are performed using this 16-bit integer for improved efficiency.
 *
 * Note: A StringPool is always unique per datasource node. Therefore,
 * the object must be constructed with a datasource node id.
 */
struct StringPool : public simfil::StringPool
{
    enum StaticFieldIds : simfil::StringId {
        IdStr = NextStaticId,
        TypeIdStr,
        MapIdStr,
        LayerIdStr,
        LayerStr,
        RelationsStr,
        DirectionStr,
        ValidityStr,
        PropertiesStr,
        NameStr,
        TargetStr,
        SourceValidityStr,
        TargetValidityStr,
        LonStr,
        LatStr,
        GeometryStr,
        GeometriesStr,
        TypeStr,
        CoordinatesStr,
        ElevationStr,
        SourceDataAddressStr,
        SourceDataTypeStr,
    };

    explicit StringPool(const std::string_view& nodeId);

    /**
     * Write is overloaded, because it prepends the stream with
     * this dictionary's data source node id. On the read side, the
     * consumer must call readDataSourceNodeId() before calling read().
     */
    void write(std::ostream& outputStream, simfil::StringId offset) const override;

    /**
     * Call this before calling read() to figure out which strings-
     * object to call read() with.
     */
    static std::string readDataSourceNodeId(std::istream& inputStream);

    std::string const nodeId_;
};

}
