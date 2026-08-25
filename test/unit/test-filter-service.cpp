#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mapget/model/featurelayer-filter.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "../../libs/http-service/src/tiles-request-json.h"

using namespace mapget;

namespace
{

TileId firstTile()
{
    return TileId::fromTileXY(1, 0, 1);
}

TileId secondTile()
{
    return TileId::fromTileXY(2, 0, 1);
}

std::vector<TileId> tileSequence(size_t count)
{
    std::vector<TileId> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        result.push_back(TileId::fromTileXY(static_cast<int>(index + 1), 0, 6));
    }
    return result;
}

DataSourceInfo filterDataSourceInfo(
    std::string const& stringPoolId =
        "FilterServicePool",
    bool addOn = false)
{
    auto json = nlohmann::json::parse(R"({
        "stringPoolId": "placeholder",
        "mapId": "FilterMap",
        "maxParallelJobs": 1,
        "layers": {
            "Road": {
                "type": "Features",
                "featureTypes": [{
                    "name": "Road",
                    "uniqueIdCompositions": [[
                        {
                            "partId": "tileId",
                            "description": "Synthetic tile id.",
                            "datatype": "U32"
                        },
                        {
                            "partId": "roadId",
                            "description": "Synthetic road id.",
                            "datatype": "U64"
                        }
                    ], [
                        {
                            "partId": "tileId",
                            "description": "Synthetic tile id.",
                            "datatype": "U32"
                        },
                        {
                            "partId": "externalRoadId",
                            "description": "Synthetic secondary road id.",
                            "datatype": "U64"
                        }
                    ]]
                }]
            }
        }
    })");
    json["stringPoolId"] = stringPoolId;
    json["addOn"] = addOn;
    return DataSourceInfo::fromJson(json);
}

class FilterDataSource : public DataSource
{
public:
    explicit FilterDataSource(
        std::string stringPoolId =
            "FilterServicePool",
        bool addOn = false)
        : info_(
              filterDataSourceInfo(
                  stringPoolId,
                  addOn))
    {}

    DataSourceInfo info() override
    {
        return info_;
    }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        {
            std::lock_guard lock(mutex_);
            requestedTiles_.push_back(tile->tileId());
        }

        tile->setInfo("Producer/backend", "synthetic");
        auto feature = tile->newFeature(
            "Road",
            {
                {
                    "tileId",
                    static_cast<int64_t>(
                        tile->tileId().value())},
                {"roadId", int64_t{42}},
            });
        auto geometry = tile->newGeometry(GeomType::Line, 2);
        geometry->setName("centerline");
        geometry->append(Point{11.0, 48.0, 0.0});
        geometry->append(Point{11.1, 48.1, 0.0});
        feature->addGeometry(geometry);
        auto attribute =
            feature->attributeLayers()
                ->newLayer("rules")
                ->newAttribute("speedLimit");
        attribute->addField(
            "limit",
            tile->newValue(int64_t{80}));
    }

    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error(
            "FilterDataSource does not provide source-data tiles");
    }

    std::vector<TileId> requestedTiles() const
    {
        std::lock_guard lock(mutex_);
        return requestedTiles_;
    }

private:
    DataSourceInfo info_;
    mutable std::mutex mutex_;
    std::vector<TileId> requestedTiles_;
};

class FailingFilterDataSource : public FilterDataSource
{
public:
    FailingFilterDataSource()
        : FilterDataSource("FailingFilterPool")
    {}

    void fill(TileFeatureLayer::Ptr const&) override
    {
        ++attempts_;
        throw std::runtime_error(
            "synthetic source failure");
    }

    size_t attempts() const
    {
        return attempts_;
    }

private:
    std::atomic_size_t attempts_ = 0;
};

class VersionedFilterDataSource : public FilterDataSource
{
public:
    VersionedFilterDataSource(
        std::string stringPoolId,
        std::string revision)
        : FilterDataSource(std::move(stringPoolId)),
          revision_(std::move(revision))
    {}

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        FilterDataSource::fill(tile);
        tile->setInfo(
            "Producer/revision",
            revision_);
    }

private:
    std::string revision_;
};

class LifetimeFilterDataSource : public FilterDataSource
{
public:
    LifetimeFilterDataSource(
        std::string stringPoolId,
        std::chrono::system_clock::time_point timestamp,
        std::chrono::milliseconds ttl,
        bool addOn = false)
        : FilterDataSource(std::move(stringPoolId), addOn),
          timestamp_(timestamp),
          ttl_(ttl)
    {}

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        FilterDataSource::fill(tile);
        tile->setTimestamp(timestamp_);
        tile->setTtl(ttl_);
    }

private:
    std::chrono::system_clock::time_point timestamp_;
    std::chrono::milliseconds ttl_;
};

class BlockingResetDataSource : public FilterDataSource
{
public:
    BlockingResetDataSource()
        : FilterDataSource("BlockingResetPool")
    {}

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        auto const fillNumber =
            fillCount_.fetch_add(1) + 1;
        FilterDataSource::fill(tile);
        if (fillNumber == 1) {
            std::unique_lock lock(mutex_);
            firstStarted_ = true;
            changed_.notify_all();
            changed_.wait(lock, [&] {
                return releaseFirst_;
            });
            tile->setInfo("Producer/revision", "stale");
            return;
        }
        tile->setInfo("Producer/revision", "fresh");
    }

    [[nodiscard]] bool waitForFirstStart()
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] {
                return firstStarted_;
            });
    }

    void releaseFirst()
    {
        {
            std::lock_guard lock(mutex_);
            releaseFirst_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] size_t fillCount() const
    {
        return fillCount_;
    }

private:
    std::atomic_size_t fillCount_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool firstStarted_ = false;
    bool releaseFirst_ = false;
};

class OutOfOrderFilterDataSource : public FilterDataSource
{
public:
    OutOfOrderFilterDataSource()
        : FilterDataSource("OutOfOrderFilterPool"),
          info_(filterDataSourceInfo("OutOfOrderFilterPool"))
    {
        info_.maxParallelJobs_ = 2;
    }

    DataSourceInfo info() override
    {
        return info_;
    }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        if (tile->tileId() == firstTile()) {
            FilterDataSource::fill(tile);
            std::unique_lock lock(mutex_);
            firstStarted_ = true;
            changed_.notify_all();
            changed_.wait(lock, [&] {
                return releaseFirst_;
            });
            return;
        }
        if (tile->tileId() == secondTile()) {
            // Admission is request-ordered, but two admitted workers may enter
            // their virtual fill calls in either OS scheduling order. Anchor
            // this fixture at the first tile's start so the test measures
            // completion/result ordering rather than thread-entry jitter.
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] {
                return firstStarted_;
            });
            lock.unlock();
            FilterDataSource::fill(tile);
            lock.lock();
            secondCompleted_ = true;
            changed_.notify_all();
            return;
        }
        FilterDataSource::fill(tile);
    }

    bool waitForSecondCompletion(
        std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return firstStarted_ && secondCompleted_;
        });
    }

    void releaseFirst()
    {
        {
            std::lock_guard lock(mutex_);
            releaseFirst_ = true;
        }
        changed_.notify_all();
    }

private:
    DataSourceInfo info_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool firstStarted_ = false;
    bool secondCompleted_ = false;
    bool releaseFirst_ = false;
};

class CanonicalLocateDataSource : public FilterDataSource
{
public:
    CanonicalLocateDataSource()
        : FilterDataSource("CanonicalLocatePool")
    {}

    std::vector<LocateCandidate> locate(
        LocateRequest const& request) override
    {
        requestedTypeId_ = request.typeId_;
        requestedFeatureId_ = request.featureId_;
        return {LocateCandidate(
            MapTileKey{
                LayerType::Features,
                "FilterMap",
                "Road",
                firstTile()},
            formatFeatureIdString(
                request.typeId_,
                request.featureId_))};
    }

