#pragma once

#include "simfil/model/nodes.h"
#include "featureid-data.h"
#include "featuremodellayer.h"
#include "info.h"
#include <vector>

namespace mapget
{

/**
 * Canonical feature identifier tied to a layer's configured id compositions.
 *
 * The string form is dot-separated and may elide a shared tile prefix when the
 * backing storage uses `useCommonTilePrefix_`.
 */
class FeatureId : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class TileSearchResultLayer;
    friend class Feature;
    friend class Relation;
    friend class bitsery::Access;

public:
    /** Convert the FeatureId to a string like `<type-id>.<part-value-0>...<part-value-n>` */
    [[nodiscard]] std::string toString() const;

    /** Get the feature ID's type id. */
    [[nodiscard]] std::string_view typeId() const;

    /** Get the effective map id referenced by this feature id. */
    [[nodiscard]] std::string mapId() const;

    /**
     * Get the explicitly stored external map id for detached references.
     * Returns nullopt when the reference points into the current tile's map.
     */
    [[nodiscard]] std::optional<std::string_view> externalMapId() const;

    /** Get all id-part key-value-pairs (including the common prefix). */
    [[nodiscard]] KeyValueViewPairs keyValuePairs() const;

    /** Compact serialized representation of a feature id node. */
    using Data = FeatureIdData;

protected:
    /**
     * Internal Node Access APIs
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ScalarValueType value() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::StringId &) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit FeatureId(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}
    FeatureId(Data& data,
              simfil::ModelConstPtr l,
              simfil::ModelNodeAddress a,
              simfil::detail::mp_key key);
    FeatureId(Data const& data,
              simfil::ModelConstPtr l,
              simfil::ModelNodeAddress a,
              simfil::detail::mp_key key);
    FeatureId() = delete;

protected:
    Data data_{};
    model_ptr<Array> values_;
    std::vector<simfil::StringId> partNames_;
    std::vector<uint32_t> visibleValueIndices_;
};

/** Parsed representation of a canonical feature-id string. */
struct ParsedFeatureId
{
    std::string typeId_;
    KeyValuePairs keyValuePairs_;
    uint8_t idCompositionIndex_ = 0;
};

/**
 * Parse a canonical dot-separated feature-id string as emitted by FeatureId::toString().
 * String-valued id parts are percent-unescaped before datatype validation.
 * The primary composition wins when a secondary composition has the same serialized
 * shape; ambiguities within the primary or among secondary compositions are rejected.
 */
bool parseFeatureIdString(
    std::string_view featureId,
    LayerInfo const& layerInfo,
    ParsedFeatureId& result,
    std::string* error = nullptr);

}
