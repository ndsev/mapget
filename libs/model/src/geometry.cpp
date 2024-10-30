#include "geometry.h"
#include "featurelayer.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "sourcedatareference.h"
#include "sourceinfo.h"
#include "stringpool.h"
#include "validity.h"
#include "pointnode.h"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <variant>

static const std::string_view GeometryCollectionStr("GeometryCollection");
static const std::string_view MultiPointStr("MultiPoint");
static const std::string_view LineStringStr("LineString");
static const std::string_view PolygonStr("Polygon");
static const std::string_view MultiPolygonStr("MultiPolygon");

namespace mapget
{

using namespace simfil;

/** Model node impls. for GeometryCollection */

GeometryCollection::GeometryCollection(ModelConstPtr pool_, ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(pool_), a)
{}

ValueType GeometryCollection::type() const {
    return ValueType::Object;
}

ModelNode::Ptr GeometryCollection::at(int64_t i) const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->at(i);
    if (i == 0) return ValueNode(GeometryCollectionStr, model_);
    if (i == 1) return ModelNode::Ptr::make(model_, ModelNodeAddress{simfil::ModelPool::Arrays, addr_.index()});
    throw std::out_of_range("geom collection: Out of range.");
}

uint32_t GeometryCollection::size() const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->size();
    return 2;
}

ModelNode::Ptr GeometryCollection::get(const StringId& f) const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->get(f);
    if (f == StringPool::TypeStr) return at(0);
    if (f == StringPool::GeometriesStr) return at(1);
    return {};
}

StringId GeometryCollection::keyAt(int64_t i) const {
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->keyAt(i);
    if (i == 0) return StringPool::TypeStr;
    if (i == 1) return StringPool::GeometriesStr;
    if (i == 1) return StringPool::SourceDataStr;
    throw std::out_of_range("geom collection: Out of range.");
}

model_ptr<Geometry> GeometryCollection::newGeometry(GeomType type, size_t initialCapacity) {
    auto result = model().newGeometry(type, initialCapacity);
    auto arrayPtr = ModelNode::Ptr::make(model_, ModelNodeAddress{simfil::ModelPool::Arrays, addr_.index()});
    model().resolveArray(arrayPtr)->append(result);
    return result;
}

bool GeometryCollection::iterate(const IterCallback& cb) const
{
    if (auto singleGeomEntry = singleGeom())
        return singleGeomEntry->iterate(cb);
    if (!cb(*at(0))) return false;
    if (!cb(*at(1))) return false;
    return true;
}

ModelNode::Ptr GeometryCollection::singleGeom() const
{
    if (model().arrayMemberStorage().size((ArrayIndex)addr_.index()) == 1) {
        auto arrayPtr = ModelNode::Ptr::make(model_, ModelNodeAddress{simfil::ModelPool::Arrays, addr_.index()});
        return model().resolveArray(arrayPtr)->at(0);
    }
    return {};
}

void GeometryCollection::addGeometry(const model_ptr<Geometry>& geom)
{
    auto arrayPtr = ModelNode::Ptr::make(model_, ModelNodeAddress{simfil::ModelPool::Arrays, addr_.index()});
    model().resolveArray(arrayPtr)->append(ModelNode::Ptr(geom));
}

size_t GeometryCollection::numGeometries() const
{
    return model().arrayMemberStorage().size((ArrayIndex)addr().index());
}

/** ModelNode impls. for Geometry */

Geometry::Geometry(Data* data, ModelConstPtr pool_, ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(pool_), a), geomData_(data)
{
    storage_ = &model().vertexBufferStorage();
}

ValueType Geometry::type() const {
    return ValueType::Object;
}

ModelNode::Ptr Geometry::at(int64_t i) const {
    if (geomData_->sourceDataReferences_) {
        if (i == 0)
            return get(StringPool::SourceDataStr);
        i -= 1;
    }
    if (i == 0)
        return get(StringPool::TypeStr);
    if (i == 1)
        return get(StringPool::CoordinatesStr);
    throw std::out_of_range("geom: Out of range.");
}

uint32_t Geometry::size() const {
    return 2 + (geomData_->sourceDataReferences_ ? 1 : 0);
}

