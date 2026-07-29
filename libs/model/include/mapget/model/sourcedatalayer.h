#pragma once

#include <string>
#include <vector>

#include "simfil/model/model.h"
#include "simfil/environment.h"
#include "simfil/model/nodes.h"

#include "layer.h"

namespace mapget
{

class SourceDataCompoundNode;

class TileSourceDataLayer : public TileLayer, public simfil::ModelPool
{
public:
    // Keep ModelPool::resolve<T> overloads visible alongside the override below.
    using ModelPool::resolve;

    using Ptr = std::shared_ptr<TileSourceDataLayer>;
    using ConstPtr = std::shared_ptr<const TileSourceDataLayer>;

    template <class T>
    using model_ptr = simfil::model_ptr<T>;

    template<typename Target>
    friend model_ptr<Target> resolveInternal(
        simfil::res::tag<Target>,
        TileSourceDataLayer const&,
        simfil::ModelNode const&);

    /**
     * ModelPool colunm ids
     */
    enum ColumnId : uint8_t {
        Compound = ModelPool::FirstCustomColumnId,
    };

    TileSourceDataLayer(
        TileId tileId,
        std::string const& stringPoolId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& stringPool);

    TileSourceDataLayer(
        const std::vector<uint8_t>& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter);

    ~TileSourceDataLayer() override;

    /**
     * Node factory interface
     */
    model_ptr<SourceDataCompoundNode> newCompound(size_t initialSize);

    /**
     * Get this pool's simfil evaluation environment.
     */
    simfil::Environment& evaluationEnvironment();

    /**
     * Serialize the layer.
     */
    tl::expected<void, simfil::Error> write(std::ostream&) override;
    nlohmann::json toJson() const override;

    tl::expected<void, simfil::Error>
    setStrings(std::shared_ptr<simfil::StringPool> const& newDict) override;

    enum class SourceDataAddressFormat : uint8_t
    {
        /** Addresses are treated as opaque integers. */
        Unknown,
        /** Addresses represent a 32 bit offset and 32 bit length in bits. */
        BitRange,
    };

    /**
     * Accessors for the source-data address format of all
     * source-data reference addresses this layer exposes.
     */
    void setSourceDataAddressFormat(SourceDataAddressFormat f);
    SourceDataAddressFormat sourceDataAddressFormat() const;

private:
    friend class SourceDataCompoundNode;

    /** Set the sparse address-scope flag associated with one compound. */
    void setSourceDataAddressScope(uint32_t compoundIndex, bool enabled);

    /** Read one compound's address-scope flag without growing storage. */
    [[nodiscard]] bool isSourceDataAddressScope(uint32_t compoundIndex) const;

    /**
     * Generic node resolution overload.
     */
    tl::expected<void, simfil::Error> resolve(const simfil::ModelNode &n, const ResolveFn &cb) const override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Primary template for ADL-based resolve hooks (specialized in sourcedatalayer.cpp).
template<typename Target>
simfil::model_ptr<Target> resolveInternal(
    simfil::res::tag<Target>,
    TileSourceDataLayer const& model,
    simfil::ModelNode const& node);

}
