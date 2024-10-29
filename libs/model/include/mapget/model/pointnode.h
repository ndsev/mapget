#pragma once

#include "simfil/model/nodes.h"

#include "geometry.h"
#include "validity.h"

namespace mapget
{

/** Vertex Node */

class PointNode final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;
    friend class Geometry;
    friend class PointBufferNode;

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    PointNode() = delete;

private:
    PointNode(ModelNode const& baseNode, Geometry::Data const* geomData);
    PointNode(ModelNode const& baseNode, Validity::Data const* geomData);

    Point point_;
};

template <typename LambdaType, class ModelType>
bool Geometry::forEachPoint(LambdaType const& callback) const {
    PointBufferNode vertexBufferNode{geomData_, model_, {ModelType::ColumnId::PointBuffers, addr_.index()}};
    for (auto i = 0; i < vertexBufferNode.size(); ++i) {
        PointNode vertex{*vertexBufferNode.at(i), vertexBufferNode.baseGeomData_};
        if (!callback(vertex.point_))
            return false;
    }
    return true;
}

}