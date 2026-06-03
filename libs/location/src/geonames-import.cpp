#include "mapget/location/geonames-import.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mapget
{
namespace
{

void raiseSqlite(sqlite3* db, std::string_view message)
{
    throw std::runtime_error(std::string(message) + ": " + sqlite3_errmsg(db));
}

void execSql(sqlite3* db, char const* sql)
{
    char* errMsg = nullptr;
    auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string message = errMsg ? errMsg : sqlite3_errmsg(db);
        sqlite3_free(errMsg);
        throw std::runtime_error(message);
    }
}

std::vector<std::string_view> splitTabs(std::string_view line)
{
    std::vector<std::string_view> result;
    size_t begin = 0;
    while (begin <= line.size()) {
        auto end = line.find('\t', begin);
        if (end == std::string_view::npos) {
            result.emplace_back(line.substr(begin));
            break;
        }
        result.emplace_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}

int64_t parseInteger(std::string_view value, int64_t fallback = 0)
{
    if (value.empty()) {
        return fallback;
    }
    size_t parsed = 0;
    auto text = std::string(value);
    auto result = std::stoll(text, &parsed);
    if (parsed != text.size()) {
        return fallback;
    }
    return result;
}

double parseDouble(std::string_view value)
{
    size_t parsed = 0;
    auto text = std::string(value);
    auto result = std::stod(text, &parsed);
    if (parsed != text.size()) {
        throw std::runtime_error("invalid floating-point value");
    }
    return result;
}

void bindTextOrNull(sqlite3_stmt* stmt, int index, std::string_view value)
{
    if (value.empty()) {
        sqlite3_bind_null(stmt, index);
        return;
    }
    sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void bindIntegerOrNull(sqlite3_stmt* stmt, int index, std::string_view value)
{
    if (value.empty()) {
        sqlite3_bind_null(stmt, index);
        return;
    }
    sqlite3_bind_int64(stmt, index, parseInteger(value));
}

void createSchema(sqlite3* db)
{
    execSql(db, R"sql(
        PRAGMA journal_mode = OFF;
        PRAGMA synchronous = OFF;
        PRAGMA temp_store = MEMORY;
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
          tokenize='unicode61 remove_diacritics 2'
        );
    )sql");
}

}  // namespace

GeonamesImportStats createGeonamesLocationDatabase(
    std::filesystem::path const& inputTxt,
    std::filesystem::path const& outputSqlite)
{
    std::ifstream input(inputTxt, std::ios::in);
    if (!input) {
        throw std::runtime_error("failed to open GeoNames input: " + inputTxt.string());
    }

    auto outputDirectory = outputSqlite.parent_path();
    if (!outputDirectory.empty()) {
        std::filesystem::create_directories(outputDirectory);
    }

    auto tempPath = outputSqlite;
    tempPath += ".tmp";
    std::filesystem::remove(tempPath);

    sqlite3* db = nullptr;
    if (sqlite3_open(tempPath.string().c_str(), &db) != SQLITE_OK) {
        std::string error = db ? sqlite3_errmsg(db) : "unknown error";
        if (db) {
            sqlite3_close(db);
        }
        throw std::runtime_error("failed to open output database: " + error);
    }

    try {
        createSchema(db);
        execSql(db, "BEGIN");

        sqlite3_stmt* insertStmt = nullptr;
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
            raiseSqlite(db, "failed to prepare GeoNames insert");
        }

        GeonamesImportStats stats;
        std::string line;
        while (std::getline(input, line)) {
            ++stats.rowsRead;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            auto columns = splitTabs(line);
            if (columns.size() != 19) {
                sqlite3_finalize(insertStmt);
                throw std::runtime_error("GeoNames row does not have 19 columns at line " + std::to_string(stats.rowsRead));
            }

            sqlite3_reset(insertStmt);
            sqlite3_clear_bindings(insertStmt);
            sqlite3_bind_int64(insertStmt, 1, parseInteger(columns[0]));
            bindTextOrNull(insertStmt, 2, columns[1]);
            bindTextOrNull(insertStmt, 3, columns[2].empty() ? columns[1] : columns[2]);
            bindTextOrNull(insertStmt, 4, columns[3]);
            sqlite3_bind_double(insertStmt, 5, parseDouble(columns[4]));
            sqlite3_bind_double(insertStmt, 6, parseDouble(columns[5]));
            bindTextOrNull(insertStmt, 7, columns[6]);
            bindTextOrNull(insertStmt, 8, columns[7]);
            bindTextOrNull(insertStmt, 9, columns[8]);
            bindTextOrNull(insertStmt, 10, columns[10]);
            bindTextOrNull(insertStmt, 11, columns[11]);
            bindIntegerOrNull(insertStmt, 12, columns[14]);
            bindIntegerOrNull(insertStmt, 13, columns[15]);
            bindTextOrNull(insertStmt, 14, columns[17]);
            bindTextOrNull(insertStmt, 15, columns[18]);

            rc = sqlite3_step(insertStmt);
            if (rc != SQLITE_DONE) {
                sqlite3_finalize(insertStmt);
                raiseSqlite(db, "failed to insert GeoNames row");
            }
            ++stats.rowsImported;
        }

        sqlite3_finalize(insertStmt);
        execSql(db, R"sql(
            INSERT INTO location_fts(rowid, name, ascii_name, alternate_names)
            SELECT geoname_id, name, ascii_name, COALESCE(alternate_names, '')
            FROM location;
        )sql");
        execSql(db, "COMMIT");
        sqlite3_close(db);

        std::filesystem::remove(outputSqlite);
        std::filesystem::rename(tempPath, outputSqlite);
        return stats;
    }
    catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        std::filesystem::remove(tempPath);
        throw;
    }
}

}  // namespace mapget
