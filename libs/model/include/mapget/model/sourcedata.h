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
    friend class simfil::shared_model_ptr<SourceDataCompoundNode>;

public:
    SourceDataCompoundNode() = delete;
    SourceDataCompoundNode(const SourceDataCompoundNode&) = delete;
    SourceDataCompoundNode(SourceDataCompoundNode&&) = default;

    /**
     * Source reference data
     */
    void setSourceDataAddress(SourceDataAddress);
    SourceDataAddress sourceDataAddress() const;

    void setSchemaName(std::string_view name);
    std::string_view schemaName() const;

    /**
     * Get this compounds object node
     */
    simfil::shared_model_ptr<simfil::Object> object();
    simfil::shared_model_ptr<const simfil::Object> object() const;

    /**
     * Simfil Model-Node Functions
     */
    simfil::ValueType type() const override;
    ModelNode::Ptr at(int64_t) const override;
    uint32_t size() const override;
    ModelNode::Ptr get(const simfil::StringId&) const override;
    simfil::StringId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

protected:
    SourceDataCompoundNode(Data* data, TileSourceDataLayer::ConstPtr model, simfil::ModelNodeAddress address);
    SourceDataCompoundNode(Data* data, TileSourceDataLayer::Ptr model, simfil::ModelNodeAddress address, size_t initialSize);

private:
    struct Data
    {
        simfil::ModelNodeAddress object_;
        simfil::StringId schemaName_ = {};
        SourceDataAddress sourceAddress_;

        template <typename S>
        void serialize(S& s)
        {
            s.object(object_);
            s.value2b(schemaName_);
            s.object(sourceAddress_);
        }
    };

    Data* const data_;
};

}
