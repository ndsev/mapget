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

struct LocationMatch
{
    std::string id;
    std::string name;
    std::string source;
    std::string countryCode;
    double longitude = 0;
    double latitude = 0;
    std::optional<int64_t> population;

    nlohmann::json serialize() const;
};

class LocationLookup
{
public:
    virtual ~LocationLookup() = default;
    virtual std::vector<LocationMatch> search(std::string_view name, uint32_t limit) const = 0;
};

class SqliteLocationLookup : public LocationLookup
{
public:
    explicit SqliteLocationLookup(std::filesystem::path databasePath);
    ~SqliteLocationLookup() override;

    SqliteLocationLookup(SqliteLocationLookup const&) = delete;
    SqliteLocationLookup& operator=(SqliteLocationLookup const&) = delete;

    bool available() const;
    std::filesystem::path const& databasePath() const;
    std::vector<LocationMatch> search(std::string_view name, uint32_t limit) const override;

private:
    std::filesystem::path databasePath_;
    sqlite3* db_ = nullptr;
};

std::filesystem::path defaultLocationDatabasePath();

}  // namespace mapget