    std::string requestedTypeId_;
    KeyValuePairs requestedFeatureId_;
};

class AttachmentFilterDataSource : public FilterDataSource
{
public:
    AttachmentFilterDataSource()
        : FilterDataSource("AttachmentFilterPool")
    {}

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        FilterDataSource::fill(tile);
        tile->setGlbAttachmentName(
            "synthetic.glb");
    }

    std::optional<AttachmentResponse> attachment(
        AttachmentRequest const& request) override
    {
        ++attachmentCalls_;
        if (request.name_ != "synthetic.glb") {
            return {};
        }
        return AttachmentResponse{
            .name_ = request.name_,
            .mimeType_ = "model/gltf-binary",
            .bytes_ =
                std::make_shared<
                    std::vector<uint8_t> const>(
                    std::initializer_list<uint8_t>{
                        0x67,
                        0x6c,
                        0x54,
                        0x46}),
            .etag_ = "\"synthetic\"",
        };
    }

    [[nodiscard]] size_t attachmentCalls() const
    {
        return attachmentCalls_;
    }

private:
    std::atomic_size_t attachmentCalls_ = 0;
};

class PointGroupDataSource : public DataSource
{
public:
    PointGroupDataSource(
        TileId westernTile,
        TileId easternTile,
        Point westernPoint,
        Point easternPoint,
        std::optional<TileId> limitingLifetimeTile = std::nullopt)
        : info_(filterDataSourceInfo("PointGroupPool")),
          westernTile_(westernTile),
          easternTile_(easternTile),
          westernPoint_(westernPoint),
          easternPoint_(easternPoint),
          limitingLifetimeTile_(limitingLifetimeTile)
    {}

    DataSourceInfo info() override
    {
        return info_;
    }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        {
            std::lock_guard lock(mutex_);
            requestedTiles_.push_back(tile->tileId());
        }
        if (limitingLifetimeTile_) {
            tile->setTimestamp(
                std::chrono::system_clock::time_point{
                    std::chrono::seconds{4'000'000'000}});
            tile->setTtl(
                tile->tileId() == *limitingLifetimeTile_
                    ? std::chrono::milliseconds{1250}
                    : std::chrono::seconds{10});
        }
        std::optional<Point> point;
        if (tile->tileId() == westernTile_) {
            point = westernPoint_;
        }
        else if (tile->tileId() == easternTile_) {
            point = easternPoint_;
        }
        if (!point) {
            return;
        }

        auto feature = tile->newFeature(
            "Road",
            {
                {
                    "tileId",
                    static_cast<int64_t>(
                        tile->tileId().value())},
                {
                    "roadId",
                    tile->tileId() == westernTile_
                        ? int64_t{1}
                        : int64_t{2}},
            });
        auto geometry =
            tile->newGeometry(GeomType::Points, 1);
        geometry->setName("merge");
        geometry->append(*point);
        feature->addGeometry(geometry);
    }

    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error(
            "PointGroupDataSource does not provide source-data tiles");
    }

    std::vector<TileId> requestedTiles() const
    {
        std::lock_guard lock(mutex_);
        return requestedTiles_;
    }

private:
    DataSourceInfo info_;
    TileId westernTile_;
    TileId easternTile_;
    Point westernPoint_;
    Point easternPoint_;
    std::optional<TileId> limitingLifetimeTile_;
    mutable std::mutex mutex_;
    std::vector<TileId> requestedTiles_;
};

class RelationDataSource : public DataSource
{
public:
    RelationDataSource(
        TileId first,
        TileId second,
        std::optional<TileId> failingTile =
            std::nullopt,
        std::optional<TileId> limitingLifetimeTile =
            std::nullopt)
        : info_(
              filterDataSourceInfo(
                  "RelationServicePool")),
          first_(first),
          second_(second),
          failingTile_(failingTile),
          limitingLifetimeTile_(limitingLifetimeTile)
    {}

    DataSourceInfo info() override
    {
        return info_;
    }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        {
            std::lock_guard lock(mutex_);
            requestedTiles_.push_back(
                tile->tileId());
        }
        if (limitingLifetimeTile_) {
            tile->setTimestamp(
                std::chrono::system_clock::time_point{
                    std::chrono::seconds{4'000'000'000}});
            tile->setTtl(
                tile->tileId() == *limitingLifetimeTile_
                    ? std::chrono::milliseconds{900}
                    : std::chrono::seconds{10});
        }
        if (failingTile_ &&
            tile->tileId() == *failingTile_)
        {
            throw std::runtime_error(
                "synthetic relation target failure");
        }
        if (tile->tileId() != first_ &&
            tile->tileId() != second_)
        {
            return;
        }
        auto const roadId =
            tile->tileId() == first_
            ? int64_t{1}
            : int64_t{2};
        auto const otherRoadId =
            tile->tileId() == first_
            ? int64_t{2}
            : int64_t{1};
        auto const otherTile =
            tile->tileId() == first_
            ? second_
            : first_;
        auto feature = tile->newFeature(
            "Road",
            {
                {
                    "tileId",
                    static_cast<int64_t>(
                        tile->tileId().value())},
                {"roadId", roadId},
            });
        auto geometry =
            tile->newGeometry(
                GeomType::Points,
                1);
        geometry->setName("relation");
        auto const [longitude, latitude] =
            tile->tileId().centerWgs84();
        geometry->append(
            Point{longitude, latitude, 0.0});
        feature->addGeometry(geometry);
        feature->addRelation(
            "connected",
            "Road",
            {
                {
                    "tileId",
                    static_cast<int64_t>(
                        otherTile.value())},
                {"externalRoadId", otherRoadId},
            });
    }

    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error(
            "RelationDataSource does not provide source-data tiles");
    }

    std::vector<LocateCandidate> locate(
        LocateRequest const& request) override
    {
        ++locateCalls_;
        return locateResponse(request);
    }

    size_t locateCalls() const
    {
        return locateCalls_;
    }

    std::vector<TileId> requestedTiles() const
    {
        std::lock_guard lock(mutex_);
        return requestedTiles_;
    }

private:
    std::vector<LocateCandidate> locateResponse(
        LocateRequest const& request)
    {
        auto tileId =
            request.getIntIdPart("tileId");
        auto externalRoadId =
            request.getIntIdPart(
                "externalRoadId");
        if (!tileId ||
            !externalRoadId ||
            request.typeId_ != "Road")
        {
            return {};
        }
        return {LocateCandidate::fromFeatureIdExpression(
            MapTileKey(
                LayerType::Features,
                "FilterMap",
                "Road",
                TileId::fromValue(
                    static_cast<int32_t>(
                        *tileId))),
            "Road",
            "$features.*{typeId == 'Road' and roadId == locateRoadId}.id",
            {
                {
                    "locateRoadId",
                    *externalRoadId},
            })};
    }

    DataSourceInfo info_;
    TileId first_;
    TileId second_;
    std::optional<TileId> failingTile_;
    std::optional<TileId> limitingLifetimeTile_;
    mutable std::mutex mutex_;
    std::vector<TileId> requestedTiles_;
    std::atomic_size_t locateCalls_ = 0;
};

FeatureLayerFilterRequest filterDefinition()
{
    return FeatureLayerFilterRequest{
        .filterId_ = "style:roads",
        .generation_ = 4,
        .channels_ = {
            FeatureLayerFilterChannel{
                .channelId_ = "roads",
                .featureFilter_ = "enabled",
                .entryFilter_ = "typeId == 'Road'",
                .scope_ = FeatureLayerFilterScope::Feature,
                .featureTypes_ = {"Road"},
                .featureFields_ = {"typeId"},
                .geometryName_ = "centerline",
            },
            FeatureLayerFilterChannel{
                .channelId_ = "speed-limits",
                .featureFilter_ = "enabled",
                .entryFilter_ = "$name == 'speedLimit'",
                .scope_ = FeatureLayerFilterScope::Attribute,
                .featureTypes_ = {"Road"},
                .featureFields_ = {"typeId"},
                .entryFields_ = {"limit", "$hasValidity"},
            },
        },
        .bindings_ = {{"enabled", true}},
    };
}

} // namespace

TEST_CASE(
    "Service evaluates ordered filter channels after source tiles load",
    "[feature-layer-filter][Service]")
{
    auto cache = std::make_shared<MemCache>(32);
    Service service(cache, false);
    auto dataSource = std::make_shared<FilterDataSource>();
    service.add(dataSource);

    auto request =
        std::make_shared<FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{firstTile(), secondTile()},
            filterDefinition());

