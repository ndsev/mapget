#include "mapget/location/location.h"
#include <charconv>
#include "mapget/log.h"
#include "plazs/gazetteer.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace mapget
{
namespace
{
int kModuleAnchor = 0;
/** Resolve the binary module containing this implementation. */
std::filesystem::path moduleDirectory()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&kModuleAnchor), &module)) {
        std::wstring buffer(256, L'\0');
        while (buffer.size() <= 32768) {
            auto size =
                GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0) {
                break;
            }
            // A full buffer means the path was truncated; retry for long
            // virtual-environment and package installation paths.
            if (size < buffer.size()) {
                buffer.resize(size);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }
#else
    Dl_info info{};
    if (dladdr(&kModuleAnchor, &info) != 0 && info.dli_fname && *info.dli_fname) {
        return std::filesystem::path(info.dli_fname).parent_path();
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
    return nlohmann::json::array({southWest.serialize(), extent.serialize()});
}

nlohmann::json LocationMatch::serialize() const
{
    nlohmann::json result = {
        {"id", id},
        {"name", name},
        {"lonLat", lonLat.serialize()},
        {"aabb", aabb.serialize()},
        {"source", source},
        {"countryCode", countryCode}};
    if (population.has_value()) {
        result["population"] = *population;
    }
    result["geometryAvailable"] = geometryAvailable;
    result["placeType"] = placeType;
    if (source == "whosonfirst")
        result["attribution"] = {
            {"name", "Who's On First"},
            {"url", "https://whosonfirst.org/"},
            {"licenseUrl", "https://whosonfirst.org/docs/licenses/"}};
    if (geometry)
        result["geometry"] = *geometry;
    return result;
}

SqliteLocationLookup::SqliteLocationLookup(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath))
{
    try {
        gazetteer_ = std::make_unique<plazs::Gazetteer>(databasePath_);
    }
    catch (std::exception const& error) {
        // Optional place lookup must not prevent the tile service from starting.
        log().warn("Location database {} unavailable: {}", databasePath_.string(), error.what());
    }
}

SqliteLocationLookup::~SqliteLocationLookup() = default;

bool SqliteLocationLookup::available() const
{
    return static_cast<bool>(gazetteer_);
}

std::filesystem::path const& SqliteLocationLookup::databasePath() const
{
    return databasePath_;
}

LocationMatch SqliteLocationLookup::adapt(plazs::Place place)
{
    LocationMatch match;
    match.id = "wof:" + std::to_string(place.id);
    match.name = std::move(place.name);
    match.countryCode = std::move(place.countryCode);
    if (!match.countryCode.empty())
        match.name += ", " + match.countryCode;
    match.source = "whosonfirst";
    match.lonLat = {place.position[0], place.position[1]};
    auto const& bounds = place.bounds;
    match.aabb.southWest = {bounds[0], bounds[1]};
    // The public mapget bbox is an origin plus extent, including dateline-crossing boxes.
    match.aabb.extent = {
        bounds[2] - bounds[0] + (bounds[2] < bounds[0] ? 360 : 0),
        bounds[3] - bounds[1]};
    match.population = place.population;
    match.placeType = std::move(place.placeType);
    match.geometryAvailable = place.geometryAvailable;
    match.geometry = std::move(place.geometry);
    return match;
}

std::vector<LocationMatch> SqliteLocationLookup::search(std::string_view name, uint32_t limit) const
{
    std::vector<LocationMatch> matches;
    if (gazetteer_)
        for (auto& place : gazetteer_->search(name, limit))
            matches.push_back(adapt(std::move(place)));
    return matches;
}

std::optional<LocationMatch> SqliteLocationLookup::find(std::string_view id) const
{
    constexpr std::string_view prefix = "wof:";
    if (!gazetteer_ || !id.starts_with(prefix))
        return {};
    id.remove_prefix(prefix.size());
    int64_t numericId = 0;
    auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), numericId);
    if (error != std::errc() || end != id.data() + id.size() || numericId <= 0)
        return {};
    if (auto place = gazetteer_->find(numericId))
        return adapt(std::move(*place));
    return {};
}

std::filesystem::path defaultLocationDatabasePath()
{
    return moduleDirectory() / "mapget-places.sqlite";
}

}  // namespace mapget
