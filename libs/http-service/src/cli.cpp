#include "cli.h"
#include "http-client.h"
#include "http-service.h"
#include "mapget/log.h"

#include "mapget/http-datasource/datasource-client.h"
#include "mapget/service/memcache.h"
#include "mapget/service/nullcache.h"
#include "mapget/service/sqlitecache.h"
#include "mapget/service/config.h"

#include "gridsource/gridsource.h"
#include "geojsonsource/geojsonsource.h"

#include <CLI/CLI.hpp>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <chrono>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#elif __linux__
#include <unistd.h>
#endif

namespace mapget
{

namespace
{

nlohmann::json dataSourceHostSchema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"url", {
                {"type", "string"},
                {"title", "URL"},
                {"description", "Host:port for the remote datasource server."}
            }}
        }},
        {"required", nlohmann::json::array({"url"})},
        {"additionalProperties", false}
    };
}

nlohmann::json dataSourceProcessSchema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"cmd", {
                {"type", "string"},
                {"title", "Command"},
                {"description", "Command line to start the datasource process."}
            }}
        }},
        {"required", nlohmann::json::array({"cmd"})},
        {"additionalProperties", false}
    };
}

nlohmann::json gridDataSourceSchema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"mapId", {{"type", "string"}, {"title", "Map ID"}}},
            {"spatialCoherence", {{"type", "boolean"}}},
            {"collisionGridSize", {{"type", "number"}}},
            {"layers", {{"type", "array"}}}
        }},
        {"additionalProperties", true}
    };
}

nlohmann::json geoJsonFolderSchema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"folder", {
                {"type", "string"},
                {"title", "Folder"},
                {"description", "Path to a folder containing GeoJSON tiles."}
            }},
            {"mapId", {
                {"type", "string"},
                {"title", "Map ID"},
                {"description", "Custom map identifier. If not provided, derived from folder path."}
            }},
            {"withAttrLayers", {
                {"type", "boolean"},
                {"title", "With Attribute Layers"},
                {"description", "Convert nested GeoJSON property objects to mapget attribute layers. Default: true."},
                {"default", true}
            }},
            {"tilePathTemplate", {
                {"type", "string"},
                {"title", "Tile Path Template"},
                {"description", "Relative path template used with explicit dataSourceInfo, e.g. `{layerId}/{tileId}.geojson`."}
            }},
            {"dataSourceInfo", {
                {"title", "Datasource Info"},
                {"description", "Inline datasource info object, local YAML/JSON file path, or HTTP(S) URL."},
                {"oneOf", nlohmann::json::array({
                    nlohmann::json{{"type", "string"}},
                    nlohmann::json{{"type", "object"}}
                })}
            }}
        }},
        {"required", nlohmann::json::array({"folder"})},
        {"additionalProperties", false}
    };
}

nlohmann::json geoJsonEndpointSchema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"baseUrl", {
                {"type", "string"},
                {"title", "Base URL"},
                {"description", "Base HTTP(S) URL used to fetch GeoJSON tiles."}
            }},
            {"mapId", {
                {"type", "string"},
                {"title", "Map ID"},
                {"description", "Custom map identifier. If not provided, derived from the baseUrl."}
            }},
            {"withAttrLayers", {
                {"type", "boolean"},
                {"title", "With Attribute Layers"},
                {"description", "Convert nested GeoJSON property objects to mapget attribute layers. Default: true."},
                {"default", true}
            }},
            {"tileUrlTemplate", {
                {"type", "string"},
                {"title", "Tile URL Template"},
                {"description", "URL or relative path template used to fetch tiles, e.g. `{layerId}/{tileId}.geojson`."}
            }},
            {"dataSourceInfo", {
                {"title", "Datasource Info"},
                {"description", "Inline datasource info object, local YAML/JSON file path, or HTTP(S) URL."},
                {"oneOf", nlohmann::json::array({
                    nlohmann::json{{"type", "string"}},
                    nlohmann::json{{"type", "object"}}
                })}
            }}
        }},
        {"required", nlohmann::json::array({"baseUrl"})},
        {"additionalProperties", false}
    };
}

