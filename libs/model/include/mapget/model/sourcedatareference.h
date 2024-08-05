#pragma once

#include <cstdint>
#include <string_view>
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "sourceinfo.h"

using simfil::ValueType;
using simfil::ModelNode;
using simfil::ModelNodeAddress;
using simfil::ModelConstPtr;
using simfil::StringId;
using simfil::ScalarValueType;

namespace mapget
{

class TileFeatureLayer;

struct QualifiedSourceDataReference {
    StringId qualifier_;
    SourceDataReference reference_;

    template <class S>
    void serialize(S& s)
    {
        s.value2b(qualifier_);
        s.object(reference_);
    }
};

/**
 * Proxy node that represents an array of Qualifier-String + SourceDataReference tuples:
 * SourceDataAddressNode.
 */
class SourceDataReferenceCollection final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class TileFeatureLayer;

    ValueType type() const override;
    uint32_t size() const override;
    ModelNode::Ptr at(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;

private:
    SourceDataReferenceCollection() = default;
    SourceDataReferenceCollection(uint32_t offset, uint32_t size, ModelConstPtr pool, ModelNodeAddress a);

    uint32_t offset_ = {};
    uint32_t size_ = {};
};

/**
 * Object holding a tuple of a qualifier string + a source data address.
 */
class SourceDataReferenceItem final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    template<typename> friend struct simfil::shared_model_ptr;
    friend class SourceDataReferenceCollection;
    friend class TileFeatureLayer;

    ValueType type() const override;
    uint32_t size() const override;
    ModelNode::Ptr at(int64_t index) const override;
    ModelNode::Ptr get(const StringId&) const override;
    StringId keyAt(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;

    /**
     * SourceData properties.
     */
    std::string_view qualifier() const;
    std::string_view layerId() const;
    SourceDataAddress address() const;

private:
    SourceDataReferenceItem() = default;
    SourceDataReferenceItem(const QualifiedSourceDataReference* data, ModelConstPtr pool, ModelNodeAddress a);

    const QualifiedSourceDataReference* const data_ = {};
};

}
