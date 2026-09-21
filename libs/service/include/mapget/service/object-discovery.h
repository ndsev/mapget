#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "mapget/model/partitionid.h"

namespace mapget
{
/** One map/layer association query. Its tile is discovery coverage, never an object's identity. */
struct ObjectDiscoveryRequest
{
    std::string mapId_;
    std::string layerId_;
    TileId tileId_;
    std::optional<std::string> sourceId_;
};

/** Associations for one discovery tile; failure is not an empty successful list. */
struct ObjectDiscoveryResult
{
    /**
     * A map/layer-scoped object identity with optional [west, south, east, north] WGS84 bounds.
     * Bounds must be finite and ordered south <= north; west > east denotes an antimeridian
     * crossing. Bounds do not define the object's geometry anchor or clip its payload.
     */
    struct Reference
    {
        uint64_t objectId_ = 0;
        std::optional<std::array<double, 4>> bounds_;
    };
    /** Missing association data remains distinguishable from a transport/backend error. */
    enum class Status { Success, Unavailable, Failed };
    Status status_ = Status::Success;
    std::vector<Reference> objects_;
    std::string message_;
    std::chrono::system_clock::time_point timestamp_ = std::chrono::system_clock::now();
    /** Independent association freshness; zero means no expiry. */
    std::chrono::milliseconds ttl_{0};

    /** Reject invalid freshness, bounds, or successful-looking payloads on failed responses. */
    void validate() const;

    /**
     * Validate and serialize IDs as unsigned decimal strings and timestamps as Unix milliseconds.
     * Duplicate object IDs are collapsed, retaining the first reference (including its bounds).
     */
    nlohmann::json toJson() const;
    /** Parse a datasource response, preserving unavailable/empty distinction. */
    static ObjectDiscoveryResult fromJson(nlohmann::json const& json);
};
}  // namespace mapget
