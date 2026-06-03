#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

struct sqlite3;

namespace mapget
{

/** One normalized place-name match returned by a location lookup backend. */
struct LocationMatch
{
    /** Stable match identifier within the source provider, for example geonames:<geoname_id>. */
    std::string id;
    /** Display-ready English/ascii location name. */
    std::string name;
    /** Authoritative WGS84 longitude used for map jumps. */
    double longitude = 0;
    /** Authoritative WGS84 latitude used for map jumps. */
    double latitude = 0;
    /** Longitude extent of the match bounding box; zero for point-only providers. */
    double extentLongitude = 0;
    /** Latitude extent of the match bounding box; zero for point-only providers. */
    double extentLatitude = 0;
    /** Provider or database identifier that produced this match. */
    std::string source;
    /** ISO country code when the provider exposes one. */
    std::string countryCode;
    /** Population hint used by clients for deterministic result sorting. */
    std::optional<int64_t> population;

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
};

/** Read-only SQLite implementation for the bundled GeoNames location database. */
class SqliteLocationLookup : public LocationLookup
{
public:
    /** Open a SQLite location database; unavailable databases are represented by available() == false. */
    explicit SqliteLocationLookup(std::filesystem::path databasePath);
    ~SqliteLocationLookup() override;

    SqliteLocationLookup(SqliteLocationLookup const&) = delete;
    SqliteLocationLookup& operator=(SqliteLocationLookup const&) = delete;

    /** Return whether the SQLite database was opened successfully. */
    bool available() const;
    /** Return the configured database path, even when opening it failed. */
    std::filesystem::path const& databasePath() const;
    /** Search the FTS index and rank results by match quality, place type, and population. */
    std::vector<LocationMatch> search(std::string_view name, uint32_t limit) const override;

private:
    std::filesystem::path databasePath_;
    sqlite3* db_ = nullptr;
};

/** Resolve the default bundled location database path next to the running executable. */
std::filesystem::path defaultLocationDatabasePath();

}  // namespace mapget
