#include "cli.h"
#include "http-client.h"
#include "http-service.h"
#include "mapget/log.h"

#include "mapget/http-datasource/datasource-client.h"
#include "mapget/service/memcache.h"
#include "mapget/service/rocksdbcache.h"
#include "mapget/service/sqlitecache.h"
#include "mapget/service/config.h"

#include <CLI/CLI.hpp>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

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

std::filesystem::path getExecutablePath() {
    char buffer[1024];
#ifdef _WIN32
    GetModuleFileNameA(NULL, buffer, sizeof(buffer));
#elif __APPLE__
    uint32_t size = sizeof(buffer);
    _NSGetExecutablePath(buffer, &size);
#elif __linux__
    ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer));
    if (count != -1) {
        buffer[count] = '\0';
    }
#endif
    return std::filesystem::path(buffer).parent_path();
}

class ConfigYAML : public CLI::Config
{
public:
    std::string to_config(const CLI::App* app, bool defaultAlso, bool, std::string) const override
    {
        std::string config_path = app->get_config_ptr() ?
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
        });
    service.registerDataSourceType(
        "DataSourceProcess",
        [](YAML::Node const& config) -> DataSource::Ptr {
            if (auto cmd = config["cmd"])
                return std::make_shared<RemoteDataSourceProcess>(cmd.as<std::string>());
            else
                throw std::runtime_error("Missing `cmd` field.");
        });
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
    std::string webapp_;
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
#if defined(MAPGET_WITH_SQLITE) || defined(MAPGET_WITH_ROCKSDB)
            "From [memory|persistent], default memory. 'persistent' uses SQLite for disk-based caching."
#else
            "Cache type (only 'memory' is available, persistent caches disabled at compile time)."
#endif
            )
            ->default_val("memory");
#if defined(MAPGET_WITH_ROCKSDB) || defined(MAPGET_WITH_SQLITE)
        serveCmd->add_option(
            "--cache-dir", cachePath_, "Path to store persistent cache (SQLite DB file or RocksDB directory).")
            ->default_val("mapget-cache");
#endif
        serveCmd->add_option(
            "--cache-max-tiles", cacheMaxTiles_, "0 for unlimited, default 1024.")
            ->default_val(1024);
        serveCmd->add_option(
            "--clear-cache", clearCache_, "Clear existing persistent cache at startup.")
            ->default_val(false);
        serveCmd->add_option(
            "-w,--webapp",
            webapp_,
            "Serve a static web application, in the format [<url-scope>:]<filesystem-path>.");
        serveCmd->add_flag(
            "--allow-post-config",
            isPostConfigEndpointEnabled_,
            "Allow the POST /config endpoint.");
        serveCmd->add_flag(
            "--no-get-config",
            isGetConfigEndpointEnabled_,
            "Allow the GET /config endpoint.");
        serveCmd->callback([this]() { serve(); });
    }

    void serve()
    {
        log().info("Starting server on port {}.", port_);

        std::shared_ptr<Cache> cache;
        if (cacheType_ == "rocksdb") {
            log().warn("Cache type 'rocksdb' is no longer supported, falling back to 'persistent' option. "
                       "Please use '--cache-type persistent' in the future.");
            cacheType_ = "persistent";
        }
        
        if (cacheType_ == "persistent") {
#ifdef MAPGET_WITH_SQLITE
            log().info("Initializing persistent SQLite cache.");
            cache = std::make_shared<SQLiteCache>(cacheMaxTiles_, cachePath_, clearCache_);
#elif MAPGET_WITH_ROCKSDB
            log().info("Initializing persistent RocksDB cache (SQLite not available).");
            cache = std::make_shared<RocksDBCache>(cacheMaxTiles_, cachePath_, clearCache_);
#else
            raise("Persistent cache was requested but neither SQLite nor RocksDB support was enabled at compile time.");
#endif
        }
        else if (cacheType_ == "memory") {
            log().info("Initializing in-memory cache.");
            cache = std::make_shared<MemCache>(cacheMaxTiles_);
        }
        else {
            raise(fmt::format("Cache type {} not supported!", cacheType_));
        }

        auto config = app_.get_config_ptr();
        bool watchConfig = config != nullptr;

        // HttpService will subscribe to DataSourceConfigService.
        HttpService srv(cache, watchConfig);

        if (config)
        {
            registerDefaultDatasourceTypes();
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
                exit(1);
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

    explicit FetchCommand(CLI::App& app)
    {
        auto fetchCmd = app.add_subcommand("fetch", "Connects to the server to fetch tiles.");
        fetchCmd->add_option("-s,--server", server_, "Server to connect to in format <host:port>.")
            ->required();
        fetchCmd->add_option("-m,--map", map_, "Map to retrieve.")->required();
        fetchCmd->add_option("-l,--layer", layer_, "Layer of the map to retrieve.")->required();
        fetchCmd->add_option("--mute",
            mute_, "Mute the actual tile GeoJSON output.");
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

        mapget::HttpClient cli(host, port);
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

std::string pathToSchema;
int runFromCommandLine(std::vector<std::string> args, bool requireSubcommand)
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
        "Optional path to a file with configuration schema for mapget.")
        ->default_val((getExecutablePath() / "default_config_schema.json").string()); // TODO: Add a test
    app.config_formatter(std::make_shared<ConfigYAML>());

    if (requireSubcommand)
        app.require_subcommand(1);

    if (!log_level_.empty()) {
        mapget::setLogLevel(log_level_, log());
    }

    ServeCommand serveCommand(app);
    FetchCommand fetchCommand(app);

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

const std::string &getPathToSchema()
{
    return pathToSchema;
}

void setPathToSchema(const std::string& path)
{
    pathToSchema = path;
}

}  // namespace mapget
