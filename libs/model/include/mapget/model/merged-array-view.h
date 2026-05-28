#pragma once

#include "simfil/model/nodes.h"

namespace mapget
{

class TileFeatureLayer;
class TileFeatureModelLayerBase;

/**
 * Generic forward-linked merged array view.
 *
 * The local entries can be customized by derived classes via
 * localMerged* methods. By default this wraps the BaseArray storage.
 */
template <class DerivedT, class ItemT, class ModelT = TileFeatureLayer>
class MergedArrayView : public simfil::BaseArray<ModelT, ItemT>
{
public:
    using Base = simfil::BaseArray<ModelT, ItemT>;
    using ExtensionPtr = simfil::model_ptr<DerivedT>;

    explicit MergedArrayView(simfil::detail::mp_key key)
        : Base(key)
    {
    }

    MergedArrayView(
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key)
        : Base(std::move(pool), address, key)
    {
    }

    [[nodiscard]] uint32_t mergedSize() const
    {
        auto size = localMergedSize();
        if (auto ext = mergedExtension()) {
            size += ext->mergedSize();
        }
        return size;
    }

    [[nodiscard]] simfil::ModelNode::Ptr mergedAt(int64_t i) const
    {
        if (i < 0) {
            return {};
        }

        auto localSize = static_cast<int64_t>(localMergedSize());
        if (i < localSize) {
            return localMergedAt(i);
        }

        auto ext = mergedExtension();
        if (!ext) {
            return {};
        }
        return ext->mergedAt(i - localSize);
    }

    bool mergedIterate(simfil::ModelNode::IterCallback const& cb) const
    {
        if (!localMergedIterate(cb)) {
            return false;
        }
        if (auto ext = mergedExtension()) {
            return ext->mergedIterate(cb);
        }
        return true;
    }

    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t i) const override
    {
        return mergedAt(i);
    }

    [[nodiscard]] uint32_t size() const override
    {
        return mergedSize();
    }

    bool iterate(simfil::ModelNode::IterCallback const& cb) const override
    {
        return mergedIterate(cb);
    }

protected:
    /**
     * Return the overlay portion of this view.
     *
     * Derived feature-scoped views compute this from the layer overlay chain
     * on demand. Keeping the link implicit avoids mutating shared tile state
     * during read-only traversal.
     */
    [[nodiscard]] virtual ExtensionPtr mergedExtension() const
    {
        return {};
    }

    [[nodiscard]] virtual uint32_t localMergedSize() const
    {
        return Base::size();
    }

    [[nodiscard]] virtual simfil::ModelNode::Ptr localMergedAt(int64_t i) const
    {
        return Base::at(i);
    }

    virtual bool localMergedIterate(simfil::ModelNode::IterCallback const& cb) const
    {
        return Base::iterate(cb);
    }
};

}  // namespace mapget
