#include "sourcedatalayer.h"

#include <limits>
#include <memory>

#include "bitsery/bitsery.h"
#include "bitsery/adapter/buffer.h"
#include "bitsery/adapter/stream.h"
#include "bitsery/deserializer.h"
#include "bitsery/serializer.h"
#include "bitsery/traits/string.h"
#include "bitsery/traits/vector.h"
#include "simfil/model/bitsery-traits.h"

#include "mapget/log.h"
#include "sourcedata.h"
#include "featureid.h"
#include "layer.h"
#include "simfil/model/model.h"
#include "simfilutil.h"
#include "simfilexpressioncache.h"

#include "simfil/environment.h"
#include "simfil/model/nodes.h"

using simfil::ModelNodeAddress;

namespace mapget
{

struct TileSourceDataLayer::Impl
{
    SourceDataAddressFormat format_;
    simfil::ModelColumn<SourceDataCompoundNode::Data, simfil::detail::ColumnPageSize / 4> compounds_;
    simfil::ModelColumn<uint8_t, simfil::detail::ColumnPageSize> addressScopeFlags_;

    // Simfil compiled expression and environment
    SimfilExpressionCache expressionCache_;

    Impl(std::shared_ptr<simfil::StringPool> stringPool)
        : expressionCache_(makeEnvironment(std::move(stringPool)))
        , format_(SourceDataAddressFormat::BitRange)
    {}

    // Bitsery (de-)serialization interface
    template<typename S>
    void readWrite(S& s) {
        s.object(compounds_);
        s.value1b(format_);
        s.object(addressScopeFlags_);
    }
};

TileSourceDataLayer::TileSourceDataLayer(
    TileId tileId,
    std::string const& stringPoolId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    std::shared_ptr<simfil::StringPool> const& stringPool) :
    TileLayer(tileId, stringPoolId, mapId, layerInfo),
    ModelPool(stringPool),
    impl_(std::make_unique<Impl>(stringPool))
{}

TileSourceDataLayer::TileSourceDataLayer(
    const std::vector<uint8_t>& input,
    LayerInfoResolveFun const& layerInfoResolveFun,
    StringPoolResolveFun const& stringPoolGetter
) :
    TileLayer(input, layerInfoResolveFun, &deserializationOffsetBytes_),
    ModelPool(stringPoolGetter(stringPoolId_)),
    impl_(std::make_unique<Impl>(stringPoolGetter(stringPoolId_)))
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    if (deserializationOffsetBytes_ > input.size()) {
        raise("Failed to read TileSourceDataLayer: invalid deserialization offset.");
    }
    bitsery::Deserializer<Adapter> s(Adapter(
        input.begin() + static_cast<std::ptrdiff_t>(deserializationOffsetBytes_),
        input.end()));
    impl_->readWrite(s);
    if (s.adapter().error() != bitsery::ReaderError::NoError) {
        raiseFmt(
            "Failed to read TileFeatureLayer: Error {}",
            static_cast<std::underlying_type_t<bitsery::ReaderError>>(s.adapter().error()));
    }
    const auto modelOffset = deserializationOffsetBytes_ + s.adapter().currentReadPos();
    if (auto result = ModelPool::read(input, modelOffset); !result) {
        raise(result.error().message);
    }
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
        initialSize,
        mpKey_);
}

// Short aliases to keep resolve hook signatures compact.
using simfil::ModelNode;
using simfil::res::tag;

template<>
model_ptr<SourceDataCompoundNode> resolveInternal(tag<SourceDataCompoundNode>, TileSourceDataLayer const& model, ModelNode const& node)
{
    assert(node.addr().column() == TileSourceDataLayer::Compound && "Unexpected column type!");

    auto& data = model.impl_->compounds_.at(node.addr().index());
    return SourceDataCompoundNode(
        &data,
        std::static_pointer_cast<const TileSourceDataLayer>(model.shared_from_this()),
        node.addr(),
        model.mpKey_);
}

tl::expected<void, simfil::Error> TileSourceDataLayer::resolve(const simfil::ModelNode& n, const ResolveFn& cb) const
{
    // Merged/container views can surface child nodes from another model. Always
    // let the owning model interpret its own column/index address.
    if (auto owner = n.owningModel(); owner && owner.get() != this) {
        return owner->resolve(n, cb);
    }

    if (n.addr().column() == Compound) {
        cb(*resolve<SourceDataCompoundNode>(n));
        return {};
    }
    return ModelPool::resolve(n, cb);
}

tl::expected<void, simfil::Error> TileSourceDataLayer::write(std::ostream& outputStream)
{
    TileLayer::write(outputStream);
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(outputStream);
    impl_->readWrite(s);
    return ModelPool::write(outputStream);
}

nlohmann::json TileSourceDataLayer::toJson() const
{
    return ModelPool::toJson();
}

MemoryUsageBreakdown TileSourceDataLayer::memoryUsage() const
{
    auto result = TileLayer::memoryUsage();
    result.add("source-data-layer-object", {
        sizeof(TileSourceDataLayer) - sizeof(TileLayer),
        sizeof(TileSourceDataLayer) - sizeof(TileLayer),
    });
    result.add("source-data-layer-impl", {sizeof(Impl), sizeof(Impl)});

    auto const model = ModelPool::memoryUsageStats();
    result.add("model-pool.implementation", model.implementation);
    result.add("model-pool.roots", model.roots);
    result.add("model-pool.int64", model.int64Values);
    result.add("model-pool.double", model.doubleValues);
    result.add("model-pool.string-data", model.stringData);
    result.add("model-pool.string-ranges", model.stringRanges);
    result.add("model-pool.byte-array-ranges", model.byteArrayRanges);
    result.add("model-pool.object-members", model.objectMembers);
    result.add("model-pool.object-schemas", model.objectSchemas);
    result.add("model-pool.array-members", model.arrayMembers);
    result.add("model-pool.array-schemas", model.arraySchemas);
    result.add("source-data.compounds", impl_->compounds_.memory_usage());
    result.add("source-data.address-scope-flags", impl_->addressScopeFlags_.memory_usage());
    result.add("source-data.expression-cache", impl_->expressionCache_.memoryUsage());
    return result;
}

tl::expected<void, simfil::Error>
TileSourceDataLayer::setStrings(std::shared_ptr<simfil::StringPool> const& newDict)
{
    for (auto& compound : impl_->compounds_) {
        if (auto str = strings()->resolve(compound.schemaName_)) {
            if (auto res = newDict->emplace(*str)) {
                compound.schemaName_ = *res;
            } else {
                raise(res.error().message);
            }
        }
    }

    impl_->expressionCache_.reset(makeEnvironment(newDict));

    return ModelPool::setStrings(newDict);
}

void TileSourceDataLayer::setSourceDataAddressFormat(SourceDataAddressFormat f)
{
    impl_->format_ = f;
}

TileSourceDataLayer::SourceDataAddressFormat TileSourceDataLayer::sourceDataAddressFormat() const
{
    return impl_->format_;
}

void TileSourceDataLayer::setSourceDataAddressScope(uint32_t compoundIndex, bool enabled)
{
    while (impl_->addressScopeFlags_.size() <= compoundIndex) {
        impl_->addressScopeFlags_.emplace_back(0);
    }
    impl_->addressScopeFlags_.at(compoundIndex) = enabled ? 1 : 0;
}

bool TileSourceDataLayer::isSourceDataAddressScope(uint32_t compoundIndex) const
{
    return compoundIndex < impl_->addressScopeFlags_.size()
        && impl_->addressScopeFlags_.at(compoundIndex) != 0;
}

}
