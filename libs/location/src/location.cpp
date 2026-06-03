#include "mapget/location/location.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "mapget/log.h"

namespace mapget
{
namespace
{

constexpr uint32_t kHardMaxLimit = 50;
constexpr std::string_view kSourceName = "geonames-cities5000";

/** Trim leading and trailing ASCII whitespace before building an FTS query. */
std::string trim(std::string_view value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

/** Return whether a byte can be kept unescaped in a SQLite FTS token. */
bool isAsciiTokenChar(unsigned char c)
{
    return std::isalnum(c) || c == '_';
}

/** Build a bounded prefix query from user text for the GeoNames FTS index. */
std::string buildFtsPrefixQuery(std::string_view input)
{
    std::vector<std::string> tokens;
    std::string token;
    for (unsigned char c : input) {
        if (c < 0x80 && !isAsciiTokenChar(c)) {
            if (!token.empty()) {
                tokens.push_back(std::move(token));
                token.clear();
            }
            continue;
        }
        if (token.size() < 64) {
            token.push_back(static_cast<char>(c));
        }
    }
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
    if (tokens.size() > 8) {
        tokens.resize(8);
    }

    std::ostringstream query;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) {
            query << " AND ";
        }
        query << tokens[i] << "*";
    }
    return query.str();
}

/** Resolve the directory that contains the current process executable. */
std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0) {
        buffer.resize(size);
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return std::filesystem::weakly_canonical(std::filesystem::path(buffer)).parent_path();
    }
#elif defined(__linux__)
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !path.empty()) {
        return path.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

}  // namespace

nlohmann::json LocationPoint::serialize() const
{
    return nlohmann::json::array({longitude, latitude});
}

nlohmann::json LocationAabb::serialize() const
{
    return nlohmann::json::array({
        southWest.serialize(),
        extent.serialize()
    });
}

nlohmann::json LocationMatch::serialize() const
{
    nlohmann::json result = {
        {"id", id},
        {"name", name},
        {"lonLat", lonLat.serialize()},
        {"aabb", aabb.serialize()},
        {"source", source},
        {"countryCode", countryCode}
    };
    if (population.has_value()) {
        result["population"] = *population;
    }
    return result;
}

SqliteLocationLookup::SqliteLocationLookup(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath))
{
    if (databasePath_.empty() || !std::filesystem::exists(databasePath_)) {
        return;
    }

    auto rc = sqlite3_open_v2(
        databasePath_.string().c_str(),
        &db_,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        log().warn("Failed to open location database {}: {}", databasePath_.string(), db_ ? sqlite3_errmsg(db_) : "unknown error");
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
}

SqliteLocationLookup::~SqliteLocationLookup()
{
    if (db_) {
        sqlite3_close(db_);
    }
}

bool SqliteLocationLookup::available() const
{
    return db_ != nullptr;
}

std::filesystem::path const& SqliteLocationLookup::databasePath() const
{
    return databasePath_;
}

std::vector<LocationMatch> SqliteLocationLookup::search(std::string_view name, uint32_t limit) const
{
    if (!db_) {
        return {};
    }
    auto trimmed = trim(name);
    if (trimmed.size() < 2) {
        return {};
    }
    auto ftsQuery = buildFtsPrefixQuery(trimmed);
    if (ftsQuery.empty()) {
        return {};
    }

    limit = std::max<uint32_t>(1, std::min<uint32_t>(limit, kHardMaxLimit));
    auto prefix = trimmed + "%";

    static constexpr char const* kQuery = R"sql(
        SELECT
          geoname_id,
          name,
          ascii_name,
          latitude,
          longitude,
          feature_code,
          country_code,
          population
        FROM location
        WHERE geoname_id IN (
          SELECT rowid FROM location_fts WHERE location_fts MATCH ?
        )
        ORDER BY
          CASE
            WHEN lower(ascii_name) = lower(?) OR lower(name) = lower(?) THEN 0
            WHEN lower(ascii_name) LIKE lower(?) OR lower(name) LIKE lower(?) THEN 1
            ELSE 2
          END,
          CASE feature_code
            WHEN 'PPLC' THEN 0
            WHEN 'PPLA' THEN 1
            WHEN 'PPLA2' THEN 2
            WHEN 'PPLA3' THEN 3
            WHEN 'PPL' THEN 4
            ELSE 5
          END,
          COALESCE(population, 0) DESC,
          ascii_name COLLATE NOCASE,
          country_code,
          geoname_id
        LIMIT ?
    )sql";

    sqlite3_stmt* stmt = nullptr;
    auto rc = sqlite3_prepare_v2(db_, kQuery, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        log().warn("Failed to prepare location query: {}", sqlite3_errmsg(db_));
        return {};
    }

    sqlite3_bind_text(stmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, trimmed.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, trimmed.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, static_cast<int>(limit));

    std::vector<LocationMatch> matches;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        auto textColumn = [stmt](int column) -> std::string {
            auto text = sqlite3_column_text(stmt, column);
            return text ? reinterpret_cast<char const*>(text) : "";
        };
        LocationMatch match;
        match.id = "geonames:" + std::to_string(sqlite3_column_int64(stmt, 0));
        auto nameValue = textColumn(2).empty() ? textColumn(1) : textColumn(2);
        auto countryCode = textColumn(6);
        match.name = countryCode.empty() ? nameValue : nameValue + ", " + countryCode;
        match.lonLat.latitude = sqlite3_column_double(stmt, 3);
        match.lonLat.longitude = sqlite3_column_double(stmt, 4);
        match.aabb.southWest = match.lonLat;
        match.countryCode = std::move(countryCode);
        match.source = std::string(kSourceName);
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            match.population = sqlite3_column_int64(stmt, 7);
        }
        matches.push_back(std::move(match));
    }
    if (rc != SQLITE_DONE) {
        log().warn("Location query failed: {}", sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
    return matches;
}

std::filesystem::path defaultLocationDatabasePath()
{
    return executableDirectory() / "geonames-cities5000.sqlite";
}

}  // namespace mapget
