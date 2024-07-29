#pragma once

#include <cstdint>
#include "simfil/model/string-pool.h"
#include "fmt/format.h"

namespace mapget
{

/**
 * Type uniquely identifying the source of data. For data loaded from a zserio
 * buffer, this is the position and length in bits in the blob.
 */
struct SourceDataAddress
{
    union {
        struct
        {
            unsigned low_  : 32;
            unsigned high_ : 32;
        };
        uint64_t u64_;
    } data_;

    SourceDataAddress()
    {
        data_.u64_ = 0u;
    }

    explicit SourceDataAddress(uint64_t value)
    {
        data_.u64_ = value;
    }

    /**
     * Create a SourceDataAddress from an offset and size in bits, useful
     * for creating addresses for objects read from a zserio buffer.
     */
    static SourceDataAddress fromBitPosition(size_t offset, size_t size)
    {
        SourceDataAddress addr;
        addr.data_.high_ = static_cast<uint32_t>(offset);
        addr.data_.low_ = static_cast<uint32_t>(size);
        return addr;
    }

    uint64_t u64() const
    {
        return data_.u64_;
    }

    uint32_t low() const
    {
        return data_.low_;
    }

    uint32_t high() const
    {
        return data_.high_;
    }

    /**
     * Bitsery interface
     */
    template <typename S>
    void serialize(S& s)
    {
        s.value8b(data_.u64_);
    }
};

/**
 * Info attached to feature components to identify the corresponding source data
 * region. Other metadata such as the zserio Type is attached to the blob-tree
 * and can be found by looking for the tree node(s) that match the target blobs
 * SourceRegion.
 */
struct SourceDataReference
{
    /** Layer Id */
    simfil::StringId layerId_;

    /** Region in the source blob */
    SourceDataAddress address_;

    template <typename S>
    void serialize(S& s)
    {
        s.value2b(layerId_);
        s.object(address_);
    }
};

}