[[nodiscard]] bool looksLikeHttpUrl(std::string_view value)
{
    static constexpr char cleartextScheme[] = {'h', 't', 't', 'p', '\0'};
    static constexpr char tlsScheme[] = {'h', 't', 't', 'p', 's', '\0'};
    constexpr std::string_view delimiter = "://";
    auto hasScheme = [&](std::string_view scheme) {
        return value.size() > scheme.size() + delimiter.size() &&
               value.compare(0, scheme.size(), scheme) == 0 &&
               value.compare(scheme.size(), delimiter.size(), delimiter) == 0;
    };
    return hasScheme({cleartextScheme, 4}) || hasScheme({tlsScheme, 5});
}

[[nodiscard]] std::string trimCopy(std::string value)
{
    auto isWhitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
        return !isWhitespace(static_cast<unsigned char>(ch));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
        return !isWhitespace(static_cast<unsigned char>(ch));
    }).base(), value.end());
    return value;
}

[[nodiscard]] std::filesystem::path configDirectory()
{
    if (auto configPath = DataSourceConfigService::get().getConfigFilePath()) {
        return std::filesystem::path(*configPath).parent_path();
    }
    return std::filesystem::current_path();
}

void applyStructuredDocumentOption(
    const YAML::Node& config,
    std::optional<nlohmann::json>& inlineJsonOut,
    std::string& locationOut)
{
    if (!config) {
        return;
    }

    if (config.IsMap() || config.IsSequence()) {
        inlineJsonOut = yamlToJson(config, false);
        return;
    }

    if (!config.IsScalar()) {
        throw std::runtime_error("`dataSourceInfo` must be a mapping or scalar string.");
    }

    auto value = trimCopy(config.as<std::string>());
    if (value.empty()) {
        return;
    }

    if (value.front() == '{' || value.front() == '[') {
        inlineJsonOut = parseStructuredDocument(value, "inline dataSourceInfo");
        return;
    }

    if (looksLikeHttpUrl(value)) {
        locationOut = value;
        return;
    }

    auto path = std::filesystem::path(value);
    if (path.is_relative())
        path = configDirectory() / path;
    locationOut = path.lexically_normal().string();
}

[[nodiscard]] geojsonsource::GeoJsonSourceOptions makeGeoJsonFolderOptions(YAML::Node const& config)
{
    geojsonsource::GeoJsonSourceOptions options;
    if (auto withAttributeLayersNode = config["withAttrLayers"])
        options.withAttrLayers = withAttributeLayersNode.as<bool>();
    if (auto mapIdNode = config["mapId"])
        options.mapId = mapIdNode.as<std::string>();
    if (auto tilePathTemplateNode = config["tilePathTemplate"])
        options.tilePathTemplate = tilePathTemplateNode.as<std::string>();
    applyStructuredDocumentOption(
        config["dataSourceInfo"],
        options.dataSourceInfoJson,
        options.dataSourceInfoLocation);
    return options;
}

[[nodiscard]] geojsonsource::GeoJsonEndpointSourceOptions makeGeoJsonEndpointOptions(YAML::Node const& config)
{
    geojsonsource::GeoJsonEndpointSourceOptions options;
    if (auto baseUrlNode = config["baseUrl"])
        options.baseUrl = baseUrlNode.as<std::string>();
    if (auto withAttributeLayersNode = config["withAttrLayers"])
        options.withAttrLayers = withAttributeLayersNode.as<bool>();
    if (auto mapIdNode = config["mapId"])
        options.mapId = mapIdNode.as<std::string>();
    if (auto tileUrlTemplateNode = config["tileUrlTemplate"])
        options.tileUrlTemplate = tileUrlTemplateNode.as<std::string>();
    applyStructuredDocumentOption(
        config["dataSourceInfo"],
        options.dataSourceInfoJson,
        options.dataSourceInfoLocation);
    return options;
}