ModelNode::Ptr Geometry::get(const StringId& f) const {
    if (f == StringPool::SourceDataStr && geomData_->sourceDataReferences_) {
        return ModelNode::Ptr::make(model_, geomData_->sourceDataReferences_);
    }
    if (f == StringPool::TypeStr) {
        return ValueNode(
            geomData_->type_ == GeomType::Points  ? MultiPointStr :
            geomData_->type_ == GeomType::Line    ? LineStringStr :
            geomData_->type_ == GeomType::Polygon ? PolygonStr :
            geomData_->type_ == GeomType::Mesh    ? MultiPolygonStr : "",
            model_);
    }
    if (f == StringPool::CoordinatesStr) {
        switch (geomData_->type_) {
        case GeomType::Polygon:
            return ModelNode::Ptr::make(
                model_, ModelNodeAddress{TileFeatureLayer::ColumnId::Polygon, addr_.index()});
        case GeomType::Mesh:
            return ModelNode::Ptr::make(
                model_, ModelNodeAddress{TileFeatureLayer::ColumnId::Mesh, addr_.index()});
        default:
            return ModelNode::Ptr::make(
                model_, ModelNodeAddress{TileFeatureLayer::ColumnId::PointBuffers, addr_.index()});
        }
    }
    return {};
}

StringId Geometry::keyAt(int64_t i) const {
    if (geomData_->sourceDataReferences_) {
        if (i == 0)
            return StringPool::SourceDataStr;
        i -= 1;
    }
    if (i == 0) return StringPool::TypeStr;
    if (i == 1) return StringPool::CoordinatesStr;
    throw std::out_of_range("geom: Out of range.");
}

model_ptr<SourceDataReferenceCollection> Geometry::sourceDataReferences() const
{
    if (geomData_->sourceDataReferences_)
        return model().resolveSourceDataReferenceCollection(*model_ptr<simfil::ModelNode>::make(model_, geomData_->sourceDataReferences_));
    return {};
}

void Geometry::setSourceDataReferences(simfil::ModelNode::Ptr const& refs)
{
    geomData_->sourceDataReferences_ = refs->addr();
}

void Geometry::append(Point const& p)
{
    if (geomData_->isView_)
        throw std::runtime_error("Cannot append to geometry view.");

    auto& geomData = geomData_->detail_.geom_;

    // Before the geometry is assigned with a vertex array,
    // a negative array handle denotes the desired initial
    // capacity, +1, because there is always the additional
    // offset point.
    if (geomData.vertexArray_ < 0) {
        auto initialCapacity = abs(geomData_->detail_.geom_.vertexArray_);
        geomData.vertexArray_ = storage_->new_array(initialCapacity-1);
        geomData.offset_ = p;
        return;
    }
    storage_->emplace_back(
        geomData.vertexArray_,
        glm::fvec3{
            static_cast<float>(p.x - geomData.offset_.x),
            static_cast<float>(p.y - geomData.offset_.y),
            static_cast<float>(p.z - geomData.offset_.z)});
}

GeomType Geometry::geomType() const {
    return geomData_->type_;
}

bool Geometry::iterate(const IterCallback& cb) const
{
    if (!cb(*at(0))) return false;
    if (!cb(*at(1))) return false;
    return true;
}

size_t Geometry::numPoints() const
{
    PointBufferNode vertexBufferNode{geomData_, model_, {TileFeatureLayer::ColumnId::PointBuffers, addr_.index()}};
    return vertexBufferNode.size();
}

Point Geometry::pointAt(size_t index) const
{
    PointBufferNode vertexBufferNode{geomData_, model_, {TileFeatureLayer::ColumnId::PointBuffers, addr_.index()}};
    PointNode vertex{*vertexBufferNode.at((int64_t)index), vertexBufferNode.baseGeomData_};
    return vertex.point_;
}

std::optional<std::string_view> Geometry::name()
{
    if (geomData_->geomName_ == StringPool::Empty) {
        return {};
    }
    return model().strings()->resolve(geomData_->geomName_);
}

void Geometry::setName(const std::string_view& newName)
{
    geomData_->geomName_ = model().strings()->emplace(newName);
}

/** ModelNode impls. for PolygonNode */

