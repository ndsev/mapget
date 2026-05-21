#pragma once

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "featureid.h"
#include "featuremodellayer.h"
#include "geometry.h"
#include "stringpool.h"

namespace mapget
{

class TileSearchResultLayer;

/**
 * Search result root node streamed by TileSearchResultLayer.
 *
 * A result keeps the matched feature id, the copied primary feature geometry,
 * an optional attribute-validity index, and a fixed-size value array aligned to
 * TileSearchResultLayer::resultFields().
 */
class SearchResult : public simfil::MandatoryDerivedModelNodeBase<TileSearchResultLayer>
{
public:
    friend class TileSearchResultLayer;

    static constexpr uint32_t InvalidAttributeIndex = std::numeric_limits<uint32_t>::max();

    /** Compact serialized representation of a search result. */
    struct Data
    {
        MODEL_COLUMN_TYPE(24);

        simfil::ModelNodeAddress featureId_{};
        simfil::ModelNodeAddress geometry_{};
        simfil::ArrayIndex values_ = simfil::InvalidArrayIndex;
        uint32_t attributeIndex_ = InvalidAttributeIndex;
        uint32_t validityIndex_ = InvalidAttributeIndex;
        uint32_t validityCount_ = 0;
    };

    /** Resolve the matched feature id. */
    [[nodiscard]] model_ptr<FeatureId> featureId() const;

    /** Resolve the copied primary geometry. */
    [[nodiscard]] model_ptr<GeometryCollection> geometry() const;

    /** Return the matched attribute-validity index for attribute-scope searches. */
    [[nodiscard]] std::optional<uint32_t> attributeIndex() const;

    /** Return the matched validity index within the matched attribute. */
    [[nodiscard]] std::optional<uint32_t> validityIndex() const;

    /** Return the number of validity contexts considered for the matched attribute. */
    [[nodiscard]] std::optional<uint32_t> validityCount() const;

    /** Return extracted SIMFIL expression values aligned to the layer result fields. */
    [[nodiscard]] model_ptr<Array> values() const;

    /** Export this result to a compact JSON shape used for tests and diagnostics. */
    [[nodiscard]] nlohmann::json toJson() const override;

protected:
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t i) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t i) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit SearchResult(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSearchResultLayer>(key) {}
    SearchResult(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    SearchResult() = delete;

private:
    Data* data_ = nullptr;
};

/**
 * Transient tile layer carrying server-side search-as-map results.
 *
 * The layer shares FeatureId and geometry node implementations with
 * TileFeatureLayer, but its roots are SearchResult objects rather than source
 * features. Search results use the source tile's node id/string-pool namespace;
 * arbitrary computed string values are stored in the ModelPool string buffer.
 */
class TileSearchResultLayer : public TileFeatureModelLayerBase
{
    friend class SearchResult;
    template<typename Target>
    friend model_ptr<Target> resolveInternal(
        simfil::res::tag<Target>,
        TileSearchResultLayer const&,
        simfil::ModelNode const&);

public:
    using TileFeatureModelLayerBase::resolve;
    using Ptr = std::shared_ptr<TileSearchResultLayer>;
    using ColumnId = TileFeatureModelLayerBase::ColumnId;

    /** Create an empty search-result tile layer. */
    TileSearchResultLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& strings);

    /** Parse a search-result tile layer from binary stream bytes. */
    TileSearchResultLayer(
        std::vector<uint8_t> const& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter);

    ~TileSearchResultLayer() override;

    /** Configure the withFields expressions whose outputs fill SearchResult::values(). */
    void setResultFields(std::vector<std::string> fields);

    /** Return the withFields expressions aligned to every result value array. */
    [[nodiscard]] std::vector<std::string> const& resultFields() const;

    /** Optional staged-loading index of the source feature tile this result was derived from. */
    [[nodiscard]] std::optional<uint32_t> stage() const override;
    void setStage(std::optional<uint32_t> stage) override;

    /** Add one result using nodes already owned by this layer. */
    model_ptr<SearchResult> newSearchResult(
        model_ptr<FeatureId> const& featureId,
        model_ptr<GeometryCollection> const& geometry,
        std::span<simfil::ModelNode::Ptr const> values = {},
        std::optional<uint32_t> attributeIndex = std::nullopt,
        std::optional<uint32_t> validityIndex = std::nullopt,
        std::optional<uint32_t> validityCount = std::nullopt);

    model_ptr<FeatureId> newFeatureId(
        std::string_view const& typeId,
        KeyValueViewPairs const& featureIdParts,
        std::optional<std::string_view> externalMapId = std::nullopt) override;

    model_ptr<GeometryCollection> newGeometryCollection(size_t initialCapacity = 2, bool fixedSize = false) override;
    model_ptr<Geometry> newGeometry(GeomType geomType, size_t initialCapacity = 2, bool fixedSize = false) override;
    model_ptr<Geometry> newGeometryView(
        GeomType geomType,
        uint32_t offset,
        uint32_t size,
        model_ptr<Geometry> const& base) override;
    model_ptr<SourceDataReferenceCollection> newSourceDataReferenceCollection(
        std::span<QualifiedSourceDataReference> list) override;

    /** Serialize this result layer to binary. */
    tl::expected<void, simfil::Error> write(std::ostream& outputStream) override;

    /** Export this result layer to JSON for tests and diagnostics. */
    [[nodiscard]] nlohmann::json toJson() const override;

    /** Number of SearchResult roots. */
    [[nodiscard]] size_t size() const;

    /** Resolve a result by root index. */
    [[nodiscard]] model_ptr<SearchResult> at(size_t index) const;

    /** Access layer-wide geometry anchor used for anchor-relative vertex encoding. */
    [[nodiscard]] Point geometryAnchor() const override;
    void setGeometryAnchor(Point const& anchor);

protected:
    tl::expected<void, simfil::Error> resolve(simfil::ModelNode const& n, ResolveFn const& cb) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Primary template for search-result-only resolve hooks.
template<typename Target>
simfil::model_ptr<Target> resolveInternal(
    simfil::res::tag<Target>,
    TileSearchResultLayer const& model,
    simfil::ModelNode const& node);

} // namespace mapget
