#pragma once

#include <cstdint>
#include <string_view>

#include "sourceinfo.h"
#include "sourcedatalayer.h"
#include "simfil/model/nodes.h"

namespace mapget
{

/**
 * Node representing a compound (e.g. zserio struct, union or choice) that knows
 * its source address (e.g. zserio binary blob region) it belongs to.
 *
 * All other types like arrays or atomic values are stored as simfil builtin nodes.
 */
class SourceDataCompoundNode : public simfil::MandatoryDerivedModelNodeBase<TileSourceDataLayer>
{
    struct Data;
    friend class TileSourceDataLayer;

public:
    SourceDataCompoundNode() = delete;
    SourceDataCompoundNode(const SourceDataCompoundNode&) = delete;
    SourceDataCompoundNode(SourceDataCompoundNode&&) = default;

    /**
     * Source reference data
     */
    void setSourceDataAddress(SourceDataAddress);
    SourceDataAddress sourceDataAddress() const;

    /**
     * Mark this compound as the origin of an independently addressed payload.
     *
     * Consumers may present this node and its descendants relative to this
     * compound's source address while retaining canonical absolute addresses
     * for lookup and feature source-data references.
     */
    void setSourceDataAddressScope(bool enabled = true);

    /** Return whether this compound starts a presentation address scope. */
    [[nodiscard]] bool isSourceDataAddressScope() const;

    void setSchemaName(std::string_view name);
    std::string_view schemaName() const;

    /**
     * Get this compounds object node
     */
    simfil::model_ptr<simfil::Object> object();
    simfil::model_ptr<const simfil::Object> object() const;

    /**
     * Simfil Model-Node Functions
     */
    simfil::ValueType type() const override;
    ModelNode::Ptr at(int64_t) const override;
    uint32_t size() const override;
    ModelNode::Ptr get(const simfil::StringId&) const override;
    simfil::StringId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit SourceDataCompoundNode(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSourceDataLayer>(key),
          data_(nullptr) {}
    SourceDataCompoundNode(Data* data,
                           TileSourceDataLayer::ConstPtr model,
                           simfil::ModelNodeAddress address,
                           simfil::detail::mp_key key);
    SourceDataCompoundNode(Data* data,
                           TileSourceDataLayer::Ptr model,
                           simfil::ModelNodeAddress address,
                           size_t initialSize,
                           simfil::detail::mp_key key);

private:
    struct Data
    {
        MODEL_COLUMN_TYPE(16);

        simfil::ModelNodeAddress object_;
        simfil::StringId schemaName_ = {};
        SourceDataAddress sourceAddress_;
    };

    Data* const data_;
};

}