    std::mutex callbackMutex;
    std::vector<TileSubsetLayer::Ptr> results;
    std::vector<nlohmann::json> statuses;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr layer) {
            std::lock_guard lock(callbackMutex);
            results.push_back(std::move(layer));
        });
    request->onStatus(
        [&](nlohmann::json const& status) {
            std::lock_guard lock(callbackMutex);
            statuses.push_back(status);
        });

    REQUIRE(service.request(request));
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(results.size() == 2);
    REQUIRE(dataSource->requestedTiles() ==
            std::vector<TileId>{firstTile(), secondTile()});
    for (auto const& subset : results) {
        REQUIRE(subset->filterId() == "style:roads");
        REQUIRE(subset->generation() == 4);
        REQUIRE(subset->stringPoolId() ==
                "FilterServicePool");
        REQUIRE(subset->size() == 2);
        REQUIRE(subset->at(0)->channelId() == "roads");
        REQUIRE(subset->at(0)->scope() == Scope::Feature);
        REQUIRE(subset->at(0)->featureEntryCount() == 1);
        REQUIRE(
            subset->at(1)->channelId() ==
            "speed-limits");
        REQUIRE(subset->at(1)->scope() == Scope::Attribute);
        REQUIRE(
            subset->at(1)->attributeValidityEntryCount() ==
            1);
        REQUIRE(subset->localSourceFeatureCount() == 1);
        REQUIRE(
            subset->info()["Producer/backend"] ==
            "synthetic");
        REQUIRE_FALSE(subset->ttl());
    }
    REQUIRE_FALSE(statuses.empty());
    // Small requests need only their immediate Open and exact terminal
    // snapshots; per-tile load/evaluation/emission events are coalesced.
    REQUIRE(statuses.size() == 2);
    REQUIRE(statuses.front()["state"] == "Open");
    REQUIRE(statuses.back()["type"] == "mapget.filter.status");
    REQUIRE(statuses.back()["state"] == "Success");
    REQUIRE(statuses.back()["outputTilesRequested"] == 2);
    REQUIRE(statuses.back()["sourceTilesQueued"] == 2);
    REQUIRE(statuses.back()["sourceTilesLoaded"] == 2);
    REQUIRE(statuses.back()["sourceTilesEvaluated"] == 2);
    REQUIRE(statuses.back()["outputTilesReady"] == 2);
    REQUIRE(statuses.back()["entriesEmitted"] == 4);
    REQUIRE(statuses.back()["outputTilesEmitted"] == 2);
}

TEST_CASE(
    "Filter source requests inherit their work admission gate",
    "[feature-layer-filter][Service][backpressure]")
{
    auto service = Service(std::make_shared<MemCache>(32), false, std::chrono::milliseconds{0}, 1);
    auto dataSource = std::make_shared<FilterDataSource>();
    service.add(dataSource);
    auto admissionOpen = std::make_shared<std::atomic_bool>(false);
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "FilterMap",
        "Road",
        std::vector<TileId>{firstTile()},
        filterDefinition());
    request->setWorkAdmissionGate(admissionOpen);

    REQUIRE(service.request(request));
    auto liveRequest =
        std::make_shared<LayerTilesRequest>("FilterMap", "Road", std::vector<TileId>{secondTile()});
    REQUIRE(service.request(std::vector<LayerTilesRequest::Ptr>{liveRequest}));
    liveRequest->wait();
    REQUIRE(liveRequest->getStatus() == RequestStatus::Success);
    REQUIRE(dataSource->requestedTiles() == std::vector<TileId>{secondTile()});

    admissionOpen->store(true);
    service.notifyWorkAvailable();
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(dataSource->requestedTiles() == std::vector<TileId>{secondTile(), firstTile()});
}

TEST_CASE(
    "Filter evaluation does not wait for earlier source completion",
    "[feature-layer-filter][Service][concurrency]")
{
    using namespace std::chrono_literals;

    Service service(std::make_shared<MemCache>(32), false);
    auto dataSource =
        std::make_shared<OutOfOrderFilterDataSource>();
    service.add(dataSource);

    auto request =
        std::make_shared<FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{firstTile(), secondTile()},
            filterDefinition());
    std::mutex resultMutex;
    std::condition_variable resultChanged;
    std::vector<TileId> resultOrder;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr layer) {
            {
                std::lock_guard lock(resultMutex);
                resultOrder.push_back(layer->tileId());
            }
            resultChanged.notify_all();
        });

    REQUIRE(service.request(request));
    REQUIRE(dataSource->waitForSecondCompletion(2s));

    bool secondEmittedBeforeFirstCompleted = false;
    {
        std::unique_lock lock(resultMutex);
        secondEmittedBeforeFirstCompleted =
            resultChanged.wait_for(lock, 2s, [&] {
                return !resultOrder.empty();
            }) &&
            resultOrder.front() == secondTile();
    }
    auto const activeMemory = service.getMemoryStatistics();
    REQUIRE(activeMemory["active-filters"].size() == 1);
    auto const& filterMemory = activeMemory["active-filters"][0];
    REQUIRE(filterMemory["filter-id"] == "style:roads");
    REQUIRE(filterMemory["orchestration"]["current-bytes"].get<uint64_t>() > 0);
    REQUIRE(filterMemory["output-subset-models"]["peak-bytes"].get<uint64_t>() > 0);
    dataSource->releaseFirst();
    request->wait();

    REQUIRE(secondEmittedBeforeFirstCompleted);
    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(resultOrder ==
            std::vector<TileId>{secondTile(), firstTile()});
    REQUIRE(dataSource->requestedTiles() ==
            std::vector<TileId>{firstTile(), secondTile()});

    bool trackerReleased = false;
    for (size_t attempt = 0; attempt < 200; ++attempt) {
        if (service.getMemoryStatistics()["active-filters"].empty()) {
            trackerReleased = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(trackerReleased);
}

TEST_CASE(
    "Cached source tiles are evaluated before the worker advances",
    "[feature-layer-filter][Service][concurrency]")
{
    auto const tileIds = tileSequence(40);
    Service service(std::make_shared<MemCache>(128), false, std::chrono::milliseconds{0}, 1);
    auto dataSource = std::make_shared<FilterDataSource>();
    service.add(dataSource);

    auto warmup = std::make_shared<LayerTilesRequest>("FilterMap", "Road", tileIds);
    REQUIRE(service.request(std::vector<LayerTilesRequest::Ptr>{warmup}));
    warmup->wait();
    REQUIRE(warmup->getStatus() == RequestStatus::Success);

    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "FilterMap",
        "Road",
        tileIds,
        filterDefinition());
    std::vector<nlohmann::json> statuses;
    request->onStatus([&](nlohmann::json const& status) { statuses.push_back(status); });

    REQUIRE(service.request(request));
    request->wait();
    REQUIRE(request->getStatus() == RequestStatus::Success);

    auto intermediate = std::ranges::find_if(
        statuses,
        [](nlohmann::json const& status)
        {
            auto const state = status.value("state", "");
            return state != "Open" && state != "Success";
        });
    REQUIRE(intermediate != statuses.end());
    // A separate evaluation queue would let the cache-hit loop report many
    // loaded tiles before evaluating any of them. Tile-centric execution keeps
    // the first worker on each model until its contribution is committed.
    REQUIRE((*intermediate)["sourceTilesEvaluated"].get<size_t>() > 0);
}

