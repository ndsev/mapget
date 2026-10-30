#include "test-http-service-fixture.h"

#include "mapget/http-datasource/datasource-client.h"
#include "mapget/http-datasource/detail/schema-response.h"
#include "mapget/http-service/http-client.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/stream.h"
#include "process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>

using namespace mapget;

namespace
{

/** Keep the format fixed while varying a field: format equality is not artifact identity. */
class SchemaTestSource final : public DataSource
{
public:
    explicit SchemaTestSource(std::string map, std::string field = "speed", bool addOn = false)
    {
        auto metadata = nlohmann::json::parse(R"({
            "mapFormat": {"name": "schema-http-test", "version": "1"},
            "layers": {
                "Road": {
                    "type": "Features",
                    "featureTypes": [{
                        "name": "Road",
                        "uniqueIdCompositions": [[{"partId": "id", "datatype": "U32"}]]
                    }],
                    "featureModelSchema": {"type": "object"}
                }
            }
        })");
        metadata["mapId"] = map;
        metadata["stringPoolId"] = map + "-" + field + "-strings";
        metadata["addOn"] = addOn;
        metadata["layers"]["Road"]["featureModelSchema"]["properties"] = {{field, {{"type", "integer"}}}};
        info_ = DataSourceInfo::fromJson(metadata);
    }

    DataSourceInfo info() override { return info_; }
    void fill(PartitionFeatureLayer::Ptr const&) override {}
    void fill(PartitionSourceDataLayer::Ptr const&) override {}

private:
    DataSourceInfo info_;
};

/** Own test registrations and a separate client event loop; cleanup survives failed assertions. */
class SchemaHttpFixture
{
public:
    SchemaHttpFixture() : service_(test::httpService()), loop_("SchemaHttpTest")
    {
        loop_.run();
        client_ = drogon::HttpClient::newHttpClient(
            "http://127.0.0.1:" + std::to_string(service_.port()), loop_.getLoop());
    }

    ~SchemaHttpFixture()
    {
        for (auto const& source : sources_)
            service_.remove(source);
    }

    std::shared_ptr<SchemaTestSource> add(
        std::string map, std::string field = "speed", bool gated = false, bool addOn = false)
    {
        auto source = std::make_shared<SchemaTestSource>(std::move(map), std::move(field), addOn);
        if (gated)
            source->requireAuthHeaderRegexMatchOption("x-schema-access", std::regex("^allowed$"));
        service_.add(source);
        sources_.push_back(source);
        return source;
    }

    std::string sourceId(DataSource::Ptr const& source) const
    {
        for (auto const& row : service_.sourceCatalog().sources) {
            if (row.dataSource == source)
                return row.descriptor.sourceId;
        }
        throw std::runtime_error("Test source is absent from the catalog.");
    }

    drogon::HttpResponsePtr get(
        std::string path,
        std::vector<std::pair<std::string, std::string>> parameters = {},
        AuthHeaders headers = {})
    {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPath(std::move(path));
        for (auto const& [key, value] : parameters)
            request->setParameter(key, value);
        for (auto const& [key, value] : headers)
            request->addHeader(key, value);
        auto [status, response] = client_->sendRequest(request, 10.0);
        REQUIRE(status == drogon::ReqResult::Ok);
        REQUIRE(response);
        return response;
    }

    HttpService& service_;

private:
    std::vector<DataSource::Ptr> sources_;
    trantor::EventLoopThread loop_;
    drogon::HttpClientPtr client_;
};

/** Launch the existing test executable in its isolated schema-protocol peer mode. */
class SchemaPeerProcess
{
public:
    explicit SchemaPeerProcess(bool holdSchema = false)
    {
        auto port = ready_.get_future();
        process_ = std::make_unique<TinyProcessLib::Process>(
            std::string("\"") + MAPGET_TEST_DATASOURCE_SERVER_EXE + "\" --schema-http-fixture" +
                (holdSchema ? " --hold-schema" : ""), "",
            [this](char const* data, size_t size) {
                std::lock_guard lock(mutex_);
                if (published_)
                    return;
                output_.append(data, size);
                for (;;) {
                    auto end = output_.find_first_of("\r\n");
                    if (end == std::string::npos)
                        break;
                    auto line = output_.substr(0, end);
                    output_.erase(0, end + 1);
                    std::smatch match;
                    if (std::regex_search(line, match, std::regex("Running on port ([0-9]+)"))) {
                        published_ = true;
                        ready_.set_value(static_cast<uint16_t>(std::stoul(match[1].str())));
                        return;
                    }
                }
            }, nullptr, true);
        if (port.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            process_->kill(true);
            process_->get_exit_status();
            throw std::runtime_error("Schema test peer failed to start.");
        }
        port_ = port.get();
    }

