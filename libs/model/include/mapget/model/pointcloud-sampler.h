// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace mapget
{

inline constexpr uint32_t DefaultPointCloudMaxPoints = 13'999'999;

/** Evenly spaced original indices, retaining the quota exactly and both endpoints. */
class PointCloudIndexSelection
{
public:
    /** Complete inputs keep every index; zero quota selects nothing. */
    PointCloudIndexSelection(uint64_t sourceCount, uint32_t quota)
        : sourceCount_(sourceCount), count_(std::min<uint64_t>(sourceCount, quota)) {}

    [[nodiscard]] uint64_t size() const { return count_; }

    /** Split the quotient to avoid overflowing the source-count/index product. */
    [[nodiscard]] uint64_t index(uint64_t ordinal) const
    {
        if (ordinal >= count_) throw std::out_of_range("Point-cloud sample index out of range");
        if (count_ == 1) return 0;
        auto const span = sourceCount_ - 1;
        auto const steps = count_ - 1;
        return ordinal * (span / steps) + ordinal * (span % steps) / steps;
    }

private:
    uint64_t sourceCount_;
    uint64_t count_;
};

}