class ConfigYAML : public CLI::Config
{
public:
    std::string to_config(const CLI::App* app, bool defaultAlso, bool, std::string) const override
    {
        std::string config_path = app->get_config_ptr() && *app->get_config_ptr() ?
            app->get_config_ptr()->as<std::string>() :
            "config.yaml";
        std::ifstream ifs(config_path);
        YAML::Node root = ifs ? YAML::Load(ifs) : YAML::Node();

        // Create or clear the 'mapget' node
        auto mapgetNode = root["mapget"] = YAML::Node(YAML::NodeType::Map);

        // Process current app configuration into 'mapget' node
        toYaml(mapgetNode, app, defaultAlso);

        // Output the YAML content as a formatted string
        std::stringstream ss;
        ss << root;
        return ss.str();
    }

    void toYaml(YAML::Node root, const CLI::App* app, bool defaultAlso) const
    {
        for (const CLI::Option* opt : app->get_options({})) {
            if (!opt->get_lnames().empty() && opt->get_configurable()) {
                std::string name = opt->get_lnames()[0];

                if (opt->get_type_size() != 0) {
                    if (opt->count() == 1)
                        root[name] = opt->results().at(0);
                    else if (opt->count() > 0)
                        root[name] = opt->results();
                    else if (defaultAlso && !opt->get_default_str().empty())
                        root[name] = opt->get_default_str();
                }
                else if (opt->count()) {
                    root[name] = opt->count() > 1 ? YAML::Node(opt->count()) : YAML::Node(true);
                }
                else {
                    root[name] = defaultAlso ? YAML::Node(false) : YAML::Node();
                }
            }
        }

        for (const CLI::App* subcom : app->get_subcommands({}))
            toYaml(root[subcom->get_name()], subcom, defaultAlso);
    }

    std::vector<CLI::ConfigItem> from_config(std::istream& input) const override
    {
        try {
            YAML::Node root = YAML::Load(input);
            YAML::Node mapgetNode = root["mapget"];
            return mapgetNode ? fromYaml(mapgetNode) : std::vector<CLI::ConfigItem>();
        }
        catch (YAML::ParserException const& e) {
            raise(fmt::format("Failed to parse config file! Error: {}", e.what()));
        }
    }

    [[nodiscard]] std::vector<CLI::ConfigItem> fromYaml(
        const YAML::Node& node,
        const std::string& name = "",
        const std::vector<std::string>& prefix = {}) const
    {
        std::vector<CLI::ConfigItem> results;

        if (node.IsMap()) {
            for (const auto& item : node) {
                auto copy_prefix = prefix;
                if (!name.empty()) {
                    copy_prefix.push_back(name);
                }
                auto sub_results = fromYaml(item.second, item.first.as<std::string>(), copy_prefix);
                results.insert(results.end(), sub_results.begin(), sub_results.end());
            }
        }
        else if (!name.empty()) {
            CLI::ConfigItem& res = results.emplace_back();
            res.name = name;
            res.parents = prefix;
            if (node.IsScalar()) {
                res.inputs = {node.as<std::string>()};
            }
            else if (node.IsSequence()) {
                for (const auto& val : node) {
                    res.inputs.push_back(val.as<std::string>());
                }
            }
        }

        return results;
    }
};

