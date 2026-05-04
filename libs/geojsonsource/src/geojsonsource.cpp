// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "geojsonsource/geojsonsource.h"

#include "mapget/log.h"
#include "mapget/service/config.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "fmt/format.h"

#include <zlib.h>

namespace
{

using namespace mapget;

constexpr auto MANIFEST_FILENAME = "manifest.json";

[[nodiscard]] int defaultParallelJobs()
{
    const auto hardwareThreads = static_cast<int>(std::thread::hardware_concurrency());
    return std::max(static_cast<int>(0.33 * std::max(hardwareThreads, 1)), 2);
}

[[nodiscard]] bool looksLikeHttpUrl(std::string_view value)
{
    return value.starts_with("http://") || value.starts_with("https://");
}

[[nodiscard]] std::string trimmed(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

[[nodiscard]] std::string replaceAll(
    std::string text,
    std::string_view needle,
    std::string_view replacement)
{
    if (needle.empty())
        return text;

    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return text;
}

[[nodiscard]] std::string renderTemplate(
    std::string const& input,
    uint64_t tileId,
    std::string_view layerId,
    std::string_view baseUrl = {})
{
    auto result = replaceAll(input, "{tileId}", std::to_string(tileId));
    result = replaceAll(result, "{layerId}", layerId);
    if (!baseUrl.empty())
        result = replaceAll(result, "{baseUrl}", baseUrl);
    return result;
}

struct ParsedHttpUrl
{
    std::string origin;
    std::string pathAndQuery;
};

[[nodiscard]] ParsedHttpUrl splitHttpUrl(std::string_view url)
{
    if (!looksLikeHttpUrl(url))
        raise(fmt::format("Expected HTTP(S) URL, got `{}`.", url));

    auto schemeEnd = url.find("://");
    auto pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string_view::npos)
        return {std::string(url), "/"};
    return {
        std::string(url.substr(0, pathStart)),
        std::string(url.substr(pathStart))};
}

[[nodiscard]] std::string joinUrlPrefixAndPath(
    std::string_view baseUrl,
    std::string_view relativePath)
{
    std::string result(baseUrl);
    if (!result.empty() && result.back() == '/' &&
        !relativePath.empty() && relativePath.front() == '/') {
        result.pop_back();
    }
    else if (!result.empty() && result.back() != '/' &&
             !relativePath.empty() && relativePath.front() != '/') {
        result.push_back('/');
    }
    result.append(relativePath);
    return result;
}

[[nodiscard]] bool hasGzipContentEncoding(std::string_view contentEncoding)
{
    if (contentEncoding.empty())
        return false;

    std::string normalized(contentEncoding);
    std::ranges::transform(
        normalized,
        normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized.find("gzip") != std::string::npos;
}

[[nodiscard]] bool looksLikeGzip(std::string_view bytes)
{
    return bytes.size() >= 2 &&
           static_cast<unsigned char>(bytes[0]) == 0x1f &&
           static_cast<unsigned char>(bytes[1]) == 0x8b;
}

[[nodiscard]] std::optional<std::string> gunzip(std::string_view input)
{
    if (input.empty())
        return std::string{};
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max()))
        return std::nullopt;

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
        return std::nullopt;

    std::string output;
    output.reserve(input.size() * 2);

    char outBuffer[8192];
    int inflateResult = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(outBuffer);
        stream.avail_out = sizeof(outBuffer);
        inflateResult = inflate(&stream, Z_NO_FLUSH);
        if (inflateResult != Z_OK && inflateResult != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        output.append(outBuffer, sizeof(outBuffer) - stream.avail_out);
    } while (inflateResult != Z_STREAM_END);

    inflateEnd(&stream);
    return output;
}

