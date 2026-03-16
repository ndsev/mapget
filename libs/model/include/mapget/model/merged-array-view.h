#pragma once

#include "simfil/model/nodes.h"

namespace mapget
{

class TileFeatureLayer;

/**
 * Generic forward-linked merged array view.
 *
 * The local entries can be customized by derived classes via
 * localMerged* methods. By default this wraps the BaseArray storage.
 */
template <class DerivedT, class ItemT>
class MergedArrayView : public simfil::BaseArray<TileFeatureLayer, ItemT>
{
public:
    using Base = simfil::BaseArray<TileFeatureLayer, ItemT>;
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

    void setExtension(ExtensionPtr extension)
    {
        if (!extension) {
            this->model().clearMergedArrayExtension(this->addr());
            return;
        }
        this->model().setMergedArrayExtension(
            this->addr(),
            &extension->model(),
            extension->addr());
    }

    [[nodiscard]] ExtensionPtr extension() const
    {
        auto link = this->model().mergedArrayExtension(this->addr());
        if (!link || !link->first || !link->second) {
            return {};
        }
        return link->first->template resolve<DerivedT>(link->second);
    }

    [[nodiscard]] uint32_t mergedSize() const
    {
        auto size = localMergedSize();
        if (auto ext = extension()) {
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

        auto ext = extension();
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
        if (auto ext = extension()) {
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