    ~SchemaPeerProcess()
    {
        process_->kill(true);
        process_->get_exit_status();
    }

    uint16_t port() const { return port_; }

private:
    std::mutex mutex_;
    std::promise<uint16_t> ready_;
    bool published_ = false;
    std::string output_;
    uint16_t port_ = 0;
    std::unique_ptr<TinyProcessLib::Process> process_;
};

/** Consume the same shared-byte stream callback used by Drogon, without starting another server. */
std::string drainSchema(drogon::HttpResponsePtr const& response)
{
    auto const& stream = response->streamCallback();
    REQUIRE(stream);
    std::string result;
    std::array<char, 257> buffer;
    while (auto size = stream(buffer.data(), buffer.size()))
        result.append(buffer.data(), size);
    stream(nullptr, 0);
    return result;
}

} // namespace

TEST_CASE("HTTP schema RPC binds compact catalogs to exact binary artifacts", "[HttpSchema]")
{
    SchemaHttpFixture fixture;
    auto source = fixture.add("SchemaHttpRoundtrip");
    auto info = source->info();
    auto sourceId = fixture.sourceId(source);
    auto catalogResponse = fixture.get("/sources");
    REQUIRE(catalogResponse->statusCode() == drogon::k200OK);
    auto catalog = nlohmann::json::parse(catalogResponse->body());
    auto row = std::ranges::find_if(catalog, [&](auto const& item) {
        return item.at("sourceId") == sourceId;
    });
    REQUIRE(row != catalog.end());
    REQUIRE(row->at("schemaId") == info.schemaId_);
    REQUIRE(row->at("mapFormat") == info.mapFormat_.toJson());
    auto const& layer = row->at("layers").at("Road");
    CHECK_FALSE(layer.contains("featureTypes"));
    CHECK_FALSE(layer.contains("schemaFeatureTypes"));
    CHECK_FALSE(layer.contains("featureModelSchema"));

    auto response = fixture.get("/schema", {{"sourceId", sourceId}, {"schemaId", info.schemaId_}});
    REQUIRE(response->statusCode() == drogon::k200OK);
    CHECK(response->contentTypeString().starts_with("application/octet-stream"));
    CHECK(response->getHeader("cache-control").find("private") != std::string::npos);
    CHECK(response->getHeader("cache-control").find("no-cache") != std::string::npos);
    auto body = response->body();
    auto bytes = std::span(reinterpret_cast<uint8_t const*>(body.data()), body.size());
    auto schema = MapSchema::read(bytes, info.schemaId_);
    CHECK(schema->format() == info.mapFormat_);
    REQUIRE(schema->layer("Road"));
    REQUIRE(schema->layer("Road")->getTypeInfo("Road"));
    CHECK(schema->layer("Road")->getTypeInfo("Road")->name_ == "Road");
    CHECK_THROWS(MapSchema::read(bytes, "not-the-expected-contract"));
    auto corrupted = std::string(body);
    REQUIRE_FALSE(corrupted.empty());
    corrupted.back() ^= 1;
    CHECK_THROWS(MapSchema::read(
        {reinterpret_cast<uint8_t const*>(corrupted.data()), corrupted.size()}, info.schemaId_));

    auto etag = response->getHeader("etag");
    REQUIRE_FALSE(etag.empty());
    for (auto const& validator : {etag, etag.substr(2), std::string("\"other\", ") + etag, std::string("*")}) {
        auto unchanged = fixture.get("/schema", {{"sourceId", sourceId}, {"schemaId", info.schemaId_}},
                                     {{"If-None-Match", validator}});
        CHECK(unchanged->statusCode() == drogon::k304NotModified);
        CHECK(unchanged->getHeader("etag") == etag);
        CHECK(unchanged->body().empty());
    }
    auto quotedComma = fixture.get("/schema", {{"sourceId", sourceId}, {"schemaId", info.schemaId_}},
                                   {{"If-None-Match", "\"other, *, still-other\""}, {"Connection", "close"}});
    CHECK(quotedComma->statusCode() == drogon::k200OK);
    CHECK(quotedComma->body() == body);

    auto status = nlohmann::json::parse(fixture.get("/status-data")->body());
    auto const& transport = status.at("memory").at("transport");
    auto const& schemas = transport.at("schema-artifacts");
    CHECK(schemas.at("max-artifact-bytes") == detail::SchemaResponseCache::MaxArtifactBytes);
    CHECK(schemas.at("retained-cache-capacity-bytes").get<size_t>() >= body.size());
    CHECK(schemas.at("allocated-bytes").get<size_t>() >= schemas.at("retained-cache-capacity-bytes").get<size_t>());
    CHECK(transport.at("current-bytes").get<uint64_t>() ==
          schemas.at("allocated-bytes").get<uint64_t>() +
          transport.at("rest-streams").at("pending-capacity-bytes").get<uint64_t>() +
          transport.at("interactive").at("pending-controller-allocated-bytes").get<uint64_t>());
}