void registerDefaultDatasourceTypes() {
    auto& service = DataSourceConfigService::get();
    service.registerDataSourceType(
        "DataSourceHost",
        [](YAML::Node const& config) -> DataSource::Ptr {
            if (auto url = config["url"])
                return RemoteDataSource::fromHostPort(url.as<std::string>());
            else
                throw std::runtime_error("Missing `url` field.");
        },
        dataSourceHostSchema());
    service.registerDataSourceType(
        "DataSourceProcess",
        [](YAML::Node const& config) -> DataSource::Ptr {
            if (auto cmd = config["cmd"])
                return std::make_shared<RemoteDataSourceProcess>(cmd.as<std::string>());
            else
                throw std::runtime_error("Missing `cmd` field.");
        },
        dataSourceProcessSchema());
    service.registerDataSourceType(
        "GridDataSource",
        [](YAML::Node const& config) -> DataSource::Ptr { return std::make_shared<gridsource::GridDataSource>(config); },
        gridDataSourceSchema());
    service.registerDataSourceType(
        "GeoJsonFolder",
        [](YAML::Node const& config) -> DataSource::Ptr {
            if (auto folder = config["folder"]) {
                return std::make_shared<geojsonsource::GeoJsonSource>(
                    folder.as<std::string>(),
                    makeGeoJsonFolderOptions(config));
            }
            throw std::runtime_error("Missing `folder` field.");
        },
        geoJsonFolderSchema());
    service.registerDataSourceType(
        "GeoJsonEndpoint",
        [](YAML::Node const& config) -> DataSource::Ptr {
            auto options = makeGeoJsonEndpointOptions(config);
            if (options.baseUrl.empty())
                throw std::runtime_error("Missing `baseUrl` field.");
            return std::make_shared<geojsonsource::GeoJsonEndpointSource>(std::move(options));
        },
        geoJsonEndpointSchema());
}

void loadConfigSchemaPatch(const std::string& schemaPath)
{
    namespace fs = std::filesystem;
    try {
        if (fs::exists(schemaPath)) {
            std::ifstream in(schemaPath);
            nlohmann::json extra;
            in >> extra;
            DataSourceConfigService::get().setDataSourceConfigSchemaPatch(extra);
        }
    }
    catch (const std::exception& e) {
        log().warn("Failed to initialize schema: {}", e.what());
    }
}

bool isPostConfigEndpointEnabled_ = false;
bool isGetConfigEndpointEnabled_ = true;
}

struct ServeCommand
{
    int port_ = 0;
    std::vector<std::string> datasourceHosts_;
    std::vector<std::string> datasourceExecutables_;
    std::string cacheType_;
    std::string cachePath_;
    int64_t cacheMaxTiles_ = 1024;
    bool clearCache_ = false;
    bool allowPostConfigEndpoint_ = false;
    bool noGetConfigEndpoint_ = false;
    std::string webapp_;
    std::vector<std::string> staticMounts_;
    int64_t ttlSeconds_ = 0;
    uint64_t memoryTrimIntervalBinary_ = HttpServiceConfig{}.memoryTrimIntervalBinary;  // Use default from config
    uint64_t memoryTrimIntervalJson_ = HttpServiceConfig{}.memoryTrimIntervalJson;      // Use default from config
    bool noLocationDb_ = false;
    std::string locationDbPath_;
    int64_t locationMaxLimit_ = HttpServiceConfig{}.locationResultMaxLimit;
    CLI::App& app_;

    explicit ServeCommand(CLI::App& app) : app_(app)
    {
        auto serveCmd = app.add_subcommand("serve", "Starts the server.");
        serveCmd->add_option(
            "-p,--port",
            port_,
            "Port to start the server on. Default is 0.")
            ->default_val("0");
        CLI::deprecate_option(serveCmd->add_option(
            "-d,--datasource-host",
            datasourceHosts_,
            "This option is deprecated. Use a config file instead!. "
            "Data sources in format <host:port>. Can be specified multiple times."),
            "--config <yaml-file>");
        CLI::deprecate_option(serveCmd->add_option(
            "-e,--datasource-exe",
            datasourceExecutables_,
            "This option is deprecated. Use a config file instead!. "
            "Data source executable paths, including arguments. "
            "Can be specified multiple times."),
            "--config <yaml-file>");
        serveCmd->add_option(
            "-c,--cache-type", cacheType_, 
            "From [memory|persistent|none], default memory. 'persistent' uses SQLite for disk-based caching, 'none' disables caching."
            )
            ->default_val("memory");
        serveCmd->add_option(
            "--cache-dir", cachePath_, "Path to store persistent cache (SQLite DB file).")
            ->default_val("mapget-cache");
        serveCmd->add_option(
            "--cache-max-tiles", cacheMaxTiles_, "0 for unlimited, default 1024.")
            ->default_val(1024);
        serveCmd->add_option(
            "--clear-cache", clearCache_, "Clear existing persistent cache at startup.")
            ->default_val(false);
        serveCmd->add_option(
            "--ttl",
            ttlSeconds_,
            "Default TTL for cached tiles in seconds (0 = infinite).")
            ->default_val(ttlSeconds_);
        serveCmd->add_option(
            "-w,--webapp",
            webapp_,
            "Serve a static web application, in the format [<url-scope>:]<filesystem-path>.");
        serveCmd->add_option(
            "--static-mount",
            staticMounts_,
            "Serve an additional static filesystem mount, in the format [<url-scope>:]<filesystem-path>. Can be specified multiple times.");
        serveCmd->add_flag(
            "--allow-post-config",
            allowPostConfigEndpoint_,
            "Allow the POST /config endpoint.");
        serveCmd->add_flag(
            "--no-get-config",
            noGetConfigEndpoint_,
            "Disable the GET /config datasource model endpoint.");
        serveCmd->add_option(
            "--memory-trim-binary-interval",
            memoryTrimIntervalBinary_,
            "Number of processed binary requests between explicit memory trimming to return unused memory to OS "
            "(0=disabled, 1=after every request, N=after every N binary requests). "
            "Only effective on platforms supporting allocator trimming (e.g., Linux).")
            ->default_val(memoryTrimIntervalBinary_);
        serveCmd->add_option(
            "--memory-trim-json-interval",
            memoryTrimIntervalJson_,
            "Number of processed JSON/GeoJSON requests between explicit memory trimming to return unused memory to OS "
            "(0=disabled, 1=after every request, N=after every N JSON requests). "
            "Only effective on platforms supporting allocator trimming (e.g., Linux).")
            ->default_val(memoryTrimIntervalJson_);
        serveCmd->add_option(
            "--location-db",
            locationDbPath_,
            "Path to the SQLite location database. Defaults to geonames-cities1000.sqlite next to the executable.");
        serveCmd->add_option(
            "--location-max-limit",
            locationMaxLimit_,
            "Maximum accepted /location result limit. Default 50.")
            ->default_val(locationMaxLimit_);
        serveCmd->add_flag(
            "--no-location-db",
            noLocationDb_,
            "Disable the /location endpoint.");
        serveCmd->callback([this]() { serve(); });
    }

    void serve()
    {
        if (ttlSeconds_ < 0) {
            raise("TTL must not be negative.");
        }
        if (locationMaxLimit_ < 1) {
            raise("Location max limit must be at least 1.");
        }
        setPostConfigEndpointEnabled(allowPostConfigEndpoint_);
        setGetConfigEndpointEnabled(!noGetConfigEndpoint_);
        log().info("Starting server on port {}.", port_);

        std::shared_ptr<Cache> cache;
        if (cacheType_ == "rocksdb") {
            log().warn("RocksDB cache support has been removed. Please use '--cache-type persistent' instead, "
                       "which now uses SQLite for persistent caching. The '--cache-type rocksdb' option will be "
                       "removed in a future version. Falling back to persistent cache using SQLite.");
            cacheType_ = "persistent";
        }
        
        if (cacheType_ == "persistent") {
            log().info("Initializing persistent SQLite cache.");
            cache = std::make_shared<SQLiteCache>(cacheMaxTiles_, cachePath_, clearCache_);
        }
        else if (cacheType_ == "memory") {
            log().info("Initializing in-memory cache.");
            cache = std::make_shared<MemCache>(cacheMaxTiles_);
        }
        else if (cacheType_ == "none") {
            log().info("Running without cache - all requests will go directly to data sources.");
            cache = std::make_shared<NullCache>();
        }
        else {
            raise(fmt::format("Cache type {} not supported!", cacheType_));
        }

        auto config = app_.get_config_ptr();
        
        // Build HttpServiceConfig
        HttpServiceConfig httpConfig;
        httpConfig.watchConfig = config && *config;
        httpConfig.defaultTtl = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(ttlSeconds_));
        httpConfig.memoryTrimIntervalBinary = memoryTrimIntervalBinary_;
        httpConfig.memoryTrimIntervalJson = memoryTrimIntervalJson_;
        httpConfig.locationLookupEnabled = !noLocationDb_;
        httpConfig.locationResultMaxLimit = static_cast<uint32_t>(locationMaxLimit_);
        if (!locationDbPath_.empty()) {
            httpConfig.locationDatabasePath = std::filesystem::path(locationDbPath_);
        }
        
