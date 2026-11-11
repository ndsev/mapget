#pragma once

#include "simfil/model/string-pool.h"
#include <vector>

namespace mapget
{

/**
 * The StringPool class is a case-insensitive dictionary of uint16_t to strings and vice versa.
 * Multiple TileFeatureLayers can share the same pool, reducing the size of serialized
 * FeatureLayers.
 *
 * Inherited simfil keys and mapget's own static keys have fixed protocol IDs.
 *
 * String IDs are uint16 values, which are smaller and faster to work with than strings.
 * When querying strings by name, the pool is used to look up a string ID.
 * Subsequent searches are performed using this 16-bit integer for improved efficiency.
 *
 * A streamed pool is identified by the producer's stringPoolId. Retain that
 * namespace with serialized tiles; the catalog sourceId is a separate identity.
 */
struct StringPool : public simfil::StringPool
{
    enum StaticStringIds : simfil::StringId {
        IdStr = NextStaticId,
        TypeIdStr,
        MapIdStr,
        LayerIdStr,
        LayerStr,
        RelationsStr,
        DirectionStr,
        ValidityStr,
        PropertiesStr,
        TargetStr,
        SourceValidityStr,
        TargetValidityStr,
        LonStr,
        LatStr,
        GeometryStr,
        GeometryNameStr,
        GeometriesStr,
        OffsetTypeStr,
        TypeStr,
        CoordinatesStr,
        ElevationStr,
        SourceDataStr,
        AddressStr,
        QualifierStr,
        StartStr,
        EndStr,
        PointStr,
        FeatureIdStr,
        AabbStr,
        OriginStr,
        SizeStr,
        GltfNodeIndexStr,
        FromStr,
        ToStr,
        ConnectedEndStr,
        FromConnectedEndStr,
        ToConnectedEndStr,
        TransitionNumberStr,
        AttributeIndexStr,
        ValuesStr,
        MatchStr,
        ValidityIndexStr,
        ValidityCountStr,
        OverlayNameStr,
        OverlayFeatureStr,
        OverlayFeaturesStr,
        OverlaySourceStr,
        OverlayTargetStr,
        OverlayTwowayStr,
        OverlayLayerStr,
        OverlayValidityIndexStr,
        OverlayValidityCountStr,
        OverlayHasValidityStr,
        CallsStr,
        TotalUsStr,
        MapgetRelationStr,
        AttributesStr,
        ChannelIdStr,
        ScopeStr,
        GeometryTypesStr,
        FeatureFieldsStr,
        EntryFieldsStr,
        FeatureEntriesStr,
        AttributeValidityEntriesStr,
        RelationEntriesStr,
        GroupEntriesStr,
        HostValuesStr,
        AttributeLayerStr,
        AttributeNameStr,
        HasValidityStr,
        RelationIdStr,
        ProvenanceStr,
        TwowayStr,
        SourceStr,
        SourceGeometryStr,
        TargetGeometryStr,
        GroupKeyStr,
        RepresentativeFeatureIdStr,
        MemberFeatureIdsStr,
        FilterIdStr,
        GenerationStr,
        SourceFeatureCountStr,
        OverlayAttributeIndexStr,
        OverlayRelationIndexStr,
        AttrPointSequencesStr,
        AttrPointsStr,
        AttrPointIndexStr,
        AttrPointIndexRangeStr,
        MapgetAttrPointSequenceStr,
        SequenceStr,
        IndexStr,
        GeometryIndexStr,
        PositionCountStr,
        SchemaFeatureStr,
        SchemaAttributeStr,
        ValueErrorsStr,
        HostValueErrorsStr,
        ExpressionIndexStr,
        StageStr,
        MessageStr
    };

    // Shared vocabulary must have one ID: name lookup and procedural fields use the same key.
    static constexpr simfil::StringId NameStr = simfil::StringPool::SchemaName;
    static constexpr simfil::StringId SchemaBitmaskStr = simfil::StringPool::SchemaBitmask;

    static_assert(
        int(MessageStr) < int(FirstDynamicId),
        "Mapget static keys must not overlap dynamic strings.");

    explicit StringPool(const std::string_view& stringPoolId);

    /**
     * Write is overloaded, because it prepends the stream with
     * this dictionary's stringPoolId. On the read side, the
     * consumer must call readDataSourceStringPoolId() before calling read().
     */
    tl::expected<void, simfil::Error>
    write(std::ostream& outputStream, simfil::StringId offset) const override;

    /**
     * Parse the stringPoolId prefix at offset. bytesRead reports prefix bytes
     * consumed relative to that offset; malformed/truncated input throws.
     */
    static std::string readDataSourceStringPoolId(
        const std::vector<uint8_t>& input,
        size_t offset = 0,
        size_t* bytesRead = nullptr);

    std::string const stringPoolId_;
};

}
