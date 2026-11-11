#include <atomic>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#include "mapget/http-service/cli.h"
#include "mapget/log.h"
#include "mapget/model/featurelayer.h"
#include "mapget/service/config.h"
#include "mapget/service/datasource.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "utility.h"

namespace fs = std::filesystem;
using namespace mapget;

TEST_CASE("Temporary config directories are private and exclusively owned", "[DataSourceConfig][temporary-directory]")
{
    auto root = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_private");
    {
        test::TemporaryDirectory directory(root);
        REQUIRE(fs::is_directory(root));
#ifdef _WIN32
        DWORD bytes = 0;
        GetFileSecurityW(root.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes);
        REQUIRE(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
        std::vector<unsigned char> descriptor(bytes);
        REQUIRE(GetFileSecurityW(
            root.c_str(),
            DACL_SECURITY_INFORMATION,
            descriptor.data(),
            bytes,
            &bytes));
        SECURITY_DESCRIPTOR_CONTROL control;
        DWORD revision;
        REQUIRE(GetSecurityDescriptorControl(descriptor.data(), &control, &revision));
        CHECK((control & SE_DACL_PROTECTED) != 0);
        BOOL present, defaulted;
        PACL acl = nullptr;
        REQUIRE(GetSecurityDescriptorDacl(descriptor.data(), &present, &acl, &defaulted));
        REQUIRE(present);
        REQUIRE(acl);
        REQUIRE(acl->AceCount == 1);
        void* entry = nullptr;
        REQUIRE(GetAce(acl, 0, &entry));
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        REQUIRE(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
        CHECK(ace->Mask == FILE_ALL_ACCESS);
        CHECK(ace->Header.AceFlags == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE));
        LPWSTR sid = nullptr;
        REQUIRE(ConvertSidToStringSidW(&ace->SidStart, &sid));
        auto ownerRights = std::wstring(sid) == L"S-1-3-4";
        LocalFree(sid);
        CHECK(ownerRights);
#else
        CHECK((fs::status(root).permissions() & fs::perms::all) == fs::perms::owner_all);
#endif

        auto sentinel = root / "sentinel";
        std::ofstream(sentinel) << "must survive rejected adoption";
        CHECK_THROWS_AS(test::TemporaryDirectory{root}, std::system_error);
        CHECK_THROWS_AS(test::TemporaryDirectory{sentinel}, std::system_error);
        CHECK(fs::is_regular_file(sentinel));
        std::ifstream contents(sentinel);
        std::string line;
        std::getline(contents, line);
        CHECK(line == "must survive rejected adoption");

#ifndef _WIN32
        // Symlink creation requires special privileges on Windows runners.
        auto link = root / "link";
        fs::create_directory_symlink(root, link);
        CHECK_THROWS_AS(test::TemporaryDirectory{link}, std::system_error);
        CHECK(fs::is_symlink(link));
        CHECK(fs::exists(sentinel));
#endif
    }
    CHECK_FALSE(fs::exists(root));

    CHECK_THROWS_AS(
        [&root]
        {
            test::TemporaryDirectory directory(root);
            std::ofstream(root / "config.yaml") << "sources: []";
            throw std::runtime_error("fixture construction failed");
        }(),
        std::runtime_error);
    CHECK_FALSE(fs::exists(root));
}

struct TestDataSource : public DataSource
{
    /** Resource whose release makes polymorphic destruction observable. */
    std::shared_ptr<int> retainedResource;

    DataSourceInfo info() override
    {
        return DataSourceInfo::fromJson(R"(
        {
            "mapId": "Catan",
            "layers": {}
        }
        )"_json);
    };

    void fill(PartitionFeatureLayer::Ptr const&) override{};
    void fill(PartitionSourceDataLayer::Ptr const&) override{};
};

TEST_CASE("DataSource destruction releases derived resources", "[DataSource]")
{
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> observer = resource;
    auto derived = std::make_unique<TestDataSource>();
    derived->retainedResource = std::move(resource);
    std::unique_ptr<DataSource> source = std::move(derived);
    REQUIRE_FALSE(observer.expired());
    source.reset();
    REQUIRE(observer.expired());
}

struct NamedTestDataSource : public DataSource
{
    explicit NamedTestDataSource(std::string mapId) : mapId_(std::move(mapId)) {}

    DataSourceInfo info() override
    {
        return DataSourceInfo::fromJson(nlohmann::json::object({
            {"mapId", mapId_},
            {"layers", nlohmann::json::object()},
        }));
    }

    void fill(PartitionFeatureLayer::Ptr const&) override{};
    void fill(PartitionSourceDataLayer::Ptr const&) override{};

    std::string mapId_;
};

struct MemoryReportingDataSource : public NamedTestDataSource
{
    MemoryReportingDataSource()
        : NamedTestDataSource("MemoryReportingMap")
    {}

    [[nodiscard]] std::optional<uint64_t> estimatedRetainedMemoryBytes() const override
    {
        return 123456;
    }
};

void syncFile(const fs::path& path)
{
    log().trace("Syncing file: {}", path.string());
#ifdef _WIN32
    // Windows doesn't have fsync, but closing the file should be sufficient
#else
    // On Unix systems, use fsync to ensure data is written to disk
    int fd = open(path.c_str(), O_RDONLY);
    if (fd != -1) {
        fsync(fd);
        close(fd);
    }
#endif
    // Small delay to ensure file system visibility
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void waitForUpdate(std::future<void>& future)
{
    log().trace("Waiting for update future...");
    auto status = future.wait_for(std::chrono::seconds(30));
    if (status != std::future_status::ready) {
        log().error("Timeout waiting for configuration update after 30 seconds");
        throw std::runtime_error("Timeout waiting for configuration update.");
    }
    log().trace("Update future ready");
}

template<typename Predicate>
void waitForCondition(Predicate pred, std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            throw std::runtime_error("Timeout waiting for condition.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/** Stable per-instance metadata and cooperative fills for reload/cache regressions. */
class ReloadTestDataSource : public DataSource
{
public:
    /** Give every constructed instance its own authoritative dictionary namespace. */
    ReloadTestDataSource(std::string mapId, size_t instance, bool addOn = false)
        : info_(DataSourceInfo::fromJson(R"({
            "mapId": "Reload",
            "layers": {
                "Features": {
                    "featureTypes": [{
                        "name": "Point",
                        "uniqueIdCompositions": [[{"partId": "id", "datatype": "U32"}]]
                    }]
                }
            }
        })"_json))
    {
        info_.mapId_ = std::move(mapId);
        info_.stringPoolId_ = "ReloadPool-" + std::to_string(instance);
        info_.isAddOn_ = addOn;
    }

    /** Return the same metadata for this instance throughout its lifetime. */
    DataSourceInfo info() override { return info_; }

    /** Populate a minimal cacheable feature tile. */
    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        if (!empty)
            tile->newFeature("Point", {{"id", int64_t{1}}});
    }

    /** Keep a fill in flight until released, while remaining safe on test failure/shutdown. */
    void
    fill(PartitionFeatureLayer::Ptr const& tile, TileCancellationCheck const& cancelled) override
    {
        ++fills;
        while (pauseFills) {
            if (cancelled()) {
                observedCancellation = true;
                throw TileLoadCancelled();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        fill(tile);
    }

    /** Source-data payloads are unused by these tests. */
    void fill(PartitionSourceDataLayer::Ptr const&) override {}

    std::atomic_size_t fills{0};
    bool empty = false;
    std::atomic_bool pauseFills{false};
    std::atomic_bool observedCancellation{false};

private:
    DataSourceInfo info_;
};

/** Own an isolated config, constructor registration and service for reload tests. */
class ConfigReloadTest
{
public:
    /** Use synchronous file notifications and two workers; no watcher timing is involved. */
    ConfigReloadTest()
        : directory_(
              fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_reload"))
    {
        auto& config = DataSourceConfigService::get();
        config.reset();
        config.registerDataSourceType(
            "Reload",
            [this](YAML::Node const& node) -> DataSource::Ptr
            {
                auto source = std::make_shared<ReloadTestDataSource>(
                    node["mapId"].as<std::string>(),
                    ++constructions,
                    node["addOn"].as<bool>(false));
                source->empty = node["empty"].as<bool>(false);
                return source;
            });
        service = std::make_unique<
            Service>(std::make_shared<MemCache>(16), true, std::chrono::milliseconds{0}, 2);
    }

    /** Join all callbacks before destroying their captured state or temporary inputs. */
    ~ConfigReloadTest()
    {
        service.reset();
        DataSourceConfigService::get().reset();
    }

    /** Publish exactly these bytes, optionally changing the config file path. */
    void load(std::string_view yaml, std::string const& filename = "config.yaml")
    {
        auto path = directory_.path() / filename;
        {
            std::ofstream file(path);
            file << yaml;
        }
        DataSourceConfigService::get().loadConfig(path.string(), false);
    }

    /** Wait with a bounded test timeout for ready or failed rows. */
    DataSourceCatalogSnapshot settled() const
    {
        waitForCondition(
            [this]
            {
                auto snapshot = service->sourceCatalog();
                return std::ranges::none_of(
                    snapshot.sources,
                    [](auto const& entry)
                    { return entry.status == DataSourceCatalogStatus::Initializing; });
            });
        return service->sourceCatalog({}, true);
    }

    /** Schedule a synthetic tile without waiting, so a reload can happen during its fill. */
    LayerTilesRequest::Ptr request(std::string const& mapId, uint16_t x = 0)
    {
        auto result = std::make_shared<LayerTilesRequest>(
            mapId,
            "Features",
            std::vector<PartitionId>{TileId::fromTileXY(x, 0, 3)});
        if (!service->request({result})) {
            throw std::runtime_error("Reload test tile was not admitted.");
        }
        return result;
    }

    /** Fail promptly if a supposedly retained request is aborted or stops completing. */
    void requireSuccess(LayerTilesRequest::Ptr const& request) const
    {
        waitForCondition([&] { return request->isDone(); });
        request->wait();
        REQUIRE(request->getStatus() == RequestStatus::Success);
    }

    std::atomic_size_t constructions{0};
    std::unique_ptr<Service> service;

private:
    test::TemporaryDirectory directory_;
};

TEST_CASE(
    "Datasource reuse fingerprints preserve YAML structure",
    "[DataSourceConfig][config-reload]")
{
    auto& config = DataSourceConfigService::get();
    auto fingerprint = [&](std::string const& yaml)
    {
        return config.dataSourceConfigFingerprint(YAML::Load(yaml));
    };
    REQUIRE(
        fingerprint("{type: Reload, nested: {x: 1, y: [a, b]}}") ==
        fingerprint("nested:\n  y: ['a', b]\n  x: 1\ntype: Reload\n# comment\n"));
    REQUIRE(fingerprint("{value: '001'}") != fingerprint("{value: '1'}"));
    REQUIRE(fingerprint("{value: 'true'}") != fingerprint("{value: 'yes'}"));
    REQUIRE(fingerprint("{value: !!str 1}") != fingerprint("{value: !!int 1}"));
    REQUIRE(fingerprint("{value: [a, b]}") != fingerprint("{value: [b, a]}"));
    REQUIRE(fingerprint("{value: []}") != fingerprint("{value: {}}"));
    REQUIRE(fingerprint("{value: null}") != fingerprint("{value: 'null'}"));
    REQUIRE(fingerprint("{value: one, value: two}") != fingerprint("{value: two, value: one}"));
}

TEST_CASE("Datasource claim gates protect initializing and failed catalog rows", "[DataSourceConfig][datasource-auth]")
{
    ConfigReloadTest test;
    auto& config = DataSourceConfigService::get();
    ClientAuthorization allowed{AuthHeaders{{"x-supplier", "mcp"}},
        std::make_shared<nlohmann::json const>(nlohmann::json{{"groups", {"SMS_users"}}})};
    ClientAuthorization denied{AuthHeaders{{"x-supplier", "mcp"}}};
    std::atomic_bool checkedInitializing{false};
    std::atomic_bool initializingAllowed{false};
    std::atomic_bool initializingDenied{false};
    config.registerDataSourceType("GatedFailure",
        [&](YAML::Node const&, DataSourceInitContext&) -> DataSource::Ptr {
            auto snapshot = test.service->sourceCatalog(allowed, false);
            initializingAllowed = snapshot.sources.size() == 1 &&
                snapshot.sources[0].status == DataSourceCatalogStatus::Initializing;
            initializingDenied = test.service->sourceCatalog(denied, false).sources.empty();
            checkedInitializing = true;
            return nullptr;
        });
    test.load(R"(
sources:
  - type: GatedFailure
    auth-header: {X-Supplier: '^mcp$'}
    auth-claims: {/groups: SMS_users}
)");
    auto snapshot = test.settled();
    REQUIRE(checkedInitializing);
    CHECK(initializingAllowed);
    CHECK(initializingDenied);
    REQUIRE(snapshot.sources.size() == 1);
    REQUIRE(snapshot.sources[0].status == DataSourceCatalogStatus::Failed);
    CHECK(test.service->sourceCatalog(allowed).sources.size() == 1);
    CHECK(test.service->sourceCatalog(denied).sources.empty());
    DataSourceCatalogChange change;
    change.sourceUpdate = DataSourceCatalogSourceUpdate{
        .descriptor = snapshot.sources[0].descriptor,
        .status = DataSourceCatalogStatus::Failed};
    CHECK(test.service->isSourceCatalogChangeVisible(change, allowed));
    CHECK_FALSE(test.service->isSourceCatalogChangeVisible(change, denied));
}

TEST_CASE("Datasource authorization configuration rejects malformed gates", "[DataSourceConfig][datasource-auth]")
{
    ConfigReloadTest test;
    auto& config = DataSourceConfigService::get();
    auto source = nlohmann::json{{"type", "Reload"}, {"mapId", "Gated"},
                               {"auth-claims", {{"/groups", "SMS_users"}}}};
    CHECK_NOTHROW(config.validateDataSourceConfig(nlohmann::json{{"sources", {source}}}));
    for (auto const& invalid : nlohmann::json::array({
            nullptr, nlohmann::json::array(), {{"groups", "SMS_users"}},
            {{"/groups~invalid", "SMS_users"}}, {{"/groups", 123}}, {{"/groups", {"SMS_users"}}}})) {
        source["auth-claims"] = invalid;
        CHECK_THROWS(config.validateDataSourceConfig(nlohmann::json{{"sources", {source}}}));
    }
    for (auto const* invalid : {
            "sources: [{type: Reload, auth-claims: {/groups: SMS_users, /groups: other}}]",
            "sources: [{type: Reload, auth-header: {X-Role: allowed, x-role: other}}]",
            "sources: [{type: Reload, auth-header: {X-Role: '['}}]"})
        CHECK_THROWS(config.validateDataSourceConfig(YAML::Load(invalid)));
}

TEST_CASE(
    "Toggling one datasource preserves unrelated pools, cache and in-flight work",
    "[DataSourceConfig][config-reload]")
{
    ConfigReloadTest test;
    std::string const enabled = R"(
sources:
  - type: Reload
    mapId: Toggled
  - type: Reload
    mapId: Unchanged
)";
    test.load(enabled);
    auto before = test.settled();
    REQUIRE(before.sources.size() == 2);
    auto const& unchanged = before.sources[1];
    auto source = std::dynamic_pointer_cast<ReloadTestDataSource>(unchanged.dataSource);
    REQUIRE(source);
    test.requireSuccess(test.request("Unchanged"));
    REQUIRE(source->fills == 1);

    source->pauseFills = true;
    auto inFlight = test.request("Unchanged", 1);
    waitForCondition([&] { return source->fills == 2; });
    // Saving through /config may reorder keys and change quoting for every
    // descriptor, even when only one enabled flag was actually edited.
    test.load(R"(
sources:
  - enabled: false
    mapId: Toggled
    type: Reload
  - mapId: 'Unchanged'
    type: Reload
)");
    auto disabled = test.settled();
    REQUIRE(disabled.revision > before.revision);
    REQUIRE(disabled.sources.size() == 1);
    REQUIRE(disabled.sources[0].descriptor.configIndex == 1);
    REQUIRE(disabled.sources[0].descriptor.sourceId == unchanged.descriptor.sourceId);
    REQUIRE(disabled.sources[0].dataSource == source);
    REQUIRE(disabled.sources[0].info == unchanged.info);
    REQUIRE(disabled.sources[0].info->stringPoolId_ == unchanged.info->stringPoolId_);
    REQUIRE_FALSE(inFlight->isDone());
    REQUIRE_FALSE(source->observedCancellation);
    source->pauseFills = false;
    test.requireSuccess(inFlight);
    test.requireSuccess(test.request("Unchanged"));
    REQUIRE(source->fills == 2);
    REQUIRE(test.constructions == 2);

    test.load(enabled);
    auto restored = test.settled();
    REQUIRE(restored.sources.size() == 2);
    REQUIRE(restored.sources[0].dataSource != before.sources[0].dataSource);
    REQUIRE(restored.sources[1].dataSource == source);
    REQUIRE(restored.sources[1].info == unchanged.info);
    REQUIRE(test.constructions == 3);
    test.requireSuccess(test.request("Unchanged"));
    REQUIRE(source->fills == 2);
}

TEST_CASE(
    "Changed datasource options replace only that ready instance",
    "[DataSourceConfig][config-reload]")
{
    ConfigReloadTest test;
    test.load(R"(
sources:
  - {type: Reload, mapId: Other}
  - type: Reload
    mapId: Changed
    ttl: 60
    auth-header: {x-role: '^old$'}
)");
    auto before = test.settled();
    REQUIRE(before.sources.size() == 2);
    test.requireSuccess(test.request("Changed"));
    test.load(R"(
sources:
  - {type: Reload, mapId: Other}
  - type: Reload
    mapId: Changed
    ttl: 120
    auth-header: {x-role: '^new$'}
)");
    auto after = test.settled();
    REQUIRE(after.sources.size() == 2);
    REQUIRE(after.sources[0].dataSource == before.sources[0].dataSource);
    REQUIRE(after.sources[1].dataSource != before.sources[1].dataSource);
    REQUIRE(after.sources[1].info->stringPoolId_ != before.sources[1].info->stringPoolId_);
    REQUIRE(after.sources[1].dataSource->ttl() == std::chrono::minutes(2));
    REQUIRE(after.sources[1].dataSource->isDataSourceAuthorized(AuthHeaders{{"x-role", "new"}}));
    REQUIRE_FALSE(after.sources[1].dataSource->isDataSourceAuthorized(AuthHeaders{{"x-role", "old"}}));
    test.requireSuccess(test.request("Changed"));
    auto replacement = std::dynamic_pointer_cast<ReloadTestDataSource>(after.sources[1].dataSource);
    REQUIRE(replacement->fills == 1);
    REQUIRE(test.constructions == 3);
}

TEST_CASE(
    "Source retry configuration distinguishes inherited, zero and positive policies",
    "[DataSourceConfig][TTL]")
{
    ConfigReloadTest test;
    test.load(R"(
sources:
  - {type: Reload, mapId: Inherited}
  - {type: Reload, mapId: Static, error-tile-ttl: 0, empty-tile-ttl: 0}
  - {type: Reload, mapId: Refresh, error-tile-ttl: 2, empty-tile-ttl: 7}
)");
    auto sources = test.settled().sources;
    REQUIRE(sources.size() == 3);
    REQUIRE_FALSE(sources[0].dataSource->errorTileTtl());
    REQUIRE_FALSE(sources[0].dataSource->emptyTileTtl());
    REQUIRE(sources[1].dataSource->errorTileTtl() == std::chrono::seconds{0});
    REQUIRE(sources[1].dataSource->emptyTileTtl() == std::chrono::seconds{0});
    REQUIRE(sources[2].dataSource->errorTileTtl() == std::chrono::seconds{2});
    REQUIRE(sources[2].dataSource->emptyTileTtl() == std::chrono::seconds{7});

    auto& config = DataSourceConfigService::get();
    auto schema = config.getDataSourceConfigSchema();
    auto properties = schema["properties"]["sources"]["items"]["properties"];
    for (auto const* key : {"error-tile-ttl", "empty-tile-ttl"}) {
        REQUIRE(properties[key]["minimum"] == 0);
        for (auto const* invalid : {"-1", "9223372036854776", "1.5", "null"}) {
            CAPTURE(key, invalid);
            auto node = YAML::Load(
                std::string("{type: Reload, mapId: Invalid, ") + key + ": " + invalid + "}");
            REQUIRE_FALSE(config.makeDataSource(node));
        }
    }
}

TEST_CASE(
    "Reloading source retry policy invalidates a non-expiring empty result",
    "[DataSourceConfig][config-reload][TTL]")
{
    ConfigReloadTest test;
    test.load("sources: [{type: Reload, mapId: Static, empty: true, empty-tile-ttl: 0}]");
    auto old = test.settled().sources.front().dataSource;
    auto original = std::dynamic_pointer_cast<ReloadTestDataSource>(old);
    test.requireSuccess(test.request("Static"));
    test.requireSuccess(test.request("Static"));
    REQUIRE(original->fills == 1);

    // Only the policy changes; it must invalidate retained data just like any
    // other constructor-affecting source setting, rather than keep its old TTL.
    test.load("sources: [{type: Reload, mapId: Static, empty: true, empty-tile-ttl: 9}]");
    auto replacement = test.settled().sources.front().dataSource;
    REQUIRE(replacement != old);
    REQUIRE(replacement->emptyTileTtl() == std::chrono::seconds{9});
    test.requireSuccess(test.request("Static"));
    REQUIRE(std::dynamic_pointer_cast<ReloadTestDataSource>(replacement)->fills == 1);
    REQUIRE(original->fills == 1);
}

TEST_CASE(
    "Reloading add-ons invalidates only their composed map",
    "[DataSourceConfig][config-reload][add-on]")
{
    std::atomic_bool addOnReady{false};
    std::atomic_bool abortedWithPublishedCatalog{false};
    ConfigReloadTest test;
    std::string const disabled = R"(sources:
  - {type: Reload, mapId: Base}
  - {type: Reload, mapId: Other}
  - {type: Reload, mapId: Base, addOn: true, enabled: false}
)";
    test.load(disabled);
    auto before = test.settled();
    REQUIRE(before.sources.size() == 2);
    auto base = std::dynamic_pointer_cast<ReloadTestDataSource>(before.sources[0].dataSource);
    auto other = std::dynamic_pointer_cast<ReloadTestDataSource>(before.sources[1].dataSource);
    test.requireSuccess(test.request("Base"));
    test.requireSuccess(test.request("Other"));

    base->pauseFills = true;
    auto inFlight = std::make_shared<LayerTilesRequest>(
        "Base",
        "Features",
        std::vector<PartitionId>{TileId::fromTileXY(1, 0, 3)});
    inFlight->onDone_ =
        [&abortedWithPublishedCatalog,
         service = test.service.get(),
         previousInfo = before.sources[0].info](RequestStatus status)
    {
        // Cancellation callbacks run after atomic publication, outside the catalog
        // lock. This same-schema add-on preserves metadata but replaces the active
        // registration; re-entry must already see its coherent Ready catalog row.
        auto catalog = service->sourceCatalog();
        abortedWithPublishedCatalog = status == RequestStatus::Aborted &&
            !catalog.sources.empty() && catalog.sources[0].info == previousInfo &&
            std::ranges::any_of(catalog.sources,
                                [](auto const& entry) {
                                    return entry.descriptor.addOn &&
                                        entry.status == DataSourceCatalogStatus::Ready;
                                });
    };
    REQUIRE(test.service->request({inFlight}));
    waitForCondition([&] { return base->fills == 2; });

    // Enabling an add-on changes the cached composition without replacing the
    // primary. The composed primary metadata also changes, so observers receive a
    // full-catalog schema notification rather than a single-source status delta.
    auto subscription = test.service->subscribeToSourceCatalogChanges(
        [&addOnReady, service = test.service.get()](DataSourceCatalogChange const& change)
        {
            if (change.reason == "schema" && !change.sourceUpdate) {
                // Observers can re-enter the catalog; publication must not hold its lock.
                auto catalog = service->sourceCatalog();
                addOnReady =
                    catalog.revision >= change.revision &&
                    std::ranges::any_of(
                        catalog.sources,
                        [](auto const& entry) {
                            return entry.descriptor.addOn &&
                                entry.status == DataSourceCatalogStatus::Ready;
                        });
            }
        });
    test.load(R"(sources:
  - {type: Reload, mapId: Base}
  - {type: Reload, mapId: Other}
  - {type: Reload, mapId: Base, addOn: true}
)");
    auto enabled = test.settled();
    waitForCondition([&] { return addOnReady.load(); });
    REQUIRE(abortedWithPublishedCatalog);
    REQUIRE(inFlight->getStatus() == RequestStatus::Aborted);
    base->pauseFills = false;
    REQUIRE(enabled.sources.size() == 3);
    REQUIRE(enabled.sources[0].dataSource == base);
    REQUIRE(enabled.sources[1].dataSource == other);
    auto addOn = std::dynamic_pointer_cast<ReloadTestDataSource>(enabled.sources[2].dataSource);
    test.requireSuccess(test.request("Base"));
    test.requireSuccess(test.request("Other"));
    REQUIRE(base->fills == 3);
    REQUIRE(addOn->fills == 1);
    REQUIRE(other->fills == 1);

    test.load(disabled);
    auto removed = test.settled();
    REQUIRE(removed.sources.size() == 2);
    REQUIRE(removed.sources[0].dataSource == base);
    test.requireSuccess(test.request("Base"));
    test.requireSuccess(test.request("Other"));
    REQUIRE(base->fills == 4);
    REQUIRE(addOn->fills == 1);
    REQUIRE(other->fills == 1);
    REQUIRE(test.constructions == 3);
}

TEST_CASE(
    "Each silent reload withdrawal fences previously assembled schema inventories",
    "[DataSourceConfig][config-reload][schema-inventory]")
{
    std::vector<uint64_t> capturedRevisions;
    std::vector<nlohmann::json> capturedInventories;
    std::vector<uint64_t> publishedRevisions;
    ConfigReloadTest test;
    test.load(R"(sources:
  - {type: Reload, mapId: First, sourceId: first}
  - {type: Reload, mapId: Second, sourceId: second}
  - {type: Reload, mapId: Retained, sourceId: retained}
)");
    REQUIRE(test.settled().sources.size() == 3);

    auto gate = std::make_shared<std::atomic_bool>(false);
    std::vector<LayerTilesRequest::Ptr> requests;
    for (auto const* map : {"First", "Second"}) {
        auto request = std::make_shared<LayerTilesRequest>(
            map,
            "Features",
            std::vector<PartitionId>{TileId::fromTileXY(1, 0, 3)});
        request->setWorkAdmissionGate(gate);
        request->onDone_ = [&, service = test.service.get()](RequestStatus status)
        {
            if (status != RequestStatus::Aborted)
                return;
            // The first withdrawal's external callback runs after the initial
            // reload revision bump but before the next source is removed. This is
            // exactly the window of a response assembled before later encoding.
            capturedRevisions.push_back(service->sourceCatalogRevision());
            capturedInventories.push_back(service->schemaInventory());
        };
        requests.push_back(std::move(request));
    }
    REQUIRE(test.service->request(requests));
    auto subscription = test.service->subscribeToSourceCatalogChanges(
        [&](DataSourceCatalogChange const& change)
        {
            if (change.reason == "reload")
                publishedRevisions.push_back(change.revision);
        });
    test.load(R"(sources:
  - {type: Reload, mapId: Retained, sourceId: retained}
)");

    REQUIRE(capturedRevisions.size() == 2);
    REQUIRE(capturedInventories.size() == 2);
    // These identical legacy declarations share one artifact; its consumers
    // change on each withdrawal, even though the artifact identity is unchanged.
    REQUIRE(capturedInventories[0].size() == 1);
    REQUIRE(capturedInventories[1].size() == 1);
    CHECK(capturedInventories[0][0]["consumers"].size() == 2);
    CHECK(capturedInventories[1][0]["consumers"].size() == 1);
    CHECK(capturedRevisions[1] > capturedRevisions[0]);
    CHECK(test.service->sourceCatalogRevision() > capturedRevisions[0]);
    REQUIRE(publishedRevisions.size() == 1);
    CHECK(publishedRevisions[0] == test.service->sourceCatalogRevision());
    for (auto const& request : requests)
        CHECK(request->getStatus() == RequestStatus::Aborted);
}

TEST_CASE(
    "Shared settings and config path changes prevent datasource reuse",
    "[DataSourceConfig][config-reload]")
{
    ConfigReloadTest test;
    std::string const original = "shared: {url: first}\nsources: [{type: Reload, mapId: Map}]\n";
    test.load(original);
    auto before = test.settled();
    REQUIRE(before.sources.size() == 1);

    test.load("sources: [{mapId: Map, type: Reload}]\nshared:\n  url: first\n");
    auto same = test.settled();
    REQUIRE(same.sources[0].dataSource == before.sources[0].dataSource);

    std::string const changed = "shared: {url: second}\nsources: [{type: Reload, mapId: Map}]\n";
    test.load(changed);
    auto sharedChanged = test.settled();
    REQUIRE(sharedChanged.sources[0].dataSource != before.sources[0].dataSource);

    test.load(changed, "moved.yaml");
    auto moved = test.settled();
    REQUIRE(moved.sources[0].dataSource != sharedChanged.sources[0].dataSource);
    REQUIRE(test.constructions == 3);
}

TEST_CASE(
    "Explicit source IDs preserve instances across catalog reordering",
    "[DataSourceConfig][config-reload]")
{
    ConfigReloadTest test;
    test.load(R"(sources:
  - {type: Reload, sourceId: first, mapId: First}
  - {type: Reload, sourceId: second, mapId: Second}
)");
    auto before = test.settled();
    REQUIRE(before.sources.size() == 2);
    test.load(R"(sources:
  - {type: Reload, sourceId: second, mapId: Second}
  - {type: Reload, sourceId: first, mapId: First}
)");
    auto after = test.settled();
    REQUIRE(after.sources.size() == 2);
    REQUIRE(after.sources[0].descriptor.sourceId == "second");
    REQUIRE(after.sources[0].descriptor.configIndex == 0);
    REQUIRE(after.sources[0].dataSource == before.sources[1].dataSource);
    REQUIRE(after.sources[1].descriptor.sourceId == "first");
    REQUIRE(after.sources[1].descriptor.configIndex == 1);
    REQUIRE(after.sources[1].dataSource == before.sources[0].dataSource);
    REQUIRE(test.constructions == 2);
}

TEST_CASE(
    "Reload retains ready sources but rejects superseded constructor callbacks",
    "[DataSourceConfig][config-reload]")
{
    std::atomic_bool started{false};
    std::atomic_bool finished{false};
    ConfigReloadTest test;
    DataSourceConfigService::get().registerDataSourceType(
        "SlowReload",
        [&](YAML::Node const&, DataSourceInitContext& context) -> DataSource::Ptr
        {
            started = true;
            while (!context.isCancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            context.setStatusMessage("Late status must not overwrite retained rows.");
            context.setProgress(99.0f);
            finished = true;
            return std::make_shared<NamedTestDataSource>("Obsolete");
        });
    test.load("sources: [{type: Reload, mapId: Ready}, {type: SlowReload}]\n");
    waitForCondition(
        [&]
        {
            auto catalog = test.service->sourceCatalog();
            return started && catalog.sources.size() == 2 &&
                catalog.sources[0].status == DataSourceCatalogStatus::Ready;
        });
    auto ready = test.service->sourceCatalog().sources[0];
    test.load("sources: [{type: Reload, mapId: Ready}, {type: SlowReload, enabled: false}]\n");
    auto after = test.settled();
    waitForCondition([&] { return finished.load(); });
    REQUIRE(after.sources.size() == 1);
    REQUIRE(after.sources[0].dataSource == ready.dataSource);
    REQUIRE(after.sources[0].info == ready.info);
    REQUIRE(after.sources[0].statusMessage.empty());
    REQUIRE_FALSE(after.sources[0].progress);
    REQUIRE(test.service->info().size() == 1);
    REQUIRE(test.constructions == 1);
}

TEST_CASE(
    "Reload retries failed constructors while retaining ready sources",
    "[DataSourceConfig][config-reload]")
{
    std::atomic_bool recover{false};
    std::atomic_size_t attempts{0};
    ConfigReloadTest test;
    DataSourceConfigService::get().registerDataSourceType(
        "RetryReload",
        [&](YAML::Node const&) -> DataSource::Ptr
        {
            ++attempts;
            if (!recover) {
                return {};
            }
            return std::make_shared<NamedTestDataSource>("Recovered");
        });
    std::string const config = "sources: [{type: Reload, mapId: Ready}, {type: RetryReload}]\n";
    test.load(config);
    auto before = test.settled();
    REQUIRE(before.sources.size() == 2);
    REQUIRE(before.sources[1].status == DataSourceCatalogStatus::Failed);
    recover = true;
    test.load(config);
    auto after = test.settled();
    REQUIRE(after.sources.size() == 2);
    REQUIRE(after.sources[0].dataSource == before.sources[0].dataSource);
    REQUIRE(after.sources[1].status == DataSourceCatalogStatus::Ready);
    REQUIRE(attempts == 2);
    REQUIRE(test.constructions == 1);
}

TEST_CASE("Config JSON preserves empty container types in YAML", "[DataSourceConfig]")
{
    auto input = R"({"sources":[],"options":{},"nested":[{},[],null]})"_json;
    auto yaml = jsonToYaml(input);
    REQUIRE(yaml["sources"].IsSequence());
    REQUIRE(yaml["options"].IsMap());
    REQUIRE(yamlToJson(yaml, false) == input);
    REQUIRE(yamlToJson(YAML::Load(YAML::Dump(yaml)), false) == input);
}

TEST_CASE("Mapget Config", "[MapgetConfig]")
{
    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test_config");
    test::TemporaryDirectory directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    SECTION("Bad Config")
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: [" << std::endl;
        REQUIRE(mapget::runFromCommandLine({std::string("--config"), tempConfigPath.string()}) == 1);
    }

    SECTION("Good Config")
    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
        sources:
          - type: TestDataSource
        )" << std::endl;
        out.flush();
        out.close();
        syncFile(tempConfigPath);
        REQUIRE(mapget::runFromCommandLine({std::string("--config"), tempConfigPath.string()}, false) == 0);
    }
}

TEST_CASE("CLI rejects invalid error and empty tile TTLs before startup", "[MapgetConfig][TTL]")
{
    for (auto const* option : {"--error-tile-ttl", "--empty-tile-ttl"}) {
        for (auto const* value : {"-1", "9223372036854776"}) {
            CAPTURE(option, value);
            REQUIRE(mapget::runFromCommandLine({"serve", option, value}) == 1);
        }
    }
}

TEST_CASE("Datasource Config", "[DataSourceConfig]")
{
    setLogLevel("trace", log());

    auto tempDir = fs::temp_directory_path() / test::generateTimestampedDirectoryName("mapget_test_ds_config");
    test::TemporaryDirectory directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    log().info("Created temp directory at: {}", tempDir.string());
    log().info("Config file path: {}", tempConfigPath.string());

    DataSourceConfigService::get().reset();
    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node&) -> DataSource::Ptr
        { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);
    log().info(service.info().empty() ? "Info is empty." : service.info()[0].toJson().dump(4));
    REQUIRE(service.info().empty());

    std::atomic<bool> updateReceived{false};
    std::promise<void> updatePromise;
    auto updateFuture = updatePromise.get_future();
    auto prepareNextUpdate = [&]()
    {
        updateReceived = false;
        updatePromise = std::promise<void>();
        updateFuture = updatePromise.get_future();
    };

    auto subscription = DataSourceConfigService::get().subscribe(
        [&](auto&& configNodes)
        {
            log().debug("Configuration update detected. Nodes count: {}", configNodes.size());
            if (!updateReceived.exchange(true)) {
                log().trace("Setting promise value for config update");
                updatePromise.set_value();
            } else {
                log().trace("Update already received, skipping promise set");
            }
        });

    // Initial empty configuration
    {
        log().trace("Writing initial empty config to: {}", tempConfigPath.string());
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        if (!out.is_open()) {
            log().error("Failed to open config file for writing");
            FAIL("Could not open config file");
        }
        out << "sources: []" << std::endl;
        out.flush();
        out.close();
        
        syncFile(tempConfigPath);
        
        if (!fs::exists(tempConfigPath)) {
            log().error("Config file was not created");
            FAIL("Config file does not exist after writing");
        }
        
        log().info("Written initial empty config, file size: {} bytes", fs::file_size(tempConfigPath));
    }

    prepareNextUpdate();
    log().debug("About to call loadConfig with path: {}", tempConfigPath.string());
    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    log().debug("loadConfig returned, waiting for update");
    waitForUpdate(updateFuture);
    log().debug("Update received, waiting for service info to be empty");
    waitForCondition([&service]() { return service.info().empty(); });
    REQUIRE(service.info().empty());

    // Adding a datasource
    prepareNextUpdate();
    {
        log().trace("Writing config with TestDataSource");
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources:\n  - type: TestDataSource\n";
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().size() == 1; });
    auto dataSourceInfos = service.info();
    REQUIRE(dataSourceInfos.size() == 1);
    REQUIRE(dataSourceInfos[0].mapId_ == "Catan");

    // Removing the datasource
    prepareNextUpdate();
    {
        log().trace("Writing empty config to remove datasources");
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << "sources: []" << std::endl;
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }
    waitForUpdate(updateFuture);
    waitForCondition([&service]() { return service.info().empty(); });
    REQUIRE(service.info().empty());

    // Join the watcher before the directory owner removes its inputs.
    DataSourceConfigService::get().end();
}

TEST_CASE("Datasource enabled flag is handled generically", "[DataSourceConfig]")
{
    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_test_ds_enabled");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    DataSourceConfigService::get().reset();
    DataSourceConfigService::get().registerDataSourceType(
        "TestDataSource",
        [](const YAML::Node&) -> DataSource::Ptr
        { return std::make_shared<TestDataSource>(); });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);

    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
sources:
  - type: TestDataSource
    enabled: false
  - type: TestDataSource
    enabled: true
)";
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }

    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    waitForCondition([&service]() { return service.info().size() == 1; });

    auto stats = DataSourceConfigService::get().getDataSourceConfigStats();
    REQUIRE(stats.configured == 2);
    REQUIRE(stats.enabled == 1);
    REQUIRE(stats.disabled == 1);

    auto serviceStats = service.getStatistics();
    REQUIRE(serviceStats["datasource-config"]["configured"].get<size_t>() == 2);
    REQUIRE(serviceStats["datasource-config"]["enabled"].get<size_t>() == 1);
    REQUIRE(serviceStats["datasource-config"]["disabled"].get<size_t>() == 1);
    REQUIRE(serviceStats["datasource-config"]["construction-failed"].get<size_t>() == 0);

    fs::remove_all(tempDir);
    DataSourceConfigService::get().end();
}

