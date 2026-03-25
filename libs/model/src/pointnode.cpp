#include "pointnode.h"
#include "featurelayer.h"
#include "mapget/log.h"

using namespace simfil;

namespace mapget
{

/** Model node impls for VertexNode. */

PointNode::PointNode(
    ModelNode const& baseNode,
    simfil::ArrayIndex vertexArray,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(baseNode, key)
{
    auto i = std::get<int64_t>(data_);
    point_ = model().geometryAnchor();
    auto vertexResult = model().vertexBufferStorage().at(
        vertexArray,
        static_cast<size_t>(i));
    if (!vertexResult) {
        raise("Failed to get vertex from buffer");
    }
    point_ += vertexResult->get();
}

PointNode::PointNode(ModelNode const& baseNode,
    Validity::Data const* geomData,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(baseNode, key)
{
    auto i = std::get<int64_t>(data_);
    // The extracted point index may point to a validity's single point
    // or to one of its range points. These magic indices are used in validity.cpp.
    switch (i) {
    case 0: point_ = geomData->geomDescr_.point_; break;
    case 1: point_ = geomData->geomDescr_.range_.first; break;
    case 2: point_ = geomData->geomDescr_.range_.second; break;
    default:
        mapget::raiseFmt<std::runtime_error>("Invalid validity point index {}", i);
    }
}

ValueType PointNode::type() const {
    return ValueType::Array;
}

ModelNode::Ptr PointNode::at(int64_t i) const {
    if (i == 0) return model_ptr<ValueNode>::make(point_.x, model_);
    if (i == 1) return model_ptr<ValueNode>::make(point_.y, model_);
    if (i == 2) return model_ptr<ValueNode>::make(point_.z, model_);
    throw std::out_of_range("vertex: Out of range.");
}

uint32_t PointNode::size() const {
    return 3;
}

ModelNode::Ptr PointNode::get(const StringId & field) const {
    if (field == StringPool::LonStr) return at(0);
    if (field == StringPool::LatStr) return at(1);
    if (field == StringPool::ElevationStr) return at(2);
    else return {};
}

StringId PointNode::keyAt(int64_t i) const {
    if (i == 0) return StringPool::LonStr;
    if (i == 1) return StringPool::LatStr;
    if (i == 2) return StringPool::ElevationStr;
    throw std::out_of_range("vertex: Out of range.");
}

bool PointNode::iterate(const IterCallback& cb) const
{
    if (!cb(*model_ptr<ValueNode>::make(point_.x, model_))) return false;
    if (!cb(*model_ptr<ValueNode>::make(point_.y, model_))) return false;
    if (!cb(*model_ptr<ValueNode>::make(point_.z, model_))) return false;
    return true;
}

}
