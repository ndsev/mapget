#include "mapget/location/location.h"

#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/format.h"

namespace
{

class LocationTestDatabaseError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/** One compact row for building the SQLite lookup fixture used by these tests. */
struct TestLocationRow
{
    int64_t geonameId = 0;
    std::string name;
    std::string asciiName;
    std::string alternateNames;
    double latitude = 0;
    double longitude = 0;
    std::string featureCode;
    std::string countryCode;
    std::optional<int64_t> population;
};

/** Create a unique temporary directory for one location lookup test. */
std::filesystem::path makeTempDir()
{
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::current_path() /
        fmt::format("mapget-location-test-{}-{}", now, counter.fetch_add(1));
    std::filesystem::create_directories(path);
    return path;
}

/** Raise a test fixture failure with the current SQLite error text. */
[[noreturn]] void raiseSqlite(sqlite3* db, std::string_view message)
{
    throw LocationTestDatabaseError(fmt::format("{}: {}", message, sqlite3_errmsg(db)));
}

/** Execute schema and FTS maintenance SQL for the test database. */
void execSql(sqlite3* db, char const* sql)
{
    char* errMsg = nullptr;
    auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string error = errMsg ? errMsg : sqlite3_errmsg(db);
        sqlite3_free(errMsg);
        throw LocationTestDatabaseError(error);
    }
}

/** Bind either a text value or NULL when the field is intentionally empty. */
void bindTextOrNull(sqlite3_stmt* stmt, int index, std::string const& value)
{
    if (value.empty()) {
        sqlite3_bind_null(stmt, index);
        return;
    }
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

/** Bind either an integer value or NULL when the field is absent. */
void bindIntegerOrNull(sqlite3_stmt* stmt, int index, std::optional<int64_t> value)
{
    if (!value.has_value()) {
        sqlite3_bind_null(stmt, index);
        return;
    }
    sqlite3_bind_int64(stmt, index, *value);
}

/** Create a small SQLite database with the production schema and supplied rows. */
void createTestLocationDatabase(std::filesystem::path const& dbPath, std::vector<TestLocationRow> const& rows)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &db) != SQLITE_OK) {
        std::string error = db ? sqlite3_errmsg(db) : "unknown error";
        if (db) {
            sqlite3_close(db);
        }
        throw LocationTestDatabaseError(fmt::format("failed to open test location database: {}", error));
    }

    sqlite3_stmt* insertStmt = nullptr;
    try {
        execSql(db, R"sql(
            CREATE TABLE location (
              geoname_id INTEGER PRIMARY KEY,
              name TEXT NOT NULL,
              ascii_name TEXT NOT NULL,
              alternate_names TEXT,
              latitude REAL NOT NULL,
              longitude REAL NOT NULL,
              feature_class TEXT NOT NULL,
              feature_code TEXT NOT NULL,
              country_code TEXT NOT NULL,
              admin1_code TEXT,
              admin2_code TEXT,
              population INTEGER,
              elevation INTEGER,
              timezone TEXT,
              modification_date TEXT
            );
            CREATE INDEX location_country_idx ON location(country_code);
            CREATE INDEX location_population_idx ON location(population DESC);
            CREATE VIRTUAL TABLE location_fts USING fts5(
              name,
              ascii_name,
              alternate_names,
              content='location',
              content_rowid='geoname_id',
              tokenize='unicode61 remove_diacritics 1'
            );
        )sql");

        auto rc = sqlite3_prepare_v2(db, R"sql(
            INSERT INTO location (
                geoname_id,
                name,
                ascii_name,
                alternate_names,
                latitude,
                longitude,
                feature_class,
                feature_code,
                country_code,
                admin1_code,
                admin2_code,
                population,
                elevation,
                timezone,
                modification_date
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )sql", -1, &insertStmt, nullptr);
        if (rc != SQLITE_OK) {
            raiseSqlite(db, "failed to prepare test location insert");
        }

        execSql(db, "BEGIN");
        for (auto const& row : rows) {
            sqlite3_reset(insertStmt);
            sqlite3_clear_bindings(insertStmt);
            sqlite3_bind_int64(insertStmt, 1, row.geonameId);
            sqlite3_bind_text(insertStmt, 2, row.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertStmt, 3, row.asciiName.c_str(), -1, SQLITE_TRANSIENT);
            bindTextOrNull(insertStmt, 4, row.alternateNames);
            sqlite3_bind_double(insertStmt, 5, row.latitude);
            sqlite3_bind_double(insertStmt, 6, row.longitude);
            sqlite3_bind_text(insertStmt, 7, "P", -1, SQLITE_STATIC);
            sqlite3_bind_text(insertStmt, 8, row.featureCode.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertStmt, 9, row.countryCode.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_null(insertStmt, 10);
            sqlite3_bind_null(insertStmt, 11);
            bindIntegerOrNull(insertStmt, 12, row.population);
            sqlite3_bind_null(insertStmt, 13);
            sqlite3_bind_text(insertStmt, 14, "UTC", -1, SQLITE_STATIC);
            sqlite3_bind_text(insertStmt, 15, "2026-01-01", -1, SQLITE_STATIC);
            rc = sqlite3_step(insertStmt);
            if (rc != SQLITE_DONE) {
                raiseSqlite(db, "failed to insert test location row");
            }
        }
        sqlite3_finalize(insertStmt);
        insertStmt = nullptr;
        execSql(db, R"sql(
            INSERT INTO location_fts(rowid, name, ascii_name, alternate_names)
            SELECT geoname_id, name, ascii_name, COALESCE(alternate_names, '')
            FROM location;
        )sql");
        execSql(db, "COMMIT");
        sqlite3_close(db);
    }
    catch (...) {
        if (insertStmt) {
            sqlite3_finalize(insertStmt);
        }
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        throw;
    }
}

}  // namespace

