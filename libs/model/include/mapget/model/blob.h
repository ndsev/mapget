#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>

#include "simfil/model/nodes.h"

namespace mapget
{

class TileBlobLayer;

struct SourceRegion
{
    uint32_t offset;
    uint32_t length;

    SourceRegion()
        : offset(0u)
        , length(0u)
    {}

    SourceRegion(std::tuple<size_t, size_t> bits)
        : offset(std::get<0>(bits))
        , length(std::get<1>(bits))
    {}

    /**
     * Get the offset and length in bits
     */
    std::tuple<size_t, size_t> asBits() const
    {
        return std::make_tuple(offset, length);
    }

    template <typename S>
    void serialize(S& s)
    {
        s.value4b(offset);
        s.value4b(length);
    }
};

/**
 * Node representing a zserio compound (struct, union or choice) that knows
 * its source region of the zserio binary blob it was parsed from.
 *
 * All other types like arrays or atomic values are stored as simfil builtin nodes.
 */
class CompoundBlobNode : public simfil::MandatoryDerivedModelNodeBase<TileBlobLayer>
{
    struct Data;
    friend class TileBlobLayer;
    friend class simfil::shared_model_ptr<CompoundBlobNode>;

public:
    CompoundBlobNode() = delete;
    CompoundBlobNode(const CompoundBlobNode&) = delete;
    CompoundBlobNode(CompoundBlobNode&&) = default;

    /**
     * Source blob region
     */
    void setSourceRegion(std::tuple<size_t, size_t> region);
    std::tuple<size_t, size_t> sourceRegion() const;

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
    ModelNode::Ptr get(const simfil::FieldId&) const override;
    simfil::FieldId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

protected:
    CompoundBlobNode(Data*, simfil::ModelConstPtr, simfil::ModelNodeAddress);

private:
    struct Data
    {
        simfil::ModelNodeAddress object_;
        SourceRegion sourceRegion_;

        template <typename S>
        void serialize(S& s)
        {
            s.object(object_);
            s.object(sourceRegion_);
        }
    };

    Data* const data_;
};

}
