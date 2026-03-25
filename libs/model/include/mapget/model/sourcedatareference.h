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
class SourceDataReferenceItem;

struct QualifiedSourceDataReference {
    MODEL_COLUMN_TYPE(12);

    SourceDataAddress address_;
    StringId layerId_;
    StringId qualifier_;
};

/**
 * Proxy node that represents an array of Qualifier-String + SourceDataReference tuples.
 */
class SourceDataReferenceCollection final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
    friend class TileFeatureLayer;

    ValueType type() const override;
    uint32_t size() const override;
    ModelNode::Ptr at(int64_t) const override;
    bool iterate(IterCallback const& cb) const override;

    /**
     * Calls the callback `fn` for each SourceDataReferenceItem this
     * collection contains.
     */
    void forEachReference(std::function<void(const SourceDataReferenceItem&)> fn) const;

public:
    explicit SourceDataReferenceCollection(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key) {}
    SourceDataReferenceCollection(uint32_t offset,
                                  uint32_t size,
                                  ModelConstPtr pool,
                                  ModelNodeAddress a,
                                  simfil::detail::mp_key key);
    SourceDataReferenceCollection() = delete;

private:
    uint32_t offset_ = {};
    uint32_t size_ = {};
};

/**
 * Object holding a tuple of a qualifier string + a source data address.
 */
class SourceDataReferenceItem final : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
public:
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

public:
    explicit SourceDataReferenceItem(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key) {}
    SourceDataReferenceItem(const QualifiedSourceDataReference* data,
                            ModelConstPtr pool,
                            ModelNodeAddress a,
                            simfil::detail::mp_key key);
    SourceDataReferenceItem() = delete;

private:
    const QualifiedSourceDataReference* const data_ = {};
};

}
