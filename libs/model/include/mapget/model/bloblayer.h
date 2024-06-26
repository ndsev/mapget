#pragma once

#include "layer.h"
#include "simfil/model/model.h"
#include "simfil/environment.h"
#include "simfil/model/nodes.h"

namespace mapget
{

class CompoundBlobNode;

class TileBlobLayer : public TileLayer, public simfil::ModelPool
{
public:
    using Ptr = std::shared_ptr<TileBlobLayer>;

    template <class T>
    using model_ptr = simfil::shared_model_ptr<T>;

    /**
     * ModelPool colunm ids
     */
    enum ColumnId : uint8_t {
        Compound = ModelPool::FirstCustomColumnId,
    };

    TileBlobLayer(
        TileId tileId,
        std::string const& nodeId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::Fields> const& fields);

    TileBlobLayer(
        std::istream&,
        LayerInfoResolveFun const& layerInfoResolveFun,
        FieldNameResolveFun const& fieldNameResolveFun);

    ~TileBlobLayer();

    /**
     * Node factory interface
     */
    model_ptr<CompoundBlobNode> newCompound();

    /**
     * Get this pool's simfil evaluation environment.
     */
    simfil::Environment& evaluationEnvironment();

    /**
     * Get the blob tree root node.
     */
    //model_ptr<BlobNode> root() const;

    /**
     * Return the binary (zserio) blob.
     */
    std::span<const std::byte> sourceData() const;

    /**
     * Serialize the layer.
     */
    void write(std::ostream&) override;

    /**
     * Serialize to json.
     */
    nlohmann::json toJson() const override;

private:
    /**
     * Node resolution functions.
     */
    model_ptr<CompoundBlobNode> resolveCompoundNode(simfil::ModelNode const&) const;

    /**
     * Generic node resolution overload.
     */
    void resolve(const simfil::ModelNode &n, const ResolveFn &cb) const override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
