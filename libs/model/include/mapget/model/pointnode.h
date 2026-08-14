#pragma once

#include "simfil/model/nodes.h"

#include "geometry.h"
#include "validity.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace mapget
{

/** Vertex Node */

class PointNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>
{
public:
    friend class TileFeatureModelLayerBase;
    friend class TileFeatureLayer;
    friend class Geometry;
    friend class PointBufferNode;

    explicit PointNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureModelLayerBase>(key) {}

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    PointNode() = delete;

public:
    PointNode(
        ModelNode const& baseNode,
        simfil::ArrayIndex vertexArray,
        simfil::detail::mp_key key);
    PointNode(ModelNode const& baseNode, Validity::Data const* geomData, simfil::detail::mp_key key);
    PointNode(ModelNode const& baseNode, simfil::detail::mp_key key);

private:
    Point point_;
};

template <typename LambdaType, class ModelType>
bool Geometry::forEachPoint(LambdaType const& callback) const {
    if (geomType() == GeomType::GltfNodeIndex) {
        return true;
    }
    auto baseAddress = addr_;
    size_t begin = 0U;
    auto end = std::numeric_limits<size_t>::max();
    if (geomViewData_) {
        auto const* view = geomViewData_;
        begin = view->offset_;
        end = begin + view->size_;
        baseAddress = view->baseGeometry_;
        while (baseAddress.column() ==
               TileFeatureModelLayerBase::ColumnId::GeometryViews)
        {
            view = model().geometryViewData(baseAddress);
            if (!view) {
                throw std::runtime_error(
                    "Failed to resolve nested geometry view.");
            }
            begin += view->offset_;
            end = begin + geomViewData_->size_;
            baseAddress = view->baseGeometry_;
        }
        auto const column = baseAddress.column();
        using Columns = TileFeatureModelLayerBase::ColumnId;
        if (column != Columns::PointGeometries &&
            column != Columns::LineGeometries &&
            column != Columns::PolygonGeometries &&
            column != Columns::MeshGeometries &&
            column != Columns::AabbGeometries &&
            column != Columns::GltfNodeIndexGeometries)
        {
            throw std::runtime_error(
                "Geometry view must resolve to a base geometry.");
        }
        if (end > storage_->size(
                static_cast<simfil::ArrayIndex>(baseAddress.index())))
        {
            throw std::runtime_error("Geometry view is out of bounds.");
        }
    }
    auto const anchor = model().geometryAnchor();
    bool completed = true;
    // Keep the callback unary so ArrayArena propagates its boolean result.
    size_t index = 0U;
    storage_->iterate(
        static_cast<simfil::ArrayIndex>(baseAddress.index()),
        [&](glm::vec3 const& storedPoint) {
            auto const currentIndex = index++;
            if (currentIndex < begin) {
                return true;
            }
            if (currentIndex >= end) {
                return false;
            }
            Point point{storedPoint};
            if (baseAddress.column() !=
                    TileFeatureModelLayerBase::ColumnId::AabbGeometries ||
                currentIndex != 1U)
            {
                point.x += anchor.x;
                point.y += anchor.y;
                point.z += anchor.z;
            }
            if (!callback(std::move(point))) {
                completed = false;
                return false;
            }
            return true;
        });
    return completed;
}

}