TEST_CASE(
    "Filter processing releases the datasource permit",
    "[feature-layer-filter][Service][concurrency]")
{
    using namespace std::chrono_literals;

    Service service(std::make_shared<MemCache>(32), false, 0ms, 2);
    auto dataSource = std::make_shared<FilterDataSource>();
    service.add(dataSource);

    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "FilterMap",
        "Road",
        std::vector<TileId>{firstTile(), secondTile()},
        filterDefinition());
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool firstCallbackStarted = false;
    bool releaseFirstCallback = false;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr)
        {
            std::unique_lock lock(callbackMutex);
            if (firstCallbackStarted) {
                return;
            }
            firstCallbackStarted = true;
            callbackChanged.notify_all();
            callbackChanged.wait(lock, [&] { return releaseFirstCallback; });
        });

    REQUIRE(service.request(request));
    {
        std::unique_lock lock(callbackMutex);
        REQUIRE(callbackChanged.wait_for(lock, 2s, [&] { return firstCallbackStarted; }));
    }

    bool secondBackendCallStarted = false;
    for (size_t attempt = 0; attempt < 200; ++attempt) {
        if (dataSource->requestedTiles().size() == 2) {
            secondBackendCallStarted = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    {
        std::lock_guard lock(callbackMutex);
        releaseFirstCallback = true;
    }
    callbackChanged.notify_all();
    request->wait();

    REQUIRE(secondBackendCallStarted);
    REQUIRE(request->getStatus() == RequestStatus::Success);
}

TEST_CASE(
    "Filter cancellation detaches source work during inline result processing",
    "[feature-layer-filter][Service][concurrency][cancellation]")
{
    using namespace std::chrono_literals;

    Service service(std::make_shared<MemCache>(32), false, 0ms, 1);
    auto dataSource = std::make_shared<FilterDataSource>();
    service.add(dataSource);

    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "FilterMap",
        "Road",
        std::vector<TileId>{firstTile(), secondTile()},
        filterDefinition());
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool callbackStarted = false;
    bool releaseCallback = false;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr)
        {
            std::unique_lock lock(callbackMutex);
            callbackStarted = true;
            callbackChanged.notify_all();
            callbackChanged.wait(lock, [&] { return releaseCallback; });
        });

    REQUIRE(service.request(request));
    {
        std::unique_lock lock(callbackMutex);
        REQUIRE(callbackChanged.wait_for(lock, 2s, [&] { return callbackStarted; }));
    }

    service.abort(request);
    {
        std::lock_guard lock(callbackMutex);
        releaseCallback = true;
    }
    callbackChanged.notify_all();
    request->wait();
    REQUIRE(request->getStatus() == RequestStatus::Aborted);

    for (size_t attempt = 0; attempt < 200; ++attempt) {
        if (service.getMemoryStatistics()["active-filters"].empty()) {
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    FAIL("Cancelled inline filter evaluation retained request-owned memory");
}

TEST_CASE(
    "Service prunes ordinary outputs without notifying a stale in-flight result",
    "[Service][request-pruning]")
{
    using namespace std::chrono_literals;

    Service service(std::make_shared<MemCache>(32), false);
    auto dataSource = std::make_shared<BlockingResetDataSource>();
    service.add(dataSource);

    auto request = std::make_shared<LayerTilesRequest>(
        "FilterMap",
        "Road",
        std::vector<TileId>{firstTile(), secondTile()});
    std::vector<TileId> results;
    request->onFeatureLayer(
        [&](TileFeatureLayer::Ptr layer) { results.push_back(layer->tileId()); });

    REQUIRE(service.request(std::vector<LayerTilesRequest::Ptr>{request}));
    REQUIRE(dataSource->waitForFirstStart());
    service.retainOutputs(request, {secondTile()});
    dataSource->releaseFirst();
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(results == std::vector<TileId>{secondTile()});
    REQUIRE(
        dataSource->requestedTiles() ==
        std::vector<TileId>{firstTile(), secondTile()});
}

TEST_CASE(
    "Service prunes filter outputs while preserving retained source work",
    "[feature-layer-filter][Service][request-pruning]")
{
    Service service(std::make_shared<MemCache>(32), false);
    auto dataSource = std::make_shared<BlockingResetDataSource>();
    service.add(dataSource);

    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "FilterMap",
        "Road",
        std::vector<TileId>{firstTile(), secondTile()},
        filterDefinition());
    std::vector<TileId> results;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr layer) { results.push_back(layer->tileId()); });

    REQUIRE(service.request(request));
    REQUIRE(dataSource->waitForFirstStart());
    service.retainOutputs(request, {secondTile()});
    dataSource->releaseFirst();
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(results == std::vector<TileId>{secondTile()});
    REQUIRE(
        dataSource->requestedTiles() ==
        std::vector<TileId>{firstTile(), secondTile()});

    for (size_t attempt = 0; attempt < 200; ++attempt) {
        if (service.getMemoryStatistics()["active-filters"].empty()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FAIL("Pruned filter execution retained request-owned memory");
}

TEST_CASE(
    "Add-on composition keeps the earliest finite source lifetime",
    "[Service][add-on][ttl]")
{
    auto const baseTimestamp =
        std::chrono::system_clock::time_point{
            std::chrono::seconds{4'000'000'000}};
    auto const addOnTimestamp = baseTimestamp +
        std::chrono::seconds{1};
    Service service(std::make_shared<MemCache>(32), false);
    service.add(std::make_shared<LifetimeFilterDataSource>(
        "BaseLifetimePool",
        baseTimestamp,
        std::chrono::seconds{10}));
    service.add(std::make_shared<LifetimeFilterDataSource>(
        "AddOnLifetimePool",
        addOnTimestamp,
        std::chrono::seconds{2},
        true));

    auto request = std::make_shared<LayerTilesRequest>(
        "FilterMap",
        "Road",
        std::vector<TileId>{firstTile()});
    TileFeatureLayer::Ptr result;
    request->onFeatureLayer(
        [&](TileFeatureLayer::Ptr layer) {
            result = std::move(layer);
        });

    REQUIRE(service.request(
        std::vector<LayerTilesRequest::Ptr>{request}));
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(result);
    REQUIRE(result->timestamp() == addOnTimestamp);
    REQUIRE(result->ttl() == std::chrono::seconds{2});
}

TEST_CASE(
    "Service scans one source union and assigns cross-tile point groups canonically",
    "[feature-layer-filter][Service][point-group]")
{
    auto const westernTile =
        TileId::fromTileXY(10, 4, 4);
    auto const easternTile =
        westernTile.eastNeighbour();
    auto const boundary =
        westernTile.northEastWgs84().first;
    auto const latitude =
        westernTile.centerWgs84().second;
    auto dataSource =
        std::make_shared<PointGroupDataSource>(
            westernTile,
            easternTile,
            Point{boundary - 0.25, latitude, 0.0},
            Point{boundary + 0.25, latitude, 0.0},
            westernTile.neighbour(-1, 0));

    Service service(
        std::make_shared<MemCache>(64),
        false);
    service.add(dataSource);

    auto request =
        std::make_shared<FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{
                westernTile,
                easternTile},
            FeatureLayerFilterRequest{
                .filterId_ = "merged-roads",
                .generation_ = 3,
                .channels_ = {
                    FeatureLayerFilterChannel{
                        .channelId_ = "merged",
                        .featureFilter_ =
                            "typeId == 'Road'",
                        .scope_ =
                            FeatureLayerFilterScope::Feature,
                        .featureTypes_ = {"Road"},
                        .entryFields_ = {
                            "count($features.*)"},
                        .geometryTypes_ =
                            uint32_t{1}
                            << static_cast<uint8_t>(
                                   GeomType::Points),
                        .geometryName_ = "merge",
                        .group_ =
                            FeatureLayerPointGridGroup{
                                .origin_ = {
                                    boundary - 1.0,
                                    latitude - 1.0,
                                    -5.0},
                                .cellSize_ = {
                                    2.0,
                                    2.0,
                                    10.0},
                            },
                    },
                },
            },
            std::vector<TileId>{easternTile});

    std::vector<TileSubsetLayer::Ptr> results;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr layer) {
            results.push_back(std::move(layer));
        });
    REQUIRE(service.request(request));
    request->wait();
    REQUIRE(
        request->getStatus() ==
        RequestStatus::Success);

    auto const requestedSources =
        dataSource->requestedTiles();
    REQUIRE(requestedSources.size() == 12);
    REQUIRE(
        std::set<TileId>(
            requestedSources.begin(),
            requestedSources.end())
            .size() == 12);
    // The eastern priority propagates to its complete source halo. The first
    // requested output remains first, while first-needed dependency sources
    // are interleaved instead of accumulating after every output.
    REQUIRE(requestedSources[0] == westernTile);
    auto const easternSource = std::ranges::find(
        requestedSources,
        easternTile);
    REQUIRE(easternSource != requestedSources.end());
    REQUIRE(std::distance(requestedSources.begin(), easternSource) > 1);

    REQUIRE(results.size() == 2);
    TileSubsetLayer::Ptr westernResult;
    TileSubsetLayer::Ptr easternResult;
    for (auto const& result : results) {
        REQUIRE(result->dependencies().size() == 9);
        if (result->tileId() == westernTile) {
            westernResult = result;
        }
        else if (result->tileId() == easternTile) {
            easternResult = result;
        }
    }
    REQUIRE(westernResult);
    REQUIRE(easternResult);
    auto const expectedTimestamp =
        std::chrono::system_clock::time_point{
            std::chrono::seconds{4'000'000'000}};
    REQUIRE(westernResult->timestamp() == expectedTimestamp);
    REQUIRE(westernResult->ttl() == std::chrono::milliseconds{1250});
    REQUIRE(easternResult->timestamp() == expectedTimestamp);
    REQUIRE(easternResult->ttl() == std::chrono::seconds{10});
    REQUIRE(
        westernResult->at(0)->groupEntryCount() ==
        0);
    REQUIRE(
        easternResult->at(0)->groupEntryCount() ==
        1);
    model_ptr<GroupEntry> group;
    REQUIRE(
        easternResult->at(0)->forEachGroupEntry(
            [&](auto const& entry) {
                group = entry;
                return true;
            }));
    REQUIRE(group);
    REQUIRE(
        group->values()->toJson() ==
        nlohmann::json::array({2}));
    REQUIRE(
        group->memberFeatureIds()->size() == 2);
}

TEST_CASE(
    "Service resolves sparse cross-tile relations and keeps permanent ownership request-closed",
    "[feature-layer-filter][Service][relation]")
{
    auto const westernTile =
        TileId::fromTileXY(10, 5, 4);
    auto const easternTile =
        westernTile.eastNeighbour();
    auto relationFilter =
        FeatureLayerFilterRequest{
            .filterId_ = "relations",
            .generation_ = 5,
            .channels_ = {
                FeatureLayerFilterChannel{
                    .channelId_ = "connected",
                    .featureFilter_ =
                        "typeId == 'Road'",
                    .entryFilter_ = "$twoway",
                    .scope_ =
                        FeatureLayerFilterScope::Relation,
                    .featureTypes_ = {"Road"},
                    .featureFields_ = {"typeId"},
                    .entryFields_ = {"$twoway"},
                    .geometryTypes_ =
                        uint32_t{1}
                        << static_cast<uint8_t>(
                               GeomType::Points),
                    .geometryName_ = "relation",
                    .relation_ =
                        FeatureLayerStoredRelationOptions{
                            .relationNamePattern_ =
                                "connected",
                            .recursive_ = true,
                            .mergeTwoway_ = true,
                        },
                },
            },
        };

    auto execute =
        [&](TileId outputTile,
            bool exactRoot = false) {
            Service service(
                std::make_shared<MemCache>(32),
                false);
            auto dataSource =
                std::make_shared<
                    RelationDataSource>(
                    westernTile,
                    easternTile,
                    std::nullopt,
                    easternTile);
            service.add(dataSource);
            auto request =
                std::make_shared<
                    FeatureLayerFilterTilesRequest>(
                    "FilterMap",
                    "Road",
                    std::vector<TileId>{
                        outputTile},
                    relationFilter);
            if (exactRoot) {
                request->exactRoots_.push_back(
                    FeatureLayerFilterRoot{
                        outputTile,
                        "Road",
                        {
                            {
                                "tileId",
                                static_cast<int64_t>(
                                    outputTile
                                        .value())},
                            {
                                "roadId",
                                outputTile ==
                                        westernTile
                                    ? int64_t{1}
                                    : int64_t{2}},
                        },
                        0,
                    });
            }
            std::vector<TileSubsetLayer::Ptr>
                results;
            request->onFilterResult(
                [&](TileSubsetLayer::Ptr layer) {
                    results.push_back(
                        std::move(layer));
                });
            REQUIRE(service.request(request));
            request->wait();
            REQUIRE(
                request->getStatus() ==
                RequestStatus::Success);
            REQUIRE(results.size() == 1);
            REQUIRE(
                dataSource->requestedTiles() ==
                std::vector<TileId>{
                    outputTile,
                    outputTile == westernTile
                        ? easternTile
                        : westernTile});
            REQUIRE(dataSource->locateCalls() == 1);
            return results.front();
        };

    auto western = execute(westernTile);
    REQUIRE(western->dependencies().size() == 2);
    REQUIRE(
        western->timestamp() ==
        std::chrono::system_clock::time_point{
            std::chrono::seconds{4'000'000'000}});
    REQUIRE(western->ttl() == std::chrono::milliseconds{900});
    REQUIRE(
        western->at(0)->relationEntryCount() ==
        1);
    REQUIRE(
        western->at(0)->featureEntryCount() ==
        2);

    auto eastern = execute(easternTile);
    REQUIRE(eastern->dependencies().size() == 2);
    REQUIRE(
        eastern->at(0)->relationEntryCount() ==
        0);
    REQUIRE(
        eastern->at(0)->featureEntryCount() ==
        0);

    auto selectedEastern =
        execute(easternTile, true);
    REQUIRE(
        selectedEastern->at(0)
            ->relationEntryCount() == 1);
}

TEST_CASE(
    "First explicit root owns a cross-tile merged relation",
    "[feature-layer-filter][Service][relation]")
{
    auto const westernTile =
        TileId::fromTileXY(10, 5, 4);
    auto const easternTile =
        westernTile.eastNeighbour();
    Service service(
        std::make_shared<MemCache>(32),
        false);
    auto dataSource =
        std::make_shared<RelationDataSource>(
            westernTile,
            easternTile);
    service.add(dataSource);

    auto request =
        std::make_shared<
            FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{
                westernTile,
                easternTile},
            FeatureLayerFilterRequest{
                .filterId_ = "selected-relations",
                .generation_ = 1,
                .channels_ = {
                    FeatureLayerFilterChannel{
                        .channelId_ = "connected",
                        .featureFilter_ =
                            "typeId == 'Road'",
                        .entryFilter_ = "$twoway",
                        .scope_ =
                            FeatureLayerFilterScope::Relation,
                        .featureTypes_ = {"Road"},
                        .geometryName_ = "relation",
                        .relation_ =
                            FeatureLayerStoredRelationOptions{
                                .relationNamePattern_ =
                                    "connected",
                                .recursive_ = true,
                                .mergeTwoway_ = true,
                            },
                    },
                },
            });
    auto root = [](TileId tileId, int64_t roadId) {
        return FeatureLayerFilterRoot{
            tileId,
            "Road",
            {
                {
                    "tileId",
                    static_cast<int64_t>(
                        tileId.value())},
                {"roadId", roadId},
            },
            0,
        };
    };
    // Deliberately put the eastern root first. Explicit-root order, rather
    // than permanent south-west ownership or processing order, must win.
    request->exactRoots_ = {
        root(easternTile, 2),
        root(westernTile, 1),
    };

    std::vector<TileSubsetLayer::Ptr> results;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr layer) {
            results.push_back(std::move(layer));
        });
    REQUIRE(service.request(request));
    request->wait();
    REQUIRE(
        request->getStatus() ==
        RequestStatus::Success);
    REQUIRE(results.size() == 2);

    size_t westernRelations = 0;
    size_t easternRelations = 0;
    for (auto const& result : results) {
        auto const count =
            result->at(0)->relationEntryCount();
        if (result->tileId() == westernTile) {
            westernRelations = count;
        }
        else if (result->tileId() == easternTile) {
            easternRelations = count;
        }
    }
    REQUIRE(westernRelations == 0);
    REQUIRE(easternRelations == 1);
}

TEST_CASE(
    "Unavailable relation target tiles produce local issues instead of aborting the filter",
    "[feature-layer-filter][Service][relation][failure]")
{
    auto const westernTile =
        TileId::fromTileXY(10, 5, 4);
    auto const easternTile =
        westernTile.eastNeighbour();
    Service service(
        std::make_shared<MemCache>(32),
        false);
    auto dataSource =
        std::make_shared<RelationDataSource>(
            westernTile,
            easternTile,
            easternTile);
    service.add(dataSource);

    auto request =
        std::make_shared<
            FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{westernTile},
            FeatureLayerFilterRequest{
                .filterId_ =
                    "unavailable-relation-target",
                .generation_ = 1,
                .channels_ = {
                    FeatureLayerFilterChannel{
                        .channelId_ = "connected",
                        .featureFilter_ =
                            "typeId == 'Road'",
                        .entryFilter_ = "$twoway",
                        .scope_ =
                            FeatureLayerFilterScope::
                                Relation,
                        .featureTypes_ = {"Road"},
                        .geometryName_ = "relation",
                        .relation_ =
                            FeatureLayerStoredRelationOptions{
                                .relationNamePattern_ =
                                    "connected",
                                .recursive_ = true,
                                .mergeTwoway_ = true,
                            },
                    },
                },
            });
    std::vector<TileSubsetLayer::Ptr> results;
    request->onFilterResult(
        [&](TileSubsetLayer::Ptr layer) {
            results.push_back(std::move(layer));
        });

    REQUIRE(service.request(request));
    request->wait();
    REQUIRE(
        request->getStatus() ==
        RequestStatus::Success);
    REQUIRE(results.size() == 1);
    REQUIRE(
        results.front()->at(0)
            ->relationEntryCount() == 0);
    REQUIRE(
        std::ranges::any_of(
            results.front()->issues(),
            [](FilterIssue const& issue) {
                return issue.message_.find(
                    "Could not load relation target tile") !=
                    std::string::npos;
            }));
}

TEST_CASE(
    "Service resolves canonical locate ids through layer compositions",
    "[Service][locate]")
{
    Service service(
        std::make_shared<MemCache>(8),
        false);
    auto dataSource =
        std::make_shared<CanonicalLocateDataSource>();
    service.add(dataSource);

    auto planned = dataSource->locate(
        LocateRequest{
            "FilterMap",
            "Road",
            {
                {
                    "tileId",
                    static_cast<int64_t>(
                        firstTile()
                            .value())},
                {"roadId", int64_t{42}},
            }});
    REQUIRE(planned.size() == 1);
    REQUIRE(
        dataSource->requestedTiles()
            .empty());

    auto const canonicalId =
        fmt::format(
            "Road.{}.42",
            firstTile().value());
    LocateRequest request(
        nlohmann::json::object({
            {"mapId", "FilterMap"},
            {"featureId", canonicalId},
        }));
    auto responses = service.locate(request);

    REQUIRE(request.canonicalFeatureId_ == canonicalId);
    REQUIRE(responses.size() == 1);
    REQUIRE(dataSource->requestedTypeId_ == "Road");
    REQUIRE(
        dataSource->requestedFeatureId_ ==
        KeyValuePairs{
            {
                "tileId",
                static_cast<int64_t>(
                    firstTile().value())},
            {"roadId", int64_t{42}},
        });
    REQUIRE(
        responses.front().tileKey_ ==
        MapTileKey{
            LayerType::Features,
            "FilterMap",
            "Road",
            firstTile()});
    REQUIRE(
        dataSource->requestedTiles() ==
        std::vector<TileId>{
            firstTile()});
}

TEST_CASE(
    "Catalog source assertions preserve unique primary map addressing",
    "[feature-layer-filter][Service][source-id]")
{
    Service service(std::make_shared<MemCache>(32), false);
    auto primary = std::make_shared<FilterDataSource>();
    service.add(primary);

    auto catalog = service.sourceCatalog();
    REQUIRE(catalog.sources.size() == 1);
    auto const sourceId =
        catalog.sources.front().descriptor.sourceId;
    REQUIRE_FALSE(sourceId.empty());
    REQUIRE(sourceId != "FilterServicePool");

    auto matching =
        std::make_shared<FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{firstTile()},
            filterDefinition());
    matching->sourceId_ = sourceId;
    REQUIRE(service.request(matching));
    matching->wait();
    REQUIRE(
        matching->getStatus() ==
        RequestStatus::Success);

    auto mismatching =
        std::make_shared<FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{secondTile()},
            filterDefinition());
    mismatching->sourceId_ = "not-this-source";
    REQUIRE_FALSE(service.request(mismatching));
    REQUIRE(
        mismatching->getStatus() ==
        RequestStatus::NoDataSource);

    REQUIRE_THROWS(
        service.add(
            std::make_shared<FilterDataSource>(
                "SecondPrimaryPool")));
    auto addOn =
        std::make_shared<FilterDataSource>(
            "AddOnPool",
            true);
    REQUIRE_NOTHROW(service.add(addOn));
    auto catalogWithAddOn = service.sourceCatalog();
    auto addOnEntry = std::ranges::find_if(
        catalogWithAddOn.sources,
        [](auto const& entry) {
            return entry.info &&
                entry.info->isAddOn_;
        });
    REQUIRE(
        addOnEntry !=
        catalogWithAddOn.sources.end());
    auto addOnOnly =
        std::make_shared<FeatureLayerFilterTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{secondTile()},
            filterDefinition());
    addOnOnly->sourceId_ =
        addOnEntry->descriptor.sourceId;
    REQUIRE_FALSE(service.request(addOnOnly));
    REQUIRE(
        addOnOnly->getStatus() ==
        RequestStatus::NoDataSource);
}

TEST_CASE(
    "Datasource replacement invalidates cached map tiles",
    "[feature-layer-filter][Service][source-lifecycle]")
{
    auto cache = std::make_shared<MemCache>(32);
    Service service(cache, false);
    auto firstSource =
        std::make_shared<VersionedFilterDataSource>(
            "ReplacementPoolA",
            "first");
    service.add(firstSource);

    auto loadRevision =
        [&](std::string expectedRevision) {
            auto request =
                std::make_shared<LayerTilesRequest>(
                    "FilterMap",
                    "Road",
                    std::vector<TileId>{firstTile()});
            std::string actualRevision;
            request->onFeatureLayer(
                [&](TileFeatureLayer::Ptr layer) {
                    actualRevision =
                        layer->info().at(
                            "Producer/revision")
                            .get<std::string>();
                });
            REQUIRE(service.request(
                std::vector<LayerTilesRequest::Ptr>{
                    request}));
            request->wait();
            REQUIRE(
                request->getStatus() ==
                RequestStatus::Success);
            REQUIRE(actualRevision == expectedRevision);
        };

    loadRevision("first");
    service.remove(firstSource);
    auto secondSource =
        std::make_shared<VersionedFilterDataSource>(
            "ReplacementPoolB",
            "second");
    service.add(secondSource);
    loadRevision("second");
}