TEST_CASE("GeoNames location lookup searches rows and ranks matches", "[Location]")
{
    auto tempDir = makeTempDir();
    auto dbPath = tempDir / "geonames-cities5000.sqlite";

    createTestLocationDatabase(dbPath, {
        {2867714, "Muenchen", "Munich", "Muenchen,München", 48.13743, 11.57549, "PPLA", "DE", 1260391},
        {5690557, "Munich", "Munich", "Munich North Dakota", 46.80667, -98.83926, "PPL", "US", 190},
        {2867543, "Munsingen", "Munsingen", "Münsingen", 48.41126, 9.49704, "PPL", "DE", 14000},
    });

    {
        mapget::SqliteLocationLookup lookup(dbPath);
        REQUIRE(lookup.available());

        auto munichMatches = lookup.search("munich", 10);
        REQUIRE(munichMatches.size() == 2);
        CHECK(munichMatches[0].id == "geonames:2867714");
        CHECK(munichMatches[0].name == "Munich, DE");
        CHECK(std::abs(munichMatches[0].lonLat.longitude - 11.57549) < 1e-8);
        CHECK(std::abs(munichMatches[0].lonLat.latitude - 48.13743) < 1e-8);
        CHECK(std::abs(munichMatches[0].aabb.southWest.longitude - 11.57549) < 1e-8);
        CHECK(std::abs(munichMatches[0].aabb.southWest.latitude - 48.13743) < 1e-8);
        CHECK(munichMatches[0].aabb.extent.longitude == 0);
        CHECK(munichMatches[0].aabb.extent.latitude == 0);
        REQUIRE(munichMatches[0].population.has_value());
        CHECK(*munichMatches[0].population == 1260391);

        auto alternateNameMatches = lookup.search("muen", 10);
        REQUIRE_FALSE(alternateNameMatches.empty());
        CHECK(alternateNameMatches[0].id == "geonames:2867714");

        auto limitedMatches = lookup.search("mun", 1);
        REQUIRE(limitedMatches.size() == 1);

        auto serialized = munichMatches[0].serialize();
        CHECK(std::abs(serialized["lonLat"][0].get<double>() - 11.57549) < 1e-8);
        CHECK(std::abs(serialized["lonLat"][1].get<double>() - 48.13743) < 1e-8);
        CHECK(std::abs(serialized["aabb"][0][0].get<double>() - 11.57549) < 1e-8);
        CHECK(std::abs(serialized["aabb"][0][1].get<double>() - 48.13743) < 1e-8);
        CHECK(serialized["aabb"][1][0] == 0);
        CHECK(serialized["aabb"][1][1] == 0);
    }

    std::filesystem::remove_all(tempDir);
}

TEST_CASE("GeoNames location lookup ignores empty and unavailable inputs", "[Location]")
{
    auto lookup = mapget::SqliteLocationLookup(std::filesystem::path("does-not-exist.sqlite"));
    CHECK_FALSE(lookup.available());
    CHECK(lookup.search("Munich", 10).empty());
}