TEST_CASE("Schema cache and conditional requests never bypass source authorization", "[HttpSchema]")
{
    SchemaHttpFixture fixture;
    auto source = fixture.add("SchemaHttpPrivate", "speed", true);
    auto info = source->info();
    auto id = fixture.sourceId(source);
    std::vector<std::pair<std::string, std::string>> query{{"sourceId", id}, {"schemaId", info.schemaId_}};
    auto response = fixture.get("/schema", query, {{"x-schema-access", "allowed"}});
    REQUIRE(response->statusCode() == drogon::k200OK);
    auto etag = response->getHeader("etag");

    for (auto const& validator : {etag, std::string("*")}) {
        auto denied = fixture.get("/schema", query, {{"If-None-Match", validator}});
        CHECK(denied->statusCode() == drogon::k404NotFound);
        CHECK(denied->getHeader("etag").empty());
        CHECK(denied->getHeader("cache-control") == "no-store");
    }
    CHECK_FALSE(fixture.service_.getSchema(id, info.schemaId_, AuthHeaders{}));
    CHECK(fixture.service_.getSchema(id, info.schemaId_, AuthHeaders{{"x-schema-access", "allowed"}}) == info.mapSchema_);
    CHECK(fixture.get("/schema", {{"schemaId", info.schemaId_}})->statusCode() == drogon::k400BadRequest);
    CHECK(fixture.get("/schema", {{"sourceId", id}})->statusCode() == drogon::k400BadRequest);

    HttpClient authorized("127.0.0.1", fixture.service_.port(), {{"x-schema-access", "allowed"}});
    auto first = authorized.getSchema(id, info.schemaId_);
    auto second = authorized.getSchema(id, info.schemaId_);
    REQUIRE(first);
    CHECK(first == second);
    HttpClient anotherAuthorized("127.0.0.1", fixture.service_.port(), {{"x-schema-access", "allowed"}});
    CHECK(anotherAuthorized.getSchema(id, info.schemaId_) == first);
    HttpClient unauthorized("127.0.0.1", fixture.service_.port());
    CHECK_THROWS(unauthorized.getSchema(id, info.schemaId_));
    fixture.service_.remove(source);
    CHECK_THROWS(authorized.getSchema(id, info.schemaId_));
    CHECK(fixture.get("/schema", query, {{"x-schema-access", "allowed"}, {"If-None-Match", etag}})->statusCode() == drogon::k404NotFound);
}

TEST_CASE("Schema RPC does not substitute a matching MapFormat for the requested binding", "[HttpSchema]")
{
    SchemaHttpFixture fixture;
    auto first = fixture.add("SchemaHttpFormatA", "speed");
    auto second = fixture.add("SchemaHttpFormatB", "width");
    auto a = first->info();
    auto b = second->info();
    REQUIRE(a.mapFormat_ == b.mapFormat_);
    REQUIRE(a.schemaId_ != b.schemaId_);
    auto id = fixture.sourceId(first);
    auto mismatch = fixture.get("/schema", {{"sourceId", id}, {"schemaId", b.schemaId_}}, {{"If-None-Match", "*"}});
    CHECK(mismatch->statusCode() == drogon::k404NotFound);
    auto correct = fixture.get("/schema", {{"sourceId", id}, {"schemaId", a.schemaId_}, {"mapFormat", b.mapFormat_.toJson().dump()}});
    CHECK(correct->statusCode() == drogon::k200OK);
}