TEST_CASE("Service memory snapshot includes cooperative datasource ownership", "[Service][memory]")
{
    Service service(std::make_shared<MemCache>(8), false);
    auto dataSource = std::make_shared<MemoryReportingDataSource>();
    service.add(dataSource);

    auto const memory = service.getMemoryStatistics();
    REQUIRE(memory["mapget"]["allocated-bytes"].get<uint64_t>() > 0);
    REQUIRE(memory["datasource-measured-bytes"] == 123456);
    REQUIRE(memory["known-current-bytes"].get<uint64_t>() >= 123456);
    REQUIRE(memory["datasources"].size() == 1);
    REQUIRE(memory["datasources"][0]["map-id"] == "MemoryReportingMap");
    REQUIRE(memory["datasources"][0]["measurement"] == "datasource-estimate");
    REQUIRE(memory["datasources"][0]["retained-bytes"] == 123456);
    REQUIRE(memory["process"].contains("measurement"));
    REQUIRE_FALSE(
        memory["mapget"]["components"].contains(
            "metadata.catalog-snapshots"));
    REQUIRE_FALSE(
        memory["mapget"]["components"].contains(
            "metadata.worker-snapshot-containers"));

    auto const firstCatalog = service.sourceCatalog();
    auto const secondCatalog = service.sourceCatalog();
    REQUIRE(firstCatalog.sources.size() == 1);
    REQUIRE(secondCatalog.sources.size() == 1);
    REQUIRE(firstCatalog.sources[0].info);
    REQUIRE(
        firstCatalog.sources[0].info ==
        secondCatalog.sources[0].info);
}