TEST_CASE(
    "Explicit map cache reset reloads only an authorized ready primary map",
    "[Service][Cache][authorization]")
{
    auto cache = std::make_shared<MemCache>(32);
    Service service(cache, false);
    auto source =
        std::make_shared<FilterDataSource>(
            "ExplicitResetPool");
    source->requireAuthHeaderRegexMatchOption(
        "X-USER-ROLE",
        std::regex("^resetter$"));
    service.add(source);

    auto load = [&] {
        auto request =
            std::make_shared<LayerTilesRequest>(
                "FilterMap",
                "Road",
                std::vector<TileId>{firstTile()});
        REQUIRE(service.request(
            std::vector<LayerTilesRequest::Ptr>{request},
            AuthHeaders{{"X-User-Role", "resetter"}}));
        request->wait();
        REQUIRE(
            request->getStatus() ==
            RequestStatus::Success);
    };

    load();
    load();
    REQUIRE(source->requestedTiles().size() == 1);
    REQUIRE_FALSE(service.resetMapCache(
        "UnknownMap",
        AuthHeaders{{"X-User-Role", "resetter"}}));
    REQUIRE_FALSE(service.resetMapCache(
        "FilterMap",
        AuthHeaders{}));
    REQUIRE(service.resetMapCache(
        "FilterMap",
        AuthHeaders{{"x-user-role", "resetter"}}));

    load();
    REQUIRE(source->requestedTiles().size() == 2);
}