[[nodiscard]] std::optional<std::string> decodeResponseBody(
    const drogon::HttpResponsePtr& response)
{
    if (!response)
        return std::nullopt;

    auto body = std::string_view(response->body().data(), response->body().size());
    auto contentEncoding = response->getHeader("Content-Encoding");
    if (contentEncoding.empty())
        contentEncoding = response->getHeader("content-encoding");

    const auto headerSaysGzip = hasGzipContentEncoding(contentEncoding);
    const auto bodyLooksGzip = looksLikeGzip(body);
    if (headerSaysGzip && !bodyLooksGzip)
        return std::string(body);
    if (!headerSaysGzip && !bodyLooksGzip)
        return std::string(body);
    return gunzip(body);
}

[[nodiscard]] std::string fetchHttpTextOnce(std::string const& url)
{
    auto parsedUrl = splitHttpUrl(url);

    trantor::EventLoopThread loopThread("GeoJsonSourceHttpFetch");
    loopThread.run();

    auto client = drogon::HttpClient::newHttpClient(parsedUrl.origin, loopThread.getLoop());
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(parsedUrl.pathAndQuery);
    request->addHeader("Accept-Encoding", "gzip");

    auto [result, response] = client->sendRequest(request);
    if (result != drogon::ReqResult::Ok || !response) {
        raise(fmt::format(
            "Failed to fetch `{}`: [{}]",
            url,
            drogon::to_string_view(result)));
    }
    if (static_cast<int>(response->statusCode()) >= 300) {
        raise(fmt::format(
            "Failed to fetch `{}`: HTTP {}",
            url,
            static_cast<int>(response->statusCode())));
    }

    auto decodedBody = decodeResponseBody(response);
    if (!decodedBody)
        raise(fmt::format("Failed to decode response body from `{}`.", url));
    return *decodedBody;
}

[[nodiscard]] std::optional<DataSourceInfo> loadConfiguredDataSourceInfo(
    std::optional<nlohmann::json> const& inlineJson,
    std::string const& location,
    std::function<std::string(std::string const&)> const& fetchText = {})
{
    if (inlineJson && !location.empty()) {
        raise("`dataSourceInfoJson` and `dataSourceInfoLocation` cannot be used together.");
    }

    if (inlineJson) {
        return DataSourceInfo::fromJson(*inlineJson);
    }

    if (location.empty())
        return std::nullopt;

    nlohmann::json infoJson;
    if (looksLikeHttpUrl(location)) {
        auto body = fetchText ? fetchText(location) : fetchHttpTextOnce(location);
        infoJson = parseStructuredDocument(body, location);
    }
    else
        infoJson = loadStructuredDocumentFile(location);
    return DataSourceInfo::fromJson(infoJson);
}

void ensureFeatureLayerInfoOnly(DataSourceInfo const& info, std::string_view context)
{
    for (auto const& [layerId, layerInfo] : info.layers_) {
        if (layerInfo->type_ != LayerType::Features) {
            raise(fmt::format(
                "{} only supports feature layers, but `{}` is configured as type `{}`.",
                context,
                layerId,
                nlohmann::json(layerInfo->type_).dump()));
        }
    }
}

void finalizeLoadedInfo(DataSourceInfo& info, std::string const& mapIdOverride)
{
    ensureFeatureLayerInfoOnly(info, "GeoJSON datasource");
    info.maxParallelJobs_ = std::max(info.maxParallelJobs_, 1);
    if (info.nodeId_.empty())
        info.nodeId_ = generateNodeHexUuid();
    if (!mapIdOverride.empty())
        info.mapId_ = mapIdOverride;
}

[[nodiscard]] DataSourceInfo synthesizeFallbackInfo(std::string const& mapId)
{
    auto fallbackLayerJson = nlohmann::json::parse(R"json(
    {
      "featureTypes": [
        {
          "name": "AnyFeature",
          "uniqueIdCompositions": [
            [
              {"partId": "tileId", "datatype": "U64"},
              {"partId": "featureIndex", "datatype": "U32"}
            ]
          ]
        }
      ]
    })json");

    auto layerInfo = LayerInfo::fromJson(fallbackLayerJson, "GeoJsonAny");
    DataSourceInfo info;
    info.nodeId_ = generateNodeHexUuid();
    info.mapId_ = mapId;
    info.maxParallelJobs_ = defaultParallelJobs();
    info.layers_.emplace(layerInfo->layerId_, std::move(layerInfo));
    return info;
}

