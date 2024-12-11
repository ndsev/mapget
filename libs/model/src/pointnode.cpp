#include "pointnode.h"
#include "featurelayer.h"
#include "mapget/log.h"

using namespace simfil;

namespace mapget
{

/** Model node impls for VertexNode. */

PointNode::PointNode(ModelNode const& baseNode, Geometry::Data const* geomData)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(baseNode)
{
    if (geomData->isView_)
        throw std::runtime_error("Point must be constructed through VertexBuffer which resolves view to geometry.");
    auto i = std::get<int64_t>(data_);
    point_ = geomData->detail_.geom_.offset_;
    if (i > 0)
        point_ += model().vertexBufferStorage().at(geomData->detail_.geom_.vertexArray_, i - 1);
}

PointNode::PointNode(ModelNode const& baseNode, Validity::Data const* geomData)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(baseNode)
{
    auto i = std::get<int64_t>(data_);
    // The extracted point index may point to a validity's single point
    // or to one of its range points. These magic indices are used in validity.cpp.
    switch (i) {
    case 0: point_ = std::get<Point>(geomData->geomDescr_); break;
    case 1: point_ = std::get<Validity::Data::Range>(geomData->geomDescr_).first; break;
    case 2: point_ = std::get<Validity::Data::Range>(geomData->geomDescr_).second; break;
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
    if (!cb(ValueNode(point_.x, model_))) return false;
    if (!cb(ValueNode(point_.y, model_))) return false;
    if (!cb(ValueNode(point_.z, model_))) return false;
    return true;
}

}