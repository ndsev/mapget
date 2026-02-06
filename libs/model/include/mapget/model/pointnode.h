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
    friend class TileFeatureLayer;
    friend class Geometry;
    friend class PointBufferNode;

    explicit PointNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key) {}

    [[nodiscard]] ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const StringId &) const override;
    [[nodiscard]] StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;  // NOLINT (allow discard)

    PointNode() = delete;

public:
    PointNode(ModelNode const& baseNode, Geometry::Data const* geomData, simfil::detail::mp_key key);
    PointNode(ModelNode const& baseNode, Validity::Data const* geomData, simfil::detail::mp_key key);

private:
    Point point_;
};

template <typename LambdaType, class ModelType>
bool Geometry::forEachPoint(LambdaType const& callback) const {
    auto vertexBufferNode = model_ptr<PointBufferNode>::make(
        geomData_, model_, ModelNodeAddress{ModelType::ColumnId::PointBuffers, addr_.index()});
    for (auto i = 0; i < vertexBufferNode->size(); ++i) {
        auto vertex = model_ptr<PointNode>::make(*vertexBufferNode->at(i), vertexBufferNode->baseGeomData_);
        if (!callback(vertex->point_))
            return false;
    }
    return true;
}

}