TEST_CASE("Encoded schema streams survive cache eviction and decoded-owner destruction", "[HttpSchema]")
{
    detail::SchemaResponseCache cache;
    std::weak_ptr<MapSchema const> owner;
    std::string id;
    drogon::HttpResponsePtr pending;
    {
        SchemaTestSource source("SchemaStreamOwner", "speed");
        auto info = source.info();
        id = info.schemaId_;
        owner = info.mapSchema_;
        pending = cache.response(info.mapSchema_, {});
        REQUIRE(cache.retainedCapacity() > 0);
    }
    CHECK(owner.expired());
    auto const firstCapacity = cache.retainedCapacity();
    CHECK(cache.allocatedCapacity() == firstCapacity);
    auto replacement = SchemaTestSource("SchemaStreamOther", "width").info().mapSchema_;
    auto other = cache.response(replacement, {});
    CHECK(cache.allocatedCapacity() == firstCapacity + cache.retainedCapacity());
    auto shared = cache.response(replacement, {});
    CHECK(cache.allocatedCapacity() == firstCapacity + cache.retainedCapacity());
    auto bytes = drainSchema(pending);
    CHECK(cache.allocatedCapacity() == cache.retainedCapacity());
    auto decoded = MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()}, id);
    CHECK(decoded->schemaId() == id);
    CHECK_FALSE(drainSchema(other).empty());
    CHECK_FALSE(drainSchema(shared).empty());
    CHECK(cache.allocatedCapacity() == cache.retainedCapacity());
}

TEST_CASE("HTTP clients retain distinct source IDs for primary and add-on schemas", "[HttpSchema]")
{
    SchemaHttpFixture fixture;
    auto primary = fixture.add("SchemaHttpSharedMap");
    auto addon = fixture.add("SchemaHttpSharedMap", "addon", false, true);
    HttpClient client("127.0.0.1", fixture.service_.port());
    auto catalog = client.sourceCatalog();
    for (auto const& source : {primary, addon}) {
        auto id = fixture.sourceId(source);
        auto row = std::ranges::find_if(catalog, [&](auto const& item) { return item.at("sourceId") == id; });
        REQUIRE(row != catalog.end());
        CHECK(row->at("mapId") == "SchemaHttpSharedMap");
        auto schema = client.getSchema(id, source->info().schemaId_);
        CHECK(schema->schemaId() == source->info().schemaId_);
    }
}

TEST_CASE("Remote datasource initialization resolves schema before exposing metadata", "[HttpSchema][HttpDataSource]")
{
    // The listener must exist before launching a peer; Drogon owns one process-global server.
    test::httpService();
    RemoteDataSourceProcess remote(std::string("\"") + MAPGET_TEST_DATASOURCE_SERVER_EXE + "\"");
    auto info = remote.info();
    REQUIRE(info.mapSchema_);
    REQUIRE_NOTHROW(info.validateSchema());
    REQUIRE(info.getLayer("WayLayer")->layerSchema());
    auto json = info.toJson();
    CHECK(json.at("schemaId") == info.mapSchema_->schemaId());
    for (auto const& layer : json.at("layers")) {
        CHECK_FALSE(layer.contains("featureModelSchema"));
        CHECK_FALSE(layer.contains("featureTypes"));
    }
}

TEST_CASE("HTTP clients reject ambiguous source metadata unless a source is selected", "[HttpSchema]")
{
    test::httpService();
    SchemaPeerProcess peer;
    HttpClient client("127.0.0.1", peer.port(), {{"x-schema-test-mode", "ambiguous"}});
    REQUIRE(client.sourceCatalog().size() == 2);
    auto request = std::make_shared<LayerTilesRequest>(
        "SchemaPeerMap", "Road", std::vector<PartitionId>{PartitionId::fromValue(131073)});
    client.request(request);
    CHECK(request->getStatus() == RequestStatus::Aborted);

    for (auto const& id : {"schema-peer-0", "schema-peer-1"}) {
        auto selected = std::make_shared<LayerTilesRequest>(
            "SchemaPeerMap", "Road", std::vector<PartitionId>{PartitionId::fromValue(131073)});
        selected->sourceId_ = id;
        client.request(selected);
        CHECK(selected->getStatus() == RequestStatus::Success);
    }
}

TEST_CASE("HTTP schema decoding rejects wrong and corrupt artifacts without poisoning retries", "[HttpSchema]")
{
    test::httpService();
    SchemaPeerProcess peer;
    CHECK_THROWS(HttpClient("127.0.0.1", peer.port(), {{"x-schema-test-mode", "wrong-artifact"}}));
    CHECK_THROWS(HttpClient("127.0.0.1", peer.port(), {{"x-schema-test-mode", "corrupt-artifact"}}));
    HttpClient valid("127.0.0.1", peer.port());
    REQUIRE(valid.sources().size() == 1);
    CHECK_NOTHROW(valid.sources().front().validateSchema());
}