PolygonNode::PolygonNode(ModelConstPtr pool, ModelNodeAddress const& a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(pool), a)
{}

ValueType PolygonNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr PolygonNode::at(int64_t index) const
{
    // Index 0 is the outer ring, all following rings are holes
    if (index == 0)
        return ModelNode::Ptr::make(
            model_, ModelNodeAddress{TileFeatureLayer::ColumnId::LinearRing, addr_.index()});

    throw std::out_of_range("PolygonNode: index out of bounds.");
}

uint32_t PolygonNode::size() const
{
    return 1;
}

ModelNode::Ptr PolygonNode::get(const StringId&) const
{
    return {};
}

StringId PolygonNode::keyAt(int64_t) const
{
    return {};
}

bool PolygonNode::iterate(IterCallback const& cb) const
{
    if (!cb(*at(0))) return false;
    return true;
}

/** ModelNode impls. for MeshNode */

MeshNode::MeshNode(Geometry::Data const* geomData, ModelConstPtr pool, ModelNodeAddress const& a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(pool), a), geomData_(geomData)
{
    auto vertex_buffer = PointBufferNode{
        geomData_, model_, {TileFeatureLayer::ColumnId::PointBuffers, addr_.index()}};
    assert(vertex_buffer.size() % 3 == 0);
    size_ = vertex_buffer.size() / 3;
}

ValueType MeshNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr MeshNode::at(int64_t index) const
{
    if (0 <= index && index < size_)
        return ModelNode::Ptr::make(
            model_, ModelNodeAddress{TileFeatureLayer::ColumnId::MeshTriangleCollection, addr_.index()}, index);

    throw std::out_of_range("MeshNode: index out of bounds.");
}

uint32_t MeshNode::size() const
{
    return size_;
}

