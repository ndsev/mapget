#pragma once

#include <cstdint>
#include <filesystem>

namespace mapget
{

struct GeonamesImportStats
{
    uint64_t rowsRead = 0;
    uint64_t rowsImported = 0;
};

GeonamesImportStats createGeonamesLocationDatabase(
    std::filesystem::path const& inputTxt,
    std::filesystem::path const& outputSqlite);

}  // namespace mapget
