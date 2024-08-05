#include "sourcedatalayer.h"

#include <limits>
#include <memory>

#include "bitsery/bitsery.h"
#include "bitsery/adapter/stream.h"
#include "bitsery/adapter/stream.h"
#include "bitsery/deserializer.h"
#include "bitsery/serializer.h"
#include "bitsery/traits/string.h"
#include "bitsery/traits/vector.h"
#include "simfil/model/bitsery-traits.h" // segmented_vector traits

#include "mapget/log.h"
#include "sourcedata.h"
#include "featureid.h"
#include "layer.h"
#include "simfil/model/model.h"
#include "simfilutil.h"

#include "simfil/environment.h"
#include "simfil/model/nodes.h"

using simfil::ModelNodeAddress;

namespace mapget
{

struct TileSourceDataLayer::Impl
{
    // Pool data
    sfl::segmented_vector<SourceDataCompoundNode::Data, simfil::detail::ColumnPageSize / 4> compounds_;

    // Simfil compiled expression and environment
    SimfilExpressionCache expressionCache_;

    Impl(std::shared_ptr<simfil::StringPool> fields)
        : expressionCache_(makeEnvironment(std::move(fields)))
    {}

    // Bitsery (de-)serialization interface
    template<typename S>
    void readWrite(S& s) {
        constexpr size_t maxColumnSize = std::numeric_limits<uint32_t>::max();
        s.container(compounds_, maxColumnSize);
    }
};

TileSourceDataLayer::TileSourceDataLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& fields
) :
    TileLayer(tileId, nodeId, mapId, layerInfo),
    ModelPool(fields),
    impl_(std::make_unique<Impl>(fields))
{}

TileSourceDataLayer::TileSourceDataLayer(
    std::istream& in,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringResolveFun const& fieldNameResolveFun
) :
    TileLayer(in, layerInfoResolveFun),
    ModelPool(fieldNameResolveFun(nodeId_)),
    impl_(std::make_unique<Impl>(fieldNameResolveFun(nodeId_)))
{
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(in);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raiseFmt(
            "Failed to read TileFeatureLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error()));
    }
    ModelPool::read(in);
}

TileSourceDataLayer::~TileSourceDataLayer() = default;

simfil::Environment& TileSourceDataLayer::evaluationEnvironment()
{
    return impl_->expressionCache_.environment();
}

model_ptr<SourceDataCompoundNode> TileSourceDataLayer::newCompound(size_t initialSize)
{
    auto index = impl_->compounds_.size();
    auto& data = impl_->compounds_.emplace_back(SourceDataCompoundNode::Data{});

    return SourceDataCompoundNode(
        &data,
        std::static_pointer_cast<TileSourceDataLayer>(shared_from_this()),
        ModelNodeAddress(Compound, static_cast<uint32_t>(index)),
        initialSize);
}

model_ptr<SourceDataCompoundNode> TileSourceDataLayer::resolveCompound(simfil::ModelNode const& n) const
{
    assert(n.addr().column() == Compound && "Unexpected column type!");

    auto& data = impl_->compounds_.at(n.addr().index());
    return SourceDataCompoundNode(&data, std::static_pointer_cast<const TileSourceDataLayer>(shared_from_this()), n.addr());
}

void TileSourceDataLayer::resolve(const simfil::ModelNode& n, const ResolveFn& cb) const
{
    if (n.addr().column() == Compound)
        return cb(*resolveCompound(n));
    else
        return ModelPool::resolve(n, cb);
}

void TileSourceDataLayer::write(std::ostream& outputStream)
{
    TileLayer::write(outputStream);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
    ModelPool::write(outputStream);
}

nlohmann::json TileSourceDataLayer::toJson() const
{
    return ModelPool::toJson();
}

}