        // Log memory trim configuration
        bool anyTrimEnabled = (memoryTrimIntervalBinary_ > 0) || (memoryTrimIntervalJson_ > 0);
        if (anyTrimEnabled) {
#ifdef __linux__
            if (memoryTrimIntervalBinary_ > 0)
                log().info("Memory trim for binary responses: every {} requests", memoryTrimIntervalBinary_);
            else
                log().info("Memory trim for binary responses: disabled");
                
            if (memoryTrimIntervalJson_ > 0)
                log().info("Memory trim for JSON responses: every {} requests", memoryTrimIntervalJson_);
            else
                log().info("Memory trim for JSON responses: disabled");
#else
            log().warn("Memory trim intervals set (binary: {}, JSON: {}), but memory trimming is currently only supported on Linux. Settings will be ignored.", 
                      memoryTrimIntervalBinary_, memoryTrimIntervalJson_);
#endif
        } else {
            log().info("Memory trimming disabled for all response types");
        }

        // HttpService will subscribe to DataSourceConfigService.
        HttpService srv(cache, httpConfig);

        if (config && *config)
        {
            registerDefaultDatasourceTypes();
            loadConfigSchemaPatch(getPathToSchemaPatch());
            DataSourceConfigService::get().loadConfig(config->as<std::string>());
        }

        if (!datasourceHosts_.empty()) {
            for (auto& ds : datasourceHosts_) {
                try {
                    srv.add(RemoteDataSource::fromHostPort(ds));
                }
                catch (std::exception const& e) {
                    log().error("  ...failed: {}", e.what());
                }
            }
        }

        if (!datasourceExecutables_.empty()) {
            for (auto& ds : datasourceExecutables_) {
                log().info("Launching datasource exe: {}", ds);
                try {
                    srv.add(std::make_shared<RemoteDataSourceProcess>(ds));
                }
                catch (std::exception const& e) {
                    log().error("  ...failed: {}", e.what());
                }
            }
        }

        if (!webapp_.empty()) {
            log().info("Webapp: {}", webapp_);
            if (!srv.mountFileSystem(webapp_)) {
                log().error("  ...failed to mount!");
                raise("Failed to mount webapp filesystem path.");
            }
        }

        if (!staticMounts_.empty()) {
            for (auto const& staticMount : staticMounts_) {
                log().info("Static mount: {}", staticMount);
                if (!srv.mountFileSystem(staticMount)) {
                    log().error("  ...failed to mount!");
                    raise("Failed to mount static filesystem path.");
                }
            }
        }

        srv.go("0.0.0.0", port_);
        srv.waitForSignal();
    }
};

struct FetchCommand
{
    std::string server_, map_, layer_;
    std::vector<uint64_t> tiles_;
    bool mute_ = false;
    bool noCompression_ = false;

