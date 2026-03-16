#pragma once

#include <cassert>
#include <cstdint>
#include "simfil/model/column.h"
#include "simfil/model/string-pool.h"

namespace mapget
{

/**
 * Type uniquely identifying the source of data. For data loaded from a zserio
 * buffer, this is the position and length in bits in the blob.
 */
struct SourceDataAddress
{
    MODEL_COLUMN_TYPE(8);

    static constexpr uint64_t BitMask = 0xffffffff;

    uint32_t bitOffset_ = 0u;
    uint32_t bitSize_ = 0u;

    SourceDataAddress() = default;

    SourceDataAddress(uint32_t bitOffset, uint32_t bitSize)
        : bitOffset_(bitOffset),
          bitSize_(bitSize)
    {}

    explicit SourceDataAddress(uint64_t value)
        : bitOffset_(static_cast<uint32_t>((value >> 32) & BitMask)),
          bitSize_(static_cast<uint32_t>(value & BitMask))
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

        return SourceDataAddress{
            static_cast<uint32_t>(offset),
            static_cast<uint32_t>(size)};
    }

    uint64_t u64() const
    {
        return (static_cast<uint64_t>(bitOffset_) << 32) | bitSize_;
    }

    uint32_t bitSize() const
    {
        return bitSize_;
    }

    uint32_t bitOffset() const
    {
        return bitOffset_;
    }

    /**
     * Bitsery interface
     */
    template <typename S>
    void serialize(S& s)
    {
        s.value4b(bitOffset_);
        s.value4b(bitSize_);
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
    MODEL_COLUMN_TYPE(12);

    /** Region in the source blob */
    SourceDataAddress address_;

    /** Layer Id */
    simfil::StringId layerId_;

    template <typename S>
    void serialize(S& s)
    {
        s.object(address_);
        s.value2b(layerId_);
    }
};

}