TEST_CASE("HTTP-decoded layers outlive the client without retaining its schema resolver", "[HttpSchema][layer-binding]")
{
    test::httpService();
    SchemaPeerProcess peer;
    PartitionFeatureLayer::Ptr escaped;
    {
        HttpClient client("127.0.0.1", peer.port());
        auto request = std::make_shared<LayerTilesRequest>(
            "SchemaPeerMap", "Road", std::vector<PartitionId>{PartitionId::fromValue(131073)});
        request->onFeatureLayer([&](auto const& tile) { escaped = tile; });
        client.request(request);
        REQUIRE(request->getStatus() == RequestStatus::Success);
        REQUIRE(escaped);
    }
    REQUIRE(escaped->schemaBinding(escaped->schemaId()));
    CHECK_FALSE(escaped->schemaBinding("uninstalled-after-client-destruction"));
}

TEST_CASE("Remote datasource cancellation is checked before connection setup", "[HttpSchema]")
{
    CHECK_THROWS_AS(RemoteDataSource("127.0.0.1", 1, [] { return true; }), TileLoadCancelled);
}

TEST_CASE("Remote schema wait cancellation leaves shared loading available to other sources", "[HttpSchema][HttpDataSource]")
{
    using namespace std::chrono_literals;
    test::httpService();
    SchemaPeerProcess peer(true);
    trantor::EventLoopThread loop("SchemaWaitControl");
    loop.run();
    auto client = drogon::HttpClient::newHttpClient(
        "http://127.0.0.1:" + std::to_string(peer.port()), loop.getLoop());
    auto control = [&](drogon::HttpMethod method = drogon::Get) {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(method);
        request->setPath("/schema-test");
        auto [status, response] = client->sendRequest(request, 2.0);
        REQUIRE(status == drogon::ReqResult::Ok);
        REQUIRE(response);
        REQUIRE(response->statusCode() == drogon::k200OK);
        return nlohmann::json::parse(response->body());
    };

    auto cancel = std::make_shared<std::atomic_bool>(false);
    // Unlike std::async, these futures never join a blocked initializer during assertion unwind.
    // Detached tasks retain only owned state; destroying the peer closes any unfinished request.
    std::packaged_task<std::shared_ptr<RemoteDataSource>()> firstTask([port = peer.port(), cancel] {
        return std::make_shared<RemoteDataSource>("127.0.0.1", port,
            [cancel] { return cancel->load(); });
    });
    auto first = firstTask.get_future();
    std::thread(std::move(firstTask)).detach();
    auto deadline = std::chrono::steady_clock::now() + 5s;
    auto state = control();
    while (state.at("pending") == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        state = control();
    }
    REQUIRE(state.at("pending") == 1);

    std::packaged_task<std::shared_ptr<RemoteDataSource>()> secondTask([port = peer.port()] {
        return std::make_shared<RemoteDataSource>("127.0.0.1", port);
    });
    auto second = secondTask.get_future();
    std::thread(std::move(secondTask)).detach();
    deadline = std::chrono::steady_clock::now() + 5s;
    do {
        state = control();
        if (state.at("probes") >= 2)
            break;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    auto bothAuthorized = state.at("probes") == 2;
    cancel->store(true);
    auto abandonedBeforeRelease = first.wait_for(1s) == std::future_status::ready;
    auto otherStillWaiting = second.wait_for(0s) == std::future_status::timeout;
    // Release first even if cancellation regresses, so subsequent assertions cannot strand work.
    state = control(drogon::Post);
    CHECK(bothAuthorized);
    CHECK(abandonedBeforeRelease);
    CHECK(otherStillWaiting);
    CHECK(state.at("fetches") == 1);
    REQUIRE(first.wait_for(5s) == std::future_status::ready);
    CHECK_THROWS_AS(first.get(), TileLoadCancelled);
    REQUIRE(second.wait_for(5s) == std::future_status::ready);
    auto remaining = second.get();
    REQUIRE(remaining);
    auto info = remaining->info();
    REQUIRE(info.mapSchema_);
    CHECK(info.schemaId_ == state.at("schemaId").get<std::string>());
    CHECK_NOTHROW(info.validateSchema());
    CHECK(control().at("fetches") == 1);
}