[[nodiscard]] std::string featureTypeNameForTile(const TileFeatureLayer::Ptr& tile)
{
    auto layerInfo = tile->layerInfo();
    if (!layerInfo->featureTypes_.empty())
        return layerInfo->featureTypes_.front().name_;

    if (layerInfo->layerId_ == "GeoJsonAny")
        return "AnyFeature";
    return layerInfo->layerId_ + "Feature";
}

void fillGeoJsonTile(
    const TileFeatureLayer::Ptr& tile,
    std::string const& geoJsonBody,
    bool withAttrLayers)
{
    tile->fromJson(
        nlohmann::json::parse(geoJsonBody),
        GeoJsonImportOptions{
            .strict_ = false,
            .fallbackFeatureType_ = featureTypeNameForTile(tile),
            .objectPropertiesAsAttributeLayers_ = withAttrLayers,
        });
}

}  // namespace

namespace mapget::geojsonsource
{

nlohmann::json GeoJsonSource::createLayerInfoJson(const std::string& layerName)
{
    std::string featureTypeName = layerName == "GeoJsonAny" ? "AnyFeature" : layerName + "Feature";
    return nlohmann::json::parse(fmt::format(R"json(
{{
  "featureTypes": [
    {{
      "name": "{}",
      "uniqueIdCompositions": [
        [
          {{
            "partId": "tileId",
            "description": "Mapget Tile ID.",
            "datatype": "U64"
          }},
          {{
            "partId": "featureIndex",
            "description": "Index of the feature within the GeoJSON collection.",
            "datatype": "U32"
          }}
        ]
      ]
    }}
  ]
}})json", featureTypeName));
}

bool GeoJsonSource::parseManifest()
{
    auto manifestPath = std::filesystem::path(inputDir_) / MANIFEST_FILENAME;
    if (!std::filesystem::exists(manifestPath))
        return false;

    try {
        auto manifestJson = loadStructuredDocumentFile(manifestPath.string());
        manifest_.version = manifestJson.value("version", 1);

        if (manifestJson.contains("metadata")) {
            auto& meta = manifestJson["metadata"];
            if (meta.contains("name"))
                manifest_.metadata.name = meta["name"].get<std::string>();
            if (meta.contains("description"))
                manifest_.metadata.description = meta["description"].get<std::string>();
            if (meta.contains("source"))
                manifest_.metadata.source = meta["source"].get<std::string>();
            if (meta.contains("created"))
                manifest_.metadata.created = meta["created"].get<std::string>();
            if (meta.contains("author"))
                manifest_.metadata.author = meta["author"].get<std::string>();
            if (meta.contains("license"))
                manifest_.metadata.license = meta["license"].get<std::string>();
        }

        if (manifestJson.contains("index")) {
            auto& index = manifestJson["index"];
            manifest_.defaultLayer = index.value("defaultLayer", "GeoJsonAny");

            if (index.contains("files")) {
                for (auto& [filename, fileInfo] : index["files"].items()) {
                    FileEntry entry;
                    entry.filename = filename;

                    if (fileInfo.is_object()) {
                        entry.tileId = fileInfo.value("tileId", uint64_t{0});
                        entry.layer = fileInfo.value("layer", std::string{});
                    }
                    else if (fileInfo.is_number()) {
                        entry.tileId = fileInfo.get<uint64_t>();
                    }
                    else {
                        mapget::log().warn(
                            "Invalid file entry in manifest for '{}': expected object or number",
                            filename);
                        continue;
                    }

                    if (entry.layer.empty())
                        entry.layer = manifest_.defaultLayer;

                    auto filePath = std::filesystem::path(inputDir_) / filename;
                    if (!std::filesystem::exists(filePath)) {
                        mapget::log().warn(
                            "File '{}' listed in manifest does not exist, skipping",
                            filename);
                        continue;
                    }

                    manifest_.files.push_back(std::move(entry));
                }
            }
        }

        mapget::log().info(
            "Loaded manifest.json with {} file entries",
            manifest_.files.size());
        return true;
    }
    catch (const std::exception& e) {
        mapget::log().error("Failed to parse manifest.json: {}", e.what());
        return false;
    }
}

void GeoJsonSource::initFromManifest()
{
    for (const auto& entry : manifest_.files) {
        layerCoverage_[entry.layer].insert(entry.tileId);
        tileLayerToFile_[{entry.tileId, entry.layer}] = entry.filename;
    }

    for (const auto& [layerName, tileIds] : layerCoverage_) {
        auto layerInfo = mapget::LayerInfo::fromJson(createLayerInfoJson(layerName), layerName);
        for (uint64_t tileId : tileIds) {
            layerInfo->coverage_.emplace_back(mapget::Coverage{tileId, tileId, {}});
        }
        info_.layers_.emplace(layerName, std::move(layerInfo));
    }
}

void GeoJsonSource::initFromDirectory()
{
    const std::string defaultLayer = "GeoJsonAny";
    auto layerInfo = mapget::LayerInfo::fromJson(createLayerInfoJson(defaultLayer), defaultLayer);

    for (const auto& file : std::filesystem::directory_iterator(inputDir_)) {
        if (file.path().extension() != ".geojson")
            continue;

        try {
            auto tileId = static_cast<uint64_t>(std::stoull(file.path().stem()));
            layerCoverage_[defaultLayer].insert(tileId);
            tileLayerToFile_[{tileId, defaultLayer}] = file.path().filename().string();
            layerInfo->coverage_.emplace_back(mapget::Coverage{tileId, tileId, {}});
        }
        catch (const std::exception&) {
            mapget::log().debug(
                "Skipping file '{}': filename is not a valid tile ID",
                file.path().filename().string());
        }
    }

    info_.layers_.emplace(defaultLayer, std::move(layerInfo));
}

GeoJsonSource::GeoJsonSource(
    const std::string& inputDir,
    bool withAttrLayers,
    const std::string& mapId)
    : GeoJsonSource(
        inputDir,
        GeoJsonSourceOptions{
            .withAttrLayers = withAttrLayers,
            .mapId = mapId})
{
}

GeoJsonSource::GeoJsonSource(std::string inputDir, GeoJsonSourceOptions options)
    : inputDir_(std::move(inputDir)),
      withAttrLayers_(options.withAttrLayers),
      tilePathTemplate_(options.tilePathTemplate.empty() ? "{tileId}.geojson" : options.tilePathTemplate)
{
    info_.maxParallelJobs_ = defaultParallelJobs();
    info_.mapId_ = options.mapId.empty() ? mapNameFromUri(inputDir_) : options.mapId;
    info_.nodeId_ = generateNodeHexUuid();

    if (auto loadedInfo = loadConfiguredDataSourceInfo(
            options.dataSourceInfoJson,
            options.dataSourceInfoLocation)) {
        info_ = std::move(*loadedInfo);
        finalizeLoadedInfo(info_, options.mapId);
        usesTemplatePaths_ = true;

        if (std::filesystem::exists(std::filesystem::path(inputDir_) / MANIFEST_FILENAME)) {
            mapget::log().info(
                "GeoJsonFolder is using explicit dataSourceInfo; manifest.json in '{}' is ignored.",
                inputDir_);
        }
    }
    else {
        if (!options.tilePathTemplate.empty()) {
            mapget::log().warn(
                "GeoJsonFolder ignores `tilePathTemplate` without explicit dataSourceInfo. "
                "Using manifest or legacy filename discovery instead.");
        }

        hasManifest_ = parseManifest();
        if (hasManifest_) {
            if (!manifest_.files.empty()) {
                initFromManifest();
            }
            else {
                mapget::log().info(
                    "manifest.json found but has no index/files section - no tiles available");
            }
        }
        else {
            mapget::log().warn(
                "No manifest.json found in '{}'. "
                "Using deprecated legacy mode with filename-based tile ID detection. "
                "Provide `dataSourceInfo` for template-based loading.",
                inputDir_);
            initFromDirectory();
        }
    }

    mapget::log().info(
        "GeoJsonFolder initialized: mapId='{}', layers={}, mode={}",
        info_.mapId_,
        info_.layers_.size(),
        usesTemplatePaths_ ? "template" : hasManifest_ ? "manifest" : "legacy");
}

mapget::DataSourceInfo GeoJsonSource::info()
{
    return info_;
}

std::string GeoJsonSource::resolveTilePath(uint64_t tileId, std::string_view layerId) const
{
    auto rendered = renderTemplate(tilePathTemplate_, tileId, layerId);
    std::filesystem::path path(rendered);
    if (path.is_absolute())
        return path.string();
    return (std::filesystem::path(inputDir_) / path).string();
}

std::string GeoJsonSource::readTileBody(uint64_t tileId, std::string_view layerId) const
{
    if (usesTemplatePaths_) {
        auto path = resolveTilePath(tileId, layerId);
        std::ifstream geojsonFile(path);
        if (!geojsonFile)
            raise(fmt::format("Failed to open GeoJSON file `{}`.", path));
        std::ostringstream buffer;
        buffer << geojsonFile.rdbuf();
        return buffer.str();
    }

    TileLayerKey key{tileId, std::string(layerId)};
    auto fileIt = tileLayerToFile_.find(key);
    if (fileIt == tileLayerToFile_.end()) {
        raise(fmt::format(
            "No GeoJSON file registered for tile {} in layer `{}`.",
            tileId,
            layerId));
    }

    auto path = (std::filesystem::path(inputDir_) / fileIt->second).string();
    std::ifstream geojsonFile(path);
    if (!geojsonFile)
        raise(fmt::format("Failed to open GeoJSON file `{}`.", path));

    std::ostringstream buffer;
    buffer << geojsonFile.rdbuf();
    return buffer.str();
}

void GeoJsonSource::fill(const mapget::TileFeatureLayer::Ptr& tile)
{
    try {
        fillGeoJsonTile(
            tile,
            readTileBody(tile->tileId().value_, tile->layerInfo()->layerId_),
            withAttrLayers_);
    }
    catch (const std::exception& e) {
        tile->setError(e.what());
        mapget::log().error(
            "GeoJsonFolder failed to fill tile {} for layer '{}': {}",
            tile->tileId().value_,
            tile->layerInfo()->layerId_,
            e.what());
    }
}

void GeoJsonSource::fill(mapget::TileSourceDataLayer::Ptr const&)
{
    // This datasource only serves feature tiles.
}

struct GeoJsonEndpointSource::Impl
{
    struct ClientPool
    {
        std::vector<drogon::HttpClientPtr> clients;
        std::size_t nextClient = 0;
    };

    explicit Impl(int clientCount)
        : clientCount_(std::max(clientCount, 1))
    {
        loopThread_ = std::make_unique<trantor::EventLoopThread>("GeoJsonEndpointSource");
        loopThread_->run();
    }

    [[nodiscard]] drogon::HttpClientPtr acquireClient(std::string const& origin)
    {
        std::lock_guard lock(mutex_);
        auto& pool = pools_[origin];
        if (pool.clients.empty()) {
            pool.clients.reserve(clientCount_);
            for (int i = 0; i < clientCount_; ++i) {
                pool.clients.push_back(drogon::HttpClient::newHttpClient(origin, loopThread_->getLoop()));
            }
        }
        auto client = pool.clients[pool.nextClient % pool.clients.size()];
        ++pool.nextClient;
        return client;
    }

    int clientCount_ = 1;
    std::unique_ptr<trantor::EventLoopThread> loopThread_;
    std::mutex mutex_;
    std::unordered_map<std::string, ClientPool> pools_;
};

GeoJsonEndpointSource::GeoJsonEndpointSource(GeoJsonEndpointSourceOptions options)
    : baseUrl_(trimmed(options.baseUrl)),
      tileUrlTemplate_(options.tileUrlTemplate.empty() ? "{tileId}.geojson" : options.tileUrlTemplate),
      withAttrLayers_(options.withAttrLayers),
      fetchText_(std::move(options.fetchText))
{
    if (baseUrl_.empty())
        raise("GeoJsonEndpoint requires a non-empty `baseUrl`.");

    info_.maxParallelJobs_ = defaultParallelJobs();
    info_.mapId_ = options.mapId.empty() ? mapNameFromUri(baseUrl_) : options.mapId;
    info_.nodeId_ = generateNodeHexUuid();

    if (auto loadedInfo = loadConfiguredDataSourceInfo(
            options.dataSourceInfoJson,
            options.dataSourceInfoLocation,
            fetchText_)) {
        info_ = std::move(*loadedInfo);
        finalizeLoadedInfo(info_, options.mapId);
    }
    else {
        info_ = synthesizeFallbackInfo(info_.mapId_);
        if (!options.mapId.empty())
            info_.mapId_ = options.mapId;

        mapget::log().warn(
            "No `dataSourceInfo` configured for GeoJsonEndpoint '{}'. "
            "Only `GeoJsonAny` will be advertised, coverage stays empty, "
            "and conversion will run in best-effort mode.",
            baseUrl_);
    }

    impl_ = std::make_unique<Impl>(std::max(info_.maxParallelJobs_, 1));

    mapget::log().info(
        "GeoJsonEndpoint initialized: mapId='{}', layers={}, baseUrl='{}'",
        info_.mapId_,
        info_.layers_.size(),
        baseUrl_);
}

GeoJsonEndpointSource::~GeoJsonEndpointSource() = default;

mapget::DataSourceInfo GeoJsonEndpointSource::info()
{
    return info_;
}

std::string GeoJsonEndpointSource::renderTileUrl(uint64_t tileId, std::string_view layerId) const
{
    auto rendered = renderTemplate(tileUrlTemplate_, tileId, layerId, baseUrl_);
    if (looksLikeHttpUrl(rendered))
        return rendered;
    return joinUrlPrefixAndPath(baseUrl_, rendered);
}

std::string GeoJsonEndpointSource::fetchTileBody(uint64_t tileId, std::string_view layerId) const
{
    auto url = renderTileUrl(tileId, layerId);
    if (fetchText_)
        return fetchText_(url);

    auto parsedUrl = splitHttpUrl(url);
    auto client = impl_->acquireClient(parsedUrl.origin);

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(parsedUrl.pathAndQuery);
    request->addHeader("Accept-Encoding", "gzip");

    auto [result, response] = client->sendRequest(request);
    if (result != drogon::ReqResult::Ok || !response) {
        raise(fmt::format(
            "Failed to fetch tile `{}` for layer `{}` from `{}`: [{}]",
            tileId,
            layerId,
            url,
            drogon::to_string_view(result)));
    }
    if (static_cast<int>(response->statusCode()) >= 300) {
        raise(fmt::format(
            "Failed to fetch tile `{}` for layer `{}` from `{}`: HTTP {}",
            tileId,
            layerId,
            url,
            static_cast<int>(response->statusCode())));
    }

    auto decodedBody = decodeResponseBody(response);
    if (!decodedBody) {
        raise(fmt::format(
            "Failed to decode tile response for `{}` / `{}` from `{}`.",
            tileId,
            layerId,
            url));
    }
    return *decodedBody;
}

void GeoJsonEndpointSource::fill(const mapget::TileFeatureLayer::Ptr& tile)
{
    try {
        fillGeoJsonTile(
            tile,
            fetchTileBody(tile->tileId().value_, tile->layerInfo()->layerId_),
            withAttrLayers_);
    }
    catch (const std::exception& e) {
        tile->setError(e.what());
        mapget::log().error(
            "GeoJsonEndpoint failed to fill tile {} for layer '{}': {}",
            tile->tileId().value_,
            tile->layerInfo()->layerId_,
            e.what());
    }
}

void GeoJsonEndpointSource::fill(mapget::TileSourceDataLayer::Ptr const&)
{
    // This datasource only serves feature tiles.
}

}  // namespace mapget::geojsonsource
