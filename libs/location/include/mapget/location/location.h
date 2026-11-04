#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

namespace plazs
{
class Gazetteer;
struct Place;
}  // namespace plazs

namespace mapget
{

/** One WGS84 coordinate encoded as [longitude, latitude] in JSON. */
struct LocationPoint
{
    /** WGS84 longitude. */
    double longitude = 0;
    /** WGS84 latitude. */
    double latitude = 0;

    /** Serialize to the public [longitude, latitude] array shape. */
    nlohmann::json serialize() const;
};

/** One axis-aligned bounding box encoded as [[west, south], [extentLon, extentLat]]. */
struct LocationAabb
{
    /** South-west corner of the bounding box. */
    LocationPoint southWest;
    /** Longitudinal/latitudinal extent; zero for point-only providers. */
    LocationPoint extent;

    /** Serialize to the public /location aabb array shape. */
    nlohmann::json serialize() const;
};

/** One normalized place-name match returned by a location lookup backend. */
struct LocationMatch
{
    /** Stable match identifier within the source provider, for example wof:<id>. */
    std::string id;
    /** Display-ready location name, preferring the provider's English label when available. */
    std::string name;
    /** Representative WGS84 jump coordinate, not necessarily inside the boundary; serialized as
     * lonLat. */
    LocationPoint lonLat;
    /** Bounding box for the match, serialized as aabb. */
    LocationAabb aabb;
    /** Provider or database identifier that produced this match. */
    std::string source;
    /** ISO country code when the provider exposes one. */
    std::string countryCode;
    /** Population hint used by clients for deterministic result sorting. */
    std::optional<int64_t> population;
    /** Provider place category, e.g. country, region (state/province), or locality. */
    std::string placeType;
    /** Whether the prepared database retains a real polygon, independently of this request. */
    bool geometryAvailable = false;
    /** Optional GeoJSON Polygon/MultiPolygon. Only exact-ID requests load the boundary. */
    std::optional<nlohmann::json> geometry;

    /** Serialize to the public /location response object shape. */
    nlohmann::json serialize() const;
};

/** Abstract lookup contract for configured location providers. */
class LocationLookup
{
public:
    virtual ~LocationLookup() = default;
    /** Search by a user-entered name fragment and return at most limit matches. */
    virtual std::vector<LocationMatch> search(std::string_view name, uint32_t limit) const = 0;
    /** Resolve a stable provider ID including its boundary; unknown IDs return no match.
     * Large boundaries may raise std::length_error instead of returning a truncated polygon. */
    virtual std::optional<LocationMatch> find(std::string_view id) const = 0;
};

/** Adapt plazs place lookup to mapget identities, labels and JSON extent conventions. */
class SqliteLocationLookup : public LocationLookup
{
public:
    /** Open a SQLite location database; unavailable databases are represented by available() ==
     * false. */
    explicit SqliteLocationLookup(std::filesystem::path databasePath);
    ~SqliteLocationLookup() override;

    SqliteLocationLookup(SqliteLocationLookup const&) = delete;
    SqliteLocationLookup& operator=(SqliteLocationLookup const&) = delete;

    /** Return whether the SQLite database opened with a supported schema and FTS index. */
    bool available() const;
    /** Return the configured database path, even when opening it failed. */
    std::filesystem::path const& databasePath() const;
    /** Search the FTS index and rank results by match quality, place type, and population. */
    std::vector<LocationMatch> search(std::string_view name, uint32_t limit) const override;
    /** Resolve a namespaced ID; packed input, decoder allocation and output vertices are bounded.
     */
    std::optional<LocationMatch> find(std::string_view id) const override;

private:
    std::filesystem::path databasePath_;
    std::unique_ptr<plazs::Gazetteer> gazetteer_;

    /** Project one native place without duplicating search, geometry or storage logic. */
    static LocationMatch adapt(plazs::Place place);
};

/** Resolve the bundled database next to the binary module containing mapget. */
std::filesystem::path defaultLocationDatabasePath();

}  // namespace mapget
