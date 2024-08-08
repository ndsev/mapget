#pragma once

#include <string>

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
    using Ptr = std::shared_ptr<TileSourceDataLayer>;
    using ConstPtr = std::shared_ptr<const TileSourceDataLayer>;

    template <class T>
    using model_ptr = simfil::shared_model_ptr<T>;

    /**
     * ModelPool colunm ids
     */
    enum ColumnId : uint8_t {
        Compound = ModelPool::FirstCustomColumnId,
    };

    TileSourceDataLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& stringPool);

    TileSourceDataLayer(
        std::istream&,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter);

    ~TileSourceDataLayer() override;

    /**
     * Node factory interface
     */
    model_ptr<SourceDataCompoundNode> newCompound(size_t initialSize);
    model_ptr<SourceDataCompoundNode> resolveCompound(simfil::ModelNode const&) const;

    /**
     * Get this pool's simfil evaluation environment.
     */
    simfil::Environment& evaluationEnvironment();

    /**
     * Serialize the layer.
     */
    void write(std::ostream&) override;
    nlohmann::json toJson() const override;

    void setStrings(std::shared_ptr<simfil::StringPool> const& newDict) override;

private:
    /**
     * Generic node resolution overload.
     */
    void resolve(const simfil::ModelNode &n, const ResolveFn &cb) const override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