TEST_CASE(
    "Explicit map cache reset rejects stale in-flight publication",
    "[Service][Cache][concurrency]")
{
    Service service(std::make_shared<MemCache>(32), false);
    auto source =
        std::make_shared<BlockingResetDataSource>();
    service.add(source);

    auto staleRequest =
        std::make_shared<LayerTilesRequest>(
            "FilterMap",
            "Road",
            std::vector<TileId>{firstTile()});
    REQUIRE(service.request(
        std::vector<LayerTilesRequest::Ptr>{
            staleRequest}));

    auto const firstStarted =
        source->waitForFirstStart();
    auto const resetSucceeded =
        firstStarted &&
        service.resetMapCache("FilterMap");
    source->releaseFirst();
    REQUIRE(firstStarted);
    REQUIRE(resetSucceeded);
    REQUIRE(
        staleRequest->getStatus() ==
        RequestStatus::Aborted);

    auto loadRevision = [&] {
        auto request =
            std::make_shared<LayerTilesRequest>(
                "FilterMap",
                "Road",
                std::vector<TileId>{firstTile()});
        std::string revision;
        request->onFeatureLayer(
            [&](TileFeatureLayer::Ptr layer) {
                revision = layer->info()
                    .at("Producer/revision")
                    .get<std::string>();
            });
        REQUIRE(service.request(
            std::vector<LayerTilesRequest::Ptr>{
                request}));
        request->wait();
        REQUIRE(
            request->getStatus() ==
            RequestStatus::Success);
        return revision;
    };

    REQUIRE(loadRevision() == "fresh");
    REQUIRE(source->fillCount() == 2);
    REQUIRE(loadRevision() == "fresh");
    REQUIRE(source->fillCount() == 2);
}

TEST_CASE(
    "Datasource failures terminally abort filter requests and clear in-flight work",
    "[feature-layer-filter][Service][failure]")
{
    Service service(std::make_shared<MemCache>(32), false);
    auto dataSource =
        std::make_shared<FailingFilterDataSource>();
    service.add(dataSource);

    auto run = [&]() {
        auto request =
            std::make_shared<FeatureLayerFilterTilesRequest>(
                "FilterMap",
                "Road",
                std::vector<TileId>{firstTile()},
                filterDefinition());
        REQUIRE(service.request(request));
        request->wait();
        REQUIRE(
            request->getStatus() ==
            RequestStatus::Aborted);
    };

    run();
    run();
    REQUIRE(dataSource->attempts() == 2);
}

TEST_CASE(
    "Interactive filter JSON preserves exact channel and generation identity",
    "[feature-layer-filter][tiles-request]")
{
    nlohmann::json envelope = {
        {"filterId", "styled-layer:17"},
        {"generation", 8},
        {"channels", {
            {
                {"channelId", "roads"},
                {"scope", "feature"},
                {"featureFilter", "enabled"},
                {"entryFilter", "typeId == 'Road'"},
                {"rewrite", false},
                {"featureTypes", {"Road"}},
                {"featureFields", {"typeId"}},
                {"entryFields", {"color"}},
                {"geometryTypes", 6},
                {"geometryName", "centerline"},
            },
            {
                {"channelId", "relations"},
                {"scope", "relation"},
                {"geometryName", "*"},
                {"relation", {
                    {"namePattern", "connected.*"},
                    {"recursive", true},
                    {"mergeTwoway", true},
                }},
            },
        }},
        {"bindings", {
            {"enabled", true},
            {"threshold", 12.5},
            {"missing", nullptr},
        }},
    };
    nlohmann::json request = {
        {"mapId", "FilterMap"},
        {"layerId", "Road"},
        {"sourceId", "optional-source-assertion"},
        {"tileIds", {
            firstTile().value(),
            secondTile().value(),
            firstTile().value(),
        }},
        {"roots", {
            {
                {"tileId", firstTile().value()},
                {"typeId", "Road"},
                {"featureId", {
                    "tileId", int64_t{1},
                    "roadId", int64_t{42},
                }},
            },
        }},
    };

    detail::inheritFilterFields(request, envelope);
    auto parsed =
        detail::parseLayerTilesRequestJson(request);

    REQUIRE(parsed.sourceId ==
            std::optional<std::string>{
                "optional-source-assertion"});
    REQUIRE(parsed.filterRequest.has_value());
    REQUIRE(parsed.exactRoots.size() == 1);
    REQUIRE(
        parsed.exactRoots[0].tileId_ ==
        firstTile());
    REQUIRE(
        parsed.exactRoots[0].typeId_ ==
        "Road");
    REQUIRE(parsed.filterRequest->filterId_ ==
            "styled-layer:17");
    REQUIRE(parsed.filterRequest->generation_ == 8);
    REQUIRE(parsed.filterRequest->channels_.size() == 2);
    auto const& feature =
        parsed.filterRequest->channels_[0];
    REQUIRE(feature.channelId_ == "roads");
    REQUIRE(feature.scope_ ==
            FeatureLayerFilterScope::Feature);
    REQUIRE(feature.geometryName_ ==
            std::optional<std::string>{"centerline"});
    auto const& relation =
        parsed.filterRequest->channels_[1];
    REQUIRE(relation.scope_ ==
            FeatureLayerFilterScope::Relation);
    REQUIRE_FALSE(relation.geometryName_.has_value());
    REQUIRE(relation.relation_.has_value());
    REQUIRE(relation.relation_->recursive_);
    REQUIRE(relation.relation_->mergeTwoway_);
    REQUIRE(
        detail::collectFilterTileIds(parsed) ==
        std::vector<TileId>{firstTile(), secondTile()});

    auto serialized =
        detail::filterRequestToJson(
            *parsed.filterRequest);
    REQUIRE(serialized["filterId"] ==
            "styled-layer:17");
    REQUIRE(serialized["generation"] == 8);
    REQUIRE_FALSE(serialized.contains("deliveryEpoch"));
    REQUIRE_FALSE(serialized.contains("deliveryEpochs"));
    REQUIRE(serialized["channels"].size() == 2);
    REQUIRE(
        serialized["channels"][1]["geometryName"] ==
        "*");
}

