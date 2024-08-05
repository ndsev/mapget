#pragma once

#include <cassert>
#include <cstdint>
#include "simfil/model/string-pool.h"

namespace mapget
{

/**
 * Type uniquely identifying the source of data. For data loaded from a zserio
 * buffer, this is the position and length in bits in the blob.
 */
struct SourceDataAddress
{
    static constexpr uint64_t BitMask = 0xffffffff;

    uint64_t value_ = 0u;

    SourceDataAddress() = default;

    explicit SourceDataAddress(uint64_t value)
        : value_(value)
    {}

    /**
     * Create a SourceDataAddress from an offset and size in bits, useful
     * for creating addresses for objects read from a zserio buffer.
     */
    static SourceDataAddress fromBitPosition(size_t offset, size_t size)
    {
        /* Assert that both values fit into a 32-bit integer. */
        assert((offset & BitMask) == offset);
        assert((size & BitMask) == size);

        return SourceDataAddress{(offset << 32) | (size & BitMask)};
    }

    uint64_t u64() const
    {
        return value_;
    }

    uint32_t bitSize() const
    {
        return value_ & BitMask;
    }

    uint32_t bitOffset() const
    {
        return (value_ >> 32) & BitMask;
    }

    /**
     * Bitsery interface
     */
    template <typename S>
    void serialize(S& s)
    {
        s.value8b(value_);
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