    explicit FetchCommand(CLI::App& app)
    {
        auto fetchCmd = app.add_subcommand("fetch", "Connects to the server to fetch tiles.");
        fetchCmd->add_option("-s,--server", server_, "Server to connect to in format <host:port>.")
            ->required();
        fetchCmd->add_option("-m,--map", map_, "Map to retrieve.")->required();
        fetchCmd->add_option("-l,--layer", layer_, "Layer of the map to retrieve.")->required();
        fetchCmd->add_option("--mute",
            mute_, "Mute the actual tile GeoJSON output.");
        fetchCmd->add_option("--no-compression",
            noCompression_, "Disable gzip compression for responses.");
        fetchCmd
            ->add_option(
                "-t,--tile",
                tiles_,
                "Tile of the map to retrieve. Can be specified multiple times.")
            ->required();
        fetchCmd->callback([this]() { fetch(); });
    }

    void fetch()
    {
        if (log().level() <= spdlog::level::debug) {
            // Skips building the tile list string if it will not be logged.
            std::string tileList;
            for (auto& tile : tiles_) {
                tileList += std::to_string(tile) + " ";
            }
            log().debug(
                "Connecting client to server {} for map {} and layer {} with tiles: {}",
                server_,
                map_,
                layer_,
                tileList);
        }

        auto delimiterPos = server_.find(':');
        std::string host = server_.substr(0, delimiterPos);
        int port = std::stoi(server_.substr(delimiterPos + 1, server_.size()));

        mapget::HttpClient cli(host, port, {}, !noCompression_);
        auto request = std::make_shared<LayerTilesRequest>(
            map_,
            layer_,
            std::vector<TileId>{tiles_.begin(), tiles_.end()});
        auto fn = [this](auto const& tile)
        {
            if (!mute_)
                std::cout << tile->toJson().dump() << std::endl;
            if (tile->error())
                raise(fmt::format("Tile {}: {}",
                                  tile->id().toString(), *tile->error()));
        };
        request->onFeatureLayer(fn);
        request->onSourceDataLayer(fn);
        cli.request(request)->wait();

        if (request->getStatus() == RequestStatus::NoDataSource)
            raise("Failed to fetch sources: no matching data source.");
        if (request->getStatus() == RequestStatus::Aborted)
            raise("Failed to fetch sources: request aborted.");
    }
};

std::string pathToSchema = "";
int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand, std::function<void(CLI::App&)> additionalCommandLineSetupFun)
{
    CLI::App app{"A client/server application for map data retrieval."};
    std::string log_level_;

    app.add_option(
        "--log-level",
        log_level_,
        "From [trace|debug|info|warn|error|critical], overrides MAPGET_LOG_LEVEL.")
        ->default_val("");
    app.set_config(
        "--config",
        "",
        "Optional path to a file with configuration arguments for mapget.");
    app.add_option(
        "--config-schema",
        pathToSchema,
        "Optional path to a file with configuration schema amendments for mapget.");
    app.config_formatter(std::make_shared<ConfigYAML>());

    if (requireSubcommand)
        app.require_subcommand(1);

    if (!log_level_.empty()) {
        mapget::setLogLevel(log_level_, log());
    }

    ServeCommand serveCommand(app);
    FetchCommand fetchCommand(app);

    if (additionalCommandLineSetupFun) {
        additionalCommandLineSetupFun(app);
    }

    try {
        std::reverse(args.begin(), args.end());
        app.parse(std::move(args));
    }
    catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    catch (std::runtime_error const& e) {
        return 1;
    }
    return 0;
}

bool isPostConfigEndpointEnabled()
{
    return isPostConfigEndpointEnabled_;
}

bool isGetConfigEndpointEnabled()
{
    return isGetConfigEndpointEnabled_;
}

void setPostConfigEndpointEnabled(bool enabled)
{
    isPostConfigEndpointEnabled_ = enabled;
}

void setGetConfigEndpointEnabled(bool enabled)
{
    isGetConfigEndpointEnabled_ = enabled;
}

const std::string &getPathToSchemaPatch()
{
    return pathToSchema;
}

void setPathToSchema(const std::string& path)
{
    pathToSchema = path;
}

}  // namespace mapget
