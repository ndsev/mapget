// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mapget
{

/** A finite Cartesian grid with power-of-two axis counts and a bounded cell product. */
class PointCloudGrid
{
public:
    using Position = std::array<double, 3>;

    /** Choose approximately isotropic cells within a quota; bounds include both endpoints. */
    PointCloudGrid(Position lower, Position upper, size_t maxCells, double minimumSpacing = 0.)
        : lower_(lower), upper_(upper)
    {
        if (!maxCells || maxCells > std::numeric_limits<uint32_t>::max() ||
            !std::isfinite(minimumSpacing) || minimumSpacing < 0.)
            throw std::invalid_argument("Invalid point-cloud grid quota or spacing");
        Position width{};
        for (size_t axis = 0; axis < 3; ++axis) {
            width[axis] = upper[axis] - lower[axis];
            if (!std::isfinite(lower[axis]) || !std::isfinite(upper[axis]) ||
                !std::isfinite(width[axis]) || width[axis] < 0.)
                throw std::invalid_argument("Invalid point-cloud grid bounds");
        }
        while (cellCount_ <= maxCells / 2) {
            auto axis = size_t{0};
            for (size_t candidate = 1; candidate < 3; ++candidate)
                if (width[candidate] / dimensions_[candidate] > width[axis] / dimensions_[axis])
                    axis = candidate;
            if (width[axis] == 0. || width[axis] / dimensions_[axis] / 2. < minimumSpacing)
                break;
            dimensions_[axis] *= 2;
            cellCount_ *= 2;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            scale_[axis] = width[axis] == 0. ? 0. : dimensions_[axis] / width[axis];
            if (!std::isfinite(scale_[axis]))
                throw std::invalid_argument("Point-cloud bounds are too small for a finite grid scale");
        }
    }

    /** Return the flat cell index; reject invalid/out-of-domain records instead of clipping them. */
    template <typename PositionType>
    [[nodiscard]] size_t index(PositionType const& position) const
    {
        std::array<size_t, 3> cell{};
        for (size_t axis = 0; axis < 3; ++axis) {
            double const value = position[axis];
            if (!std::isfinite(value) || value < lower_[axis] || value > upper_[axis])
                throw std::out_of_range("Point outside point-cloud grid bounds");
            cell[axis] = std::min<size_t>(
                static_cast<size_t>((value - lower_[axis]) * scale_[axis]), dimensions_[axis] - 1);
        }
        return (cell[2] * dimensions_[1] + cell[1]) * dimensions_[0] + cell[0];
    }

    /** Number of possible cells, including empty cells. */
    [[nodiscard]] size_t cellCount() const { return cellCount_; }
    /** Axis counts define aligned coarser grids when the same bounds are reused. */
    [[nodiscard]] std::array<uint32_t, 3> dimensions() const { return dimensions_; }
    /** Occupancy bytes including word padding. */
    [[nodiscard]] size_t occupancyBytes() const { return ((cellCount_ + 63) / 64) * sizeof(uint64_t); }

private:
    Position lower_;
    Position upper_;
    Position scale_{};
    std::array<uint32_t, 3> dimensions_{1, 1, 1};
    size_t cellCount_ = 1;
};

/**
 * Bounded first-point selection in source order. Record::position supplies Cartesian XYZ;
 * the remaining fields carry the original record/provenance without averaging attributes.
 * The caller validates every source record, including records that are not retained.
 */
template <typename Record>
class PointCloudSampler
{
public:
    /** A zero quota retains no records. Small inputs keep every point, including duplicates. */
    PointCloudSampler(PointCloudGrid grid, size_t quota, bool preserveSmallInputs = true)
        : grid_(std::move(grid)), quota_(quota), reduced_(!preserveSmallInputs)
    {
        if (quota_ && grid_.cellCount() > quota_)
            throw std::invalid_argument("Point-cloud grid exceeds its point quota");
        if (quota_ > records_.max_size())
            throw std::length_error("Point-cloud record quota is not representable");
        if (reduced_ && quota_)
            occupied_.resize((grid_.cellCount() + 63) / 64);
    }

    /** Count and consider one original record; input ordering must be stable across requests. */
    void add(Record const& record)
    {
        if (sourceCount_ == std::numeric_limits<uint64_t>::max())
            throw std::length_error("Point-cloud source count overflow");
        ++sourceCount_;
        if (!quota_) return;
        if (!reduced_ && records_.size() < quota_) {
            append(record);
            return;
        }
        if (!reduced_) {
            occupied_.resize((grid_.cellCount() + 63) / 64);
            size_t output = 0;
            for (size_t input = 0; input < records_.size(); ++input) {
                if (occupy(records_[input].position)) {
                    if (output != input) records_[output] = std::move(records_[input]);
                    ++output;
                }
            }
            records_.resize(output);
            reduced_ = true;
        }
        if (occupy(record.position)) append(record);
    }

    /** Original point count, independent of cell occupancy and sensor observation counts. */
    [[nodiscard]] uint64_t sourceCount() const { return sourceCount_; }
    /** Selected records remain in their original traversal order. */
    [[nodiscard]] std::vector<Record> const& records() const { return records_; }
    /** Transfer the selected records after the final input record. */
    [[nodiscard]] std::vector<Record> takeRecords() { return std::move(records_); }
    /** Retained capacity only; caller also accounts for input, decoder and output storage. */
    [[nodiscard]] size_t allocatedBytes() const
    {
        return records_.capacity() * sizeof(Record) + occupied_.capacity() * sizeof(uint64_t);
    }

private:
    void append(Record const& record)
    {
        if (records_.size() == records_.capacity())
            records_.reserve(std::min(quota_, std::max<size_t>(64, records_.capacity() * 2)));
        records_.push_back(record);
    }

    /** Mark one previously unseen cell in this request-local sampler. */
    template <typename PositionType>
    bool occupy(PositionType const& position)
    {
        auto const index = grid_.index(position);
        auto& word = occupied_[index / 64];
        auto const mask = uint64_t{1} << (index % 64);
        if (word & mask) return false;
        word |= mask;
        return true;
    }

    PointCloudGrid grid_;
    size_t quota_;
    bool reduced_;
    uint64_t sourceCount_ = 0;
    std::vector<Record> records_;
    std::vector<uint64_t> occupied_;
};

}