TEST_CASE("Datasource catalog display names are generic and display-only", "[DataSourceConfig]")
{
    auto& configService = DataSourceConfigService::get();

    auto const configuredMap = configService.describeDataSource(YAML::Load(R"(
type: TestDataSource
mapId: ConfiguredMap
uri: /ignored/when/mapId/is/present
)"), 3);
    REQUIRE(configuredMap.displayName == "datasource-3-ConfiguredMap");

    auto const scalarFallback = configService.describeDataSource(YAML::Load(R"(
type: TestDataSource
uri: filestore:/tmp/Example.Map
serverIndex: 2
apiKey: must-not-leak
auth-header:
  Authorization: "^Bearer .+$"
)"), 4);
    REQUIRE(
        scalarFallback.displayName
        == "datasource-4-TestDataSource-filestore:/tmp/Example.Map-2");
}

TEST_CASE("Datasource catalog tracks config order and async startup status", "[DataSourceConfig]")
{
    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_test_ds_catalog");
    fs::create_directory(tempDir);
    auto tempConfigPath = tempDir / "temp_config.yaml";

    DataSourceConfigService::get().reset();

    std::promise<void> slowStartedPromise;
    auto slowStarted = slowStartedPromise.get_future().share();
    std::promise<void> releaseSlowPromise;
    auto releaseSlow = releaseSlowPromise.get_future().share();
    std::atomic_bool slowStartedReported{false};

    DataSourceConfigService::get().registerDataSourceType(
        "SlowDataSource",
        [&](const YAML::Node&, DataSourceInitContext& initContext) -> DataSource::Ptr
        {
            initContext.setStatusMessage("Waiting for test release.");
            initContext.setProgress(25.0f);
            if (!slowStartedReported.exchange(true)) {
                slowStartedPromise.set_value();
            }
            while (releaseSlow.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                if (initContext.isCancelled && initContext.isCancelled()) {
                    return nullptr;
                }
            }
            return std::make_shared<NamedTestDataSource>("SlowMap");
        });

    DataSourceConfigService::get().registerDataSourceType(
        "FastDataSource",
        [](const YAML::Node&, DataSourceInitContext&) -> DataSource::Ptr
        {
            return std::make_shared<NamedTestDataSource>("FastMap");
        });

    DataSourceConfigService::get().registerDataSourceType(
        "FailingDataSource",
        [](const YAML::Node&, DataSourceInitContext& initContext) -> DataSource::Ptr
        {
            initContext.setStatusMessage("Intentional test failure.");
            return nullptr;
        });

    auto cache = std::make_shared<MemCache>();
    Service service(cache, true);

    std::mutex changesMutex;
    std::vector<DataSourceCatalogChange> changes;
    auto sourceChanges = service.subscribeToSourceCatalogChanges(
        [&](DataSourceCatalogChange const& change) {
            std::lock_guard lock(changesMutex);
            changes.push_back(change);
        });

    {
        std::ofstream out(tempConfigPath, std::ios_base::trunc);
        out << R"(
sources:
  - type: SlowDataSource
    mapId: SlowConfiguredMap
  - type: FailingDataSource
    mapId: FailingConfiguredMap
  - type: FastDataSource
    mapId: FastConfiguredMap
)";
        out.flush();
        out.close();
        syncFile(tempConfigPath);
    }

    DataSourceConfigService::get().loadConfig(tempConfigPath.string());
    REQUIRE(slowStarted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    waitForCondition([&service]() {
        auto catalog = service.sourceCatalog();
        return catalog.sources.size() == 3
            && catalog.sources[0].status == DataSourceCatalogStatus::Initializing
            && catalog.sources[1].status == DataSourceCatalogStatus::Failed
            && catalog.sources[2].status == DataSourceCatalogStatus::Ready;
    });

    auto catalog = service.sourceCatalog();
    REQUIRE(catalog.sources.size() == 3);
    REQUIRE(catalog.sources[0].descriptor.displayName == "datasource-0-SlowConfiguredMap");
    REQUIRE(catalog.sources[1].descriptor.displayName == "datasource-1-FailingConfiguredMap");
    REQUIRE(catalog.sources[2].descriptor.displayName == "datasource-2-FastConfiguredMap");
    REQUIRE(catalog.sources[0].descriptor.configIndex == 0);
    REQUIRE(catalog.sources[1].descriptor.configIndex == 1);
    REQUIRE(catalog.sources[2].descriptor.configIndex == 2);
    REQUIRE(catalog.sources[0].statusMessage == "Waiting for test release.");
    REQUIRE(catalog.sources[0].progress == std::optional<float>{25.0f});
    REQUIRE(catalog.sources[1].statusMessage == "Intentional test failure.");
    REQUIRE(catalog.sources[2].info);
    REQUIRE(catalog.sources[2].info->mapId_ == "FastMap");

    auto readyInfos = service.info();
    REQUIRE(readyInfos.size() == 1);
    REQUIRE(readyInfos.front().mapId_ == "FastMap");

    auto blockingCatalogFuture = std::async(std::launch::async, [&service] {
        return service.sourceCatalog({}, true);
    });
    REQUIRE(blockingCatalogFuture.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);

    releaseSlowPromise.set_value();
    REQUIRE(blockingCatalogFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    waitForCondition([&service]() {
        auto catalog = service.sourceCatalog();
        return catalog.sources.size() == 3
            && catalog.sources[0].status == DataSourceCatalogStatus::Ready
            && catalog.sources[1].status == DataSourceCatalogStatus::Failed
            && catalog.sources[2].status == DataSourceCatalogStatus::Ready;
    });

    catalog = service.sourceCatalog();
    auto blockingCatalog = blockingCatalogFuture.get();
    REQUIRE(blockingCatalog.sources.size() == 3);
    REQUIRE(blockingCatalog.sources[0].status == DataSourceCatalogStatus::Ready);
    REQUIRE(catalog.sources[0].info);
    REQUIRE(catalog.sources[0].info->mapId_ == "SlowMap");
    REQUIRE(service.info().size() == 2);
    {
        std::lock_guard lock(changesMutex);
        REQUIRE_FALSE(changes.empty());
        REQUIRE(std::ranges::any_of(changes, [](auto const& change) {
            return change.sourceUpdate
                && change.sourceUpdate->descriptor.configIndex == 0
                && change.sourceUpdate->statusMessage == "Waiting for test release.";
        }));
        REQUIRE(std::ranges::any_of(changes, [](auto const& change) {
            return change.sourceUpdate
                && change.sourceUpdate->descriptor.configIndex == 0
                && change.sourceUpdate->progress == std::optional<float>{25.0f};
        }));
        REQUIRE(changes.back().revision == service.sourceCatalogRevision());
    }

    fs::remove_all(tempDir);
    DataSourceConfigService::get().end();
}

TEST_CASE("Transient datasource construction retries preserve catalog identity and cancel on reload", "[DataSourceConfig][retry]")
{
    auto tempDir = fs::current_path() / test::generateTimestampedDirectoryName("mapget_retry");
    fs::create_directory(tempDir);
    auto config = tempDir / "config.yaml";
    auto& configs = DataSourceConfigService::get();
    configs.reset();
    std::atomic_int attempts{0};
    std::atomic_bool available{false};
    std::atomic_size_t retryIndex{0};
    configs.registerDataSourceType("RetrySource", [&](YAML::Node const&, DataSourceInitContext& context) -> DataSource::Ptr {
        retryIndex = context.retryAttempt;
        ++attempts;
        if (available) return std::make_shared<NamedTestDataSource>("RecoveredMap");
        context.retryAfter = std::chrono::milliseconds(50);
        context.setStatusMessage("temporary outage");
        return nullptr;
    });
    {
        Service service(std::make_shared<MemCache>(), true);
        { std::ofstream out(config); out << "sources:\n  - type: RetrySource\n    mapId: RetryMap\n"; }
        configs.loadConfig(config.string());
        waitForCondition([&] { return attempts >= 2; });
        auto failed = service.sourceCatalog({}, true);
        REQUIRE(failed.sources.size() == 1);
        REQUIRE(failed.sources[0].status == DataSourceCatalogStatus::Failed);
        REQUIRE(failed.sources[0].retrying);
        REQUIRE(retryIndex >= 1);
        REQUIRE(service.getStatistics()["datasource-config"]["construction-failed"] == 1);
        auto id = failed.sources[0].descriptor.sourceId;
        available = true;
        waitForCondition([&] { return service.sourceCatalog().sources[0].status == DataSourceCatalogStatus::Ready; });
        REQUIRE(service.sourceCatalog().sources[0].descriptor.sourceId == id);
        REQUIRE_FALSE(service.sourceCatalog().sources[0].retrying);
        REQUIRE(service.info().size() == 1);
        REQUIRE(service.getStatistics()["datasource-config"]["construction-failed"] == 0);
        // A new outage's wait must be cancelled by config removal.
        available = false;
        configs.loadConfig(config.string());
        REQUIRE(service.sourceCatalog().sources[0].status == DataSourceCatalogStatus::Ready);
        // Unchanged ready sources survive reload, so change the constructor inputs
        // to start a fresh attempt before checking cancellation of its retry wait.
        { std::ofstream out(config); out << "sources:\n  - type: RetrySource\n    mapId: RetryMapAgain\n"; }
        configs.loadConfig(config.string());
        waitForCondition([&] { return service.sourceCatalog().sources[0].status == DataSourceCatalogStatus::Failed; });
        { std::ofstream out(config); out << "sources: []\n"; }
        configs.loadConfig(config.string());
        int stopped = attempts;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        REQUIRE(attempts == stopped);
        REQUIRE(service.sourceCatalog().sources.empty());
    }
    configs.end();
    fs::remove_all(tempDir);
}
