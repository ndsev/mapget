#include "bloblayer.h"

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

#include "blob.h"
#include "featureid.h"
#include "layer.h"
#include "mapget/log.h"
#include "simfilutil.h"

#include "simfil/environment.h"
#include "simfil/model/nodes.h"

using simfil::ModelNodeAddress;

namespace mapget
{

struct TileBlobLayer::Impl
{
    // The binary zserio data.
    std::vector<std::byte> sourceData_;

    // Pool data
    sfl::segmented_vector<CompoundBlobNode::Data, simfil::detail::ColumnPageSize / 4> compounds_;

    // Simfil compiled expression and environment
    SimfilExpressionCache expressionCache_;

    Impl(std::shared_ptr<simfil::Fields> fields)
        : expressionCache_(makeEnvironment(std::move(fields)))
    {}

    // Bitsery (de-)serialization interface
    template<typename S>
    void readWrite(S& s) {
        constexpr size_t maxColumnSize = std::numeric_limits<uint32_t>::max();
        s.container(compounds_, maxColumnSize);
        //s.container(sourceData_, std::numeric_limits<uint32_t>::max()); // FIXME: !!
    }
};

TileBlobLayer::TileBlobLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::Fields> const& fields
) :
    TileLayer(tileId, nodeId, mapId, layerInfo),
    ModelPool(fields),
    impl_(std::make_unique<Impl>(fields))
{}

TileBlobLayer::TileBlobLayer(
    std::istream& in,
    LayerInfoResolveFun const& layerInfoResolveFun,
    FieldNameResolveFun const& fieldNameResolveFun
) :
    TileLayer(in, layerInfoResolveFun),
    ModelPool(fieldNameResolveFun(nodeId_)),
    impl_(std::make_unique<Impl>(fieldNameResolveFun(nodeId_)))
{
    bitsery::Deserializer<bitsery::InputStreamAdapter> s(in);
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raise(fmt::format(
            "Failed to read TileFeatureLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error())));
    }
    ModelPool::read(in);
}

simfil::Environment& TileBlobLayer::evaluationEnvironment()
{
    return impl_->expressionCache_.environment();
}

std::span<const std::byte> TileBlobLayer::sourceData() const
{
    return impl_->sourceData_;
}

model_ptr<CompoundBlobNode> TileBlobLayer::newCompound()
{
    auto& data = impl_->compounds_.emplace_back(CompoundBlobNode::Data{ /* TODO: ... */ });
    auto index = impl_->compounds_.size();

    return CompoundBlobNode(&data, shared_from_this(), ModelNodeAddress(Compound, (int32_t)index));
}

model_ptr<CompoundBlobNode> TileBlobLayer::resolveCompoundNode(simfil::ModelNode const& n) const
{
    assert(n.addr().column() == Compound && "Unexpected column type!");

    auto& data = impl_->compounds_.at(n.addr().index());
    return CompoundBlobNode(&data, shared_from_this(), n.addr());
}

void TileBlobLayer::resolve(const simfil::ModelNode& n, const ResolveFn& cb) const
{
    switch (n.addr().column()) {
    case Compound:
        return cb(*resolveCompoundNode(n));
    }

    return ModelPool::resolve(n, cb);
}

void TileBlobLayer::write(std::ostream& outputStream)
{
    TileLayer::write(outputStream);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
    ModelPool::write(outputStream);
}

}