bool MeshNode::iterate(IterCallback const& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

MeshTriangleCollectionNode::MeshTriangleCollectionNode(const ModelNode& base)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(base)
    , index_(std::get<int64_t>(data_) * 3)
{}

ValueType MeshTriangleCollectionNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr MeshTriangleCollectionNode::at(int64_t index) const
{
    if (index == 0)
        return ModelNode::Ptr::make(model_, ModelNodeAddress{TileFeatureLayer::ColumnId::MeshTriangleLinearRing, addr_.index()}, index_);

    throw std::out_of_range("MeshTriangleCollectionNode: index out of bounds.");
}

uint32_t MeshTriangleCollectionNode::size() const
{
    return 1;
}

bool MeshTriangleCollectionNode::iterate(IterCallback const& cb) const
{
    if (!cb(*at(0)))
        return false;
    return true;
}

/** ModelNode impls. for LinearRingNode (a closed, simple polygon in CCW order) */

LinearRingNode::LinearRingNode(const ModelNode& base, std::optional<size_t> length)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(base)
{
    if (std::get_if<int64_t>(&data_))
        offset_ = std::get<int64_t>(data_);

    auto buffer = vertexBuffer();
    size_ = length.value_or(buffer->size() - offset_);

    auto isClosed = [&]()
    {
        const auto n = size_;
        if (n < 3)
            return false;

        const auto& first = buffer->pointAt(0 + offset_);
        const auto& last = buffer->pointAt(n - 1 + offset_);

        return first == last;
    };

    closed_ = isClosed();

    // The signed area of a simple polygon can be calculated
    // using the shoelace formula:
    //
    //        n-1
    //   2 A = Σ  x_i y_(i+1) - y_i x_(i+1)
    //        i=0
    //
    // If the area is negative, the polygon is in clock-wise orientation.
    // We assume the polygon in in the x-y plane, returning 0 for polygons
    // on a different plane.
    auto signedArea = [&]()
    {
        if (size_ <= 0)
            return 0.0;

        auto area = 0.0;
        auto z = buffer->pointAt(0 + offset_).z;
        const auto n = size_;
        for (auto i = 0; i < n; ++i) {
            const auto& a = buffer->pointAt(i + offset_);
            const auto& b = buffer->pointAt((i + 1 + offset_) % n);
            if (a.z != z)
                return 0.0;

            area += a.x * b.y - a.y * b.x;
        }
        return area / 2;
    };

    orientation_ = signedArea() < 0 ? Orientation::CW : Orientation::CCW;
}

ValueType LinearRingNode::type() const
{
    return ValueType::Array;
}

ModelNode::Ptr LinearRingNode::at(int64_t index) const
{
    auto buffer = vertexBuffer();
    if (0 > index || index >= size())
        throw std::out_of_range("LinearRingNode: index out of range.");

    // GeoJSON rings must be closed. If the ring is not closed,
    // we return the first point as last point again.
    if (!closed_ && index == size() - 1)
        return buffer->at(0 + offset_);

    // If the ring is in clockwise order, reverse the index to
    // conform to GeoJSON expecting CCW polygons (for outer rings) only.
    if (orientation_ == Orientation::CW)
        index = (size_ - index) % size_;

    return buffer->at(index + offset_);
}

ModelNode::Ptr LinearRingNode::get(const StringId&) const
{
    return {};
}

StringId LinearRingNode::keyAt(int64_t) const
{
    return {};
}

bool LinearRingNode::iterate(const IterCallback& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;
    return true;
}

uint32_t LinearRingNode::size() const
{
    return size_ + (closed_ ? 0 : 1);
}

model_ptr<PointBufferNode> LinearRingNode::vertexBuffer() const
{
    auto ptr = ModelNode::Ptr::make(
        model_, ModelNodeAddress{TileFeatureLayer::ColumnId::PointBuffers, addr_.index()}, 0);
    return model().resolvePointBuffer(*ptr);
}

/** ModelNode impls. for VertexBufferNode */

PointBufferNode::PointBufferNode(Geometry::Data const* geomData, ModelConstPtr pool_, ModelNodeAddress const& a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(pool_), a), baseGeomData_(geomData), baseGeomAddress_(a)
{
    storage_ = &model().vertexBufferStorage();

    // Resolve geometry view to actual geometry, process
    // actual offset and length.
    if (baseGeomData_->isView_) {
        size_ = baseGeomData_->detail_.view_.size_;

        while (baseGeomData_->isView_) {
            offset_ += baseGeomData_->detail_.view_.offset_;
            baseGeomAddress_ = baseGeomData_->detail_.view_.baseGeometry_;
            baseGeomData_ = model().resolveGeometry(
                *ModelNode::Ptr::make(model_, baseGeomData_->detail_.view_.baseGeometry_))->geomData_;
        }

        auto maxSize = 1 + storage_->size(baseGeomData_->detail_.geom_.vertexArray_);
        if (offset_ + size_ > maxSize)
            throw std::runtime_error("Geometry view is out of bounds.");
    }
    else {
        // Just get the correct length.
        if (baseGeomData_->detail_.geom_.vertexArray_ >= 0)
            size_ = 1 + storage_->size(baseGeomData_->detail_.geom_.vertexArray_);
    }
}

ValueType PointBufferNode::type() const {
    return ValueType::Array;
}

ModelNode::Ptr PointBufferNode::at(int64_t i) const {
    if (i < 0 || i >= size())
        throw std::out_of_range("vertex-buffer: Out of range.");
    i += offset_;
    return ModelNode::Ptr::make(model_, ModelNodeAddress{TileFeatureLayer::ColumnId::Points, baseGeomAddress_.index()}, i);
}

uint32_t PointBufferNode::size() const {
    return size_;
}

ModelNode::Ptr PointBufferNode::get(const StringId &) const {
    return {};
}

StringId PointBufferNode::keyAt(int64_t) const {
    return {};
}

bool PointBufferNode::iterate(const IterCallback& cb) const
{
    auto cont = true;
    auto resolveAndCb = Model::Lambda([&cb, &cont](auto && node){
        cont = cb(node);
    });
    for (auto i = 0u; i < size_; ++i) {
        resolveAndCb(*ModelNode::Ptr::make(
            model_, ModelNodeAddress{TileFeatureLayer::ColumnId::Points, baseGeomAddress_.index()}, (int64_t)i+offset_));
        if (!cont)
            break;
    }
    return cont;
}

Point PointBufferNode::pointAt(int64_t index) const
{
    PointNode vertex{*at(index), baseGeomData_};
    return vertex.point_;
}

}