TEST_CASE(
    "Interactive filter JSON rejects removed delivery epoch fields",
    "[feature-layer-filter][tiles-request]")
{
    auto envelope = nlohmann::json::object({
        {"filterId", "styled-layer:17"},
        {"generation", 8},
        {"deliveryEpoch", 1},
        {"channels", nlohmann::json::array({{
            {"channelId", "roads"},
            {"scope", "feature"},
            {"entryFilter", "typeId == 'Road'"},
        }})},
    });
    nlohmann::json request = {
        {"mapId", "FilterMap"},
        {"layerId", "Road"},
        {"tileIds", {firstTile().value()}},
    };
    detail::inheritFilterFields(request, envelope);
    REQUIRE_THROWS_WITH(
        detail::parseLayerTilesRequestJson(request),
        Catch::Matchers::ContainsSubstring("deliveryEpoch"));

    envelope.erase("deliveryEpoch");
    request = {
        {"mapId", "FilterMap"},
        {"layerId", "Road"},
        {"tileIds", {firstTile().value()}},
        {"deliveryEpochs", nlohmann::json::array()},
    };
    detail::inheritFilterFields(request, envelope);
    REQUIRE_THROWS_WITH(
        detail::parseLayerTilesRequestJson(request),
        Catch::Matchers::ContainsSubstring("deliveryEpoch"));
}

TEST_CASE(
    "REST filter JSON keeps definition on its envelope",
    "[feature-layer-filter][tiles-request]")
{
    auto definition = detail::parseRestFilterEnvelopeJson({
        {"channels", {{
            {"channelId", "roads"},
            {"scope", "auto"},
            {"entryFilter", "Road"},
            {"rewrite", true},
        }}},
        {"bindings", {{"selected", false}}},
    });
    REQUIRE(definition.filterId_.empty());
    REQUIRE(definition.generation_ == 0);
    REQUIRE(definition.channels_.size() == 1);
    REQUIRE(definition.channels_[0].scope_ ==
            FeatureLayerFilterScope::Auto);

    nlohmann::json request = {
        {"mapId", "FilterMap"},
        {"layerId", "Road"},
        {"tileIds", {
            firstTile().value(),
            secondTile().value(),
        }},
    };
    auto parsed =
        detail::parseRestFilterLayerRequestJson(
            request,
            definition);
    REQUIRE(parsed.filterRequest.has_value());
    REQUIRE(
        detail::collectFilterTileIds(parsed) ==
        std::vector<TileId>{firstTile(), secondTile()});

    request["channels"] = nlohmann::json::array();
    REQUIRE_THROWS(
        detail::parseRestFilterLayerRequestJson(
            request,
            definition));

    request.erase("channels");
    request.erase("tileIds");
    request["tileIdsByNextStage"] =
        nlohmann::json::array();
    try {
        (void)detail::parseRestFilterLayerRequestJson(
            request,
            definition);
        FAIL("staged tile lists must be rejected");
    }
    catch (std::runtime_error const& error) {
        REQUIRE(
            std::string(error.what()) ==
            "tileIdsByNextStage is not supported; use tileIds");
    }
}

TEST_CASE(
    "Tile request parser preserves metadata source-data tile zero",
    "[tiles-request]")
{
    auto parsed = detail::parseLayerTilesRequestJson({
        {"mapId", "FilterMap"},
        {"layerId", "Metadata-RegistryMetadata"},
        {"tileIds", {0}},
    });
    REQUIRE(parsed.tileIds.size() == 1);
    REQUIRE(parsed.tileIds[0].value() == 0);

    REQUIRE_THROWS(
        detail::parseLayerTilesRequestJson({
            {"mapId", "FilterMap"},
            {"layerId", "Road"},
            {"tileIds", {0}},
        }));
}

TEST_CASE(
    "Tile request JSON rejects out-of-domain identities and scheduling hints",
    "[feature-layer-filter][tiles-request]")
{
    auto requireError = [](auto&& parse, std::string_view expected) {
        try {
            parse();
            FAIL("request must be rejected");
        }
        catch (std::runtime_error const& error) {
            REQUIRE(std::string_view(error.what()) == expected);
        }
    };

    auto request = nlohmann::json{
        {"mapId", "FilterMap"},
        {"layerId", "Road"},
        {"tileIds", {
            firstTile().value(),
        }},
    };

    request["priorityTileIds"] = {
        secondTile().value(),
    };
    requireError(
        [&] {
            (void)detail::parseLayerTilesRequestJson(
                request);
        },
        "priorityTileIds must be contained in tileIds");

    request.erase("priorityTileIds");
    request["roots"] = {{
        {"tileId", secondTile().value()},
        {"typeId", "Road"},
        {"featureId", {
            "tileId", int64_t{1},
            "roadId", int64_t{42},
        }},
    }};
    requireError(
        [&] {
            (void)detail::parseLayerTilesRequestJson(
                request);
        },
        "roots tileId values must be contained in tileIds");

    request["roots"][0]["tileId"] =
        firstTile().value();
    request["roots"][0]["featureId"][3] =
        std::numeric_limits<uint64_t>::max();
    requireError(
        [&] {
            (void)detail::parseLayerTilesRequestJson(
                request);
        },
        "roots featureId integer values exceed the signed 64-bit model domain");

    request.erase("roots");
    request["tileIds"] = {
        std::numeric_limits<uint64_t>::max(),
    };
    requireError(
        [&] {
            (void)detail::parseLayerTilesRequestJson(
                request);
        },
        "tile IDs must be signed 32-bit integers");
}

TEST_CASE(
    "Service transfers only the attachment named by the source tile",
    "[attachment][Service]")
{
    auto source =
        std::make_shared<AttachmentFilterDataSource>();
    Service service;
    service.add(source);

    auto request = AttachmentRequest{
        .tileKey_ = MapTileKey(
            LayerType::Features,
            "FilterMap",
            "Road",
            firstTile()),
        .name_ = "synthetic.glb",
    };
    auto result = service.attachment(request);
    REQUIRE(result.status_ == RequestStatus::Success);
    REQUIRE(result.response_);
    REQUIRE(
        result.response_->name_ ==
        "synthetic.glb");
    REQUIRE(
        result.response_->mimeType_ ==
        "model/gltf-binary");
    REQUIRE(result.response_->bytes_);
    REQUIRE(
        *result.response_->bytes_ ==
        std::vector<uint8_t>{
            0x67,
            0x6c,
            0x54,
            0x46});
    REQUIRE(
        result.response_->etag_ ==
        std::optional<std::string>{
            "\"synthetic\""});
    REQUIRE(source->attachmentCalls() == 1);

    request.name_ = "other.glb";
    result = service.attachment(request);
    REQUIRE(result.status_ == RequestStatus::Success);
    REQUIRE_FALSE(result.response_);
    REQUIRE(source->attachmentCalls() == 1);
}
