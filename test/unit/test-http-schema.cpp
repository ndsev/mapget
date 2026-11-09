#include "test-http-service-fixture.h"
#include "../../libs/http-service/src/mcp-mapget-tools.h"

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
    explicit SchemaTestSource(
        std::string map,
        std::string field = "speed",
        bool addOn = false,
        size_t descriptionBytes = 0,
        bool producedFeatureRoot = false)
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
        metadata["layers"]["Road"]["featureModelSchema"]["properties"] = {
            {field, {{"type", "integer"}}}};
        metadata["layers"]["Road"]["featureTypes"][0]["uniqueIdCompositions"][0][0]["description"] =
            std::string(descriptionBytes, 'x');
        info_ = DataSourceInfo::fromJson(metadata);
        if (producedFeatureRoot) {
            // The explorer follows produced roots, not an unrelated legacy schema fragment.
            auto owner = std::make_shared<MapSchema>(info_.mapFormat_);
            auto layer = owner->newLayer("Road", LayerType::Features);
            auto root = layer->addSchema(LayerSchema::FeatureKind);
            layer->addFeatureType(
                FeatureTypeInfo::fromJson(metadata["layers"]["Road"]["featureTypes"][0]),
                root);
            auto properties = layer->addSchema(simfil::SchemaType::Kind::Object);
            owner->schemaType(root)
                .asObject()
                .addField("properties", owner->schemaType(properties));
            owner->schemaType(properties)
                .asObject()
                .addField(
                    field,
                    owner->schemaType(layer->addSchema(simfil::SchemaType::Kind::Int)));
            owner->finalize();
            info_.schemaId_.clear();
            info_.setMapSchema(std::move(owner));
        }
    }

    /** Install a purpose-built native graph before the fixture registers the source. */
    SchemaTestSource(std::string map, MapSchema::Ptr schema) : SchemaTestSource(std::move(map))
    {
        info_.schemaId_.clear();
        info_.setMapSchema(std::move(schema));
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

    std::shared_ptr<SchemaTestSource>
    add(std::string map,
        std::string field = "speed",
        bool gated = false,
        bool addOn = false,
        size_t descriptionBytes = 0,
        bool producedFeatureRoot = false)
    {
        auto source = std::make_shared<SchemaTestSource>(
            std::move(map),
            std::move(field),
            addOn,
            descriptionBytes,
            producedFeatureRoot);
        if (gated)
            source->requireAuthHeaderRegexMatchOption("x-schema-access", std::regex("^allowed$"));
        return add(std::move(source));
    }

    std::shared_ptr<SchemaTestSource> add(std::shared_ptr<SchemaTestSource> source)
    {
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
    CHECK(schema->layer("Road")->getTypeInfo("Road").name() == "Road");
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
        auto schema = client.getSchema(id, row->at("schemaId").get<std::string>());
        CHECK(schema->schemaId() == row->at("schemaId").get<std::string>());
        if (source == primary)
            CHECK(schema->schemaId() != source->info().schemaId_);
        else
            CHECK(schema->schemaId() == source->info().schemaId_);
    }
}

TEST_CASE("Schema explorer serves authorized inventory and lazy type links", "[HttpSchema]")
{
    SchemaHttpFixture fixture;
    auto source = fixture.add("SchemaExplorer");
    auto hidden = fixture.add("SchemaExplorerHidden", "private", true);
    auto response = fixture.get("/schemas");
    REQUIRE(response->statusCode() == drogon::k200OK);
    auto inventory = nlohmann::json::parse(response->body());
    CHECK(inventory.dump().find(hidden->info().schemaId_) == std::string::npos);
    auto found = std::ranges::find_if(
        inventory["schemas"],
        [&](auto const& item) { return item["mapSchemaId"] == source->info().schemaId_; });
    REQUIRE(found != inventory["schemas"].end());
    CHECK(found->at("ownerInstances") == 1);
    auto const& counts = found->at("statistics").at("counts");
    CHECK(
        counts.at("types").get<uint64_t>() ==
        counts.at("physicalTypes").get<uint64_t>() + counts.at("queryContexts").get<uint64_t>());
    CHECK(found->at("statistics").at("limits").at("typeId") == simfil::MaxTypeId);
    REQUIRE(found->at("layers").size() == 1);
    auto const& layer = found->at("layers")[0];
    CHECK(layer.at("category") == "Features");
    CHECK(layer.contains("typeId"));
    CHECK(layer.at("open").is_boolean());
    CHECK(layer.at("reachabilityComplete").is_boolean());
    CHECK_FALSE(layer.contains("schemaId"));
    auto id = source->info().mapSchema_->layer("Road")->featureSchema("Road");
    std::vector<std::pair<std::string, std::string>> parameters{
        {"mapSchemaId", source->info().schemaId_},
        {"typeId", std::to_string(id)}};
    auto node = fixture.get("/schema/inspect", parameters);
    REQUIRE(node->statusCode() == drogon::k200OK);
    auto json = nlohmann::json::parse(node->body());
    CHECK(json["typeId"] == id);
    CHECK(json["descriptor"].is_object());
    CHECK_FALSE(json["descriptor"].contains("fields"));
    REQUIRE(json["featureTypes"].size() == 1);
    CHECK(json["featureTypes"][0]["name"] == "Road");
    CHECK(json["featureTypes"][0]["produced"] == true);
    CHECK(json["featureTypes"][0]["uniqueIdCompositions"][0][0]["partId"] == "id");
    CHECK(json["featureTypes"][0]["forwardRelationsKnown"] == false);
    for (auto const& field : json.value("fields", nlohmann::json::array())) {
        CHECK(field.contains("name"));
        CHECK(field.contains("$ref"));
        CHECK_FALSE(field.contains("fields"));
        CHECK(field.at("descriptor").contains("kind"));
        CHECK_FALSE(field.at("descriptor").contains("fields"));
    }
    auto legacyParameters = parameters;
    legacyParameters[1].first = "schemaId";
    CHECK(fixture.get("/schema/inspect", legacyParameters)->statusCode() == drogon::k400BadRequest);
    parameters[0].second = hidden->info().schemaId_;
    CHECK(fixture.get("/schema/inspect", parameters)->statusCode() == drogon::k404NotFound);
    parameters[1].second = "1junk";
    CHECK(fixture.get("/schema/inspect", parameters)->statusCode() == drogon::k400BadRequest);
    parameters[1].second = "65536";
    CHECK(fixture.get("/schema/inspect", parameters)->statusCode() == drogon::k400BadRequest);
}

TEST_CASE(
    "Schema explorer previews native named values without expanding alternatives",
    "[HttpSchema]")
{
    using Type = simfil::SchemaType;
    auto owner = std::make_shared<MapSchema>(MapFormat{"schema-http-test", "1", {}});
    auto layer = owner->newLayer("Road", LayerType::Features);
    auto root = layer->addSchema(LayerSchema::FeatureKind);
    layer->addFeatureType(
        FeatureTypeInfo::fromJson(
            {{"name", "Road"},
             {"uniqueIdCompositions", {{{{"partId", "id"}, {"datatype", "U32"}}}}}}),
        root);
    auto choice = layer->addSchema(Type::Kind::OneOf);
    owner->schemaType(choice).setComposition(Type::Composition::OneOf);
    auto symbols = layer->addSchema(Type::Kind::String);
    auto fallback = layer->addSchema(Type::Kind::Int);
    owner->schemaType(symbols).setTypeName("Accuracy");
    owner->schemaType(fallback).setTypeName("Accuracy");
    std::array<Type::NumericRange, 3> ranges{
        {{int64_t{0}, int64_t{9}}, {int64_t{20}, int64_t{29}}, {int64_t{40}, int64_t{49}}}};
    owner->schemaType(fallback).asScalar().setNumericDomain(
        {.encoding = "uint64",
         .unit = "cm",
         .minimum = int64_t{0},
         .maximum = UINT64_MAX,
         .ordinaryRanges = simfil::SchemaType::NumericRanges(std::span(ranges)),
         .scale = 0.1,
         .offset = 2,
         .complete = true});
    for (auto name : {"LOW", "MEDIUM", "HIGH", "UNKNOWN"}) {
        owner->schemaType(symbols).asEnum().addSymbol(name);
        owner->schemaType(symbols).asScalar().addNamedValue(
            {.name = name, .value = std::string_view{name}});
    }
    owner->schemaType(choice).asComposition().addAlternative(owner->schemaType(symbols));
    owner->schemaType(choice).asComposition().addAlternative(owner->schemaType(fallback));
    owner->schemaType(root).asObject().addField("accuracy", owner->schemaType(choice));
    owner->finalize();

    SchemaHttpFixture fixture;
    auto source = fixture.add(std::make_shared<SchemaTestSource>("SchemaChoices", owner));
    auto response = fixture.get(
        "/schema/inspect",
        {{"mapSchemaId", source->info().schemaId_}, {"typeId", std::to_string(choice)}});
    REQUIRE(response->statusCode() == drogon::k200OK);
    auto json = nlohmann::json::parse(response->body());
    auto const& alternatives = json.at("alternatives");
    REQUIRE(alternatives.size() == 2);
    auto const& stringType = alternatives[0].at("descriptor");
    auto const& numericType = alternatives[1].at("descriptor");
    CHECK(stringType.at("typename") == numericType.at("typename"));
    CHECK(stringType.at("kind") == "string");
    CHECK(numericType.at("kind") == "integer");
    CHECK(stringType.at("enumCount") == 4);
    CHECK(stringType.at("namedValueCount") == 4);
    CHECK(stringType.at("namedValueNames") == nlohmann::json::array({"LOW", "MEDIUM", "HIGH"}));
    CHECK_FALSE(stringType.contains("namedValues"));
    CHECK_FALSE(numericType.contains("namedValueCount"));
    auto const& numeric = numericType.at("numericDomain");
    CHECK(numeric.at("encoding") == "uint64");
    CHECK(numeric.at("unit") == "cm");
    CHECK(numeric.at("minimum") == 0);
    CHECK(numeric.at("maximum") == "18446744073709551615");
    CHECK(numeric.at("scale") == 0.1);
    CHECK(numeric.at("offset") == 2);
    CHECK(numeric.at("ordinaryRangeCount") == 3);
    REQUIRE(numeric.at("ordinaryRanges").size() == 2);
    CHECK(numeric.at("ordinaryRanges")[1].at("max") == 29);
    CHECK(alternatives[0].at("$ref") == symbols);
    CHECK(alternatives[1].at("$ref") == fallback);
    auto expanded = fixture.get(
        "/schema/inspect",
        {{"mapSchemaId", source->info().schemaId_}, {"typeId", std::to_string(fallback)}});
    REQUIRE(expanded->statusCode() == drogon::k200OK);
    auto definition = nlohmann::json::parse(expanded->body());
    CHECK(definition.at("descriptor").at("numericDomain").at("ordinaryRanges").size() == 3);
}

TEST_CASE(
    "Schema explorer search shares MCP discovery and honors current authorization",
    "[HttpSchema][schema-search]")
{
    using Json = nlohmann::json;
    SchemaHttpFixture fixture;
    auto source = fixture.add("SchemaExplorerSearch", "speed_limit", true, false, 0, true);
    auto info = source->info();
    auto layer = info.mapSchema_->layer("Road");
    auto root = layer->featureSchema("Road");
    REQUIRE(layer->producesFeatureType("Road"));
    auto properties = layer->childSchema(root, "properties");
    REQUIRE(properties != simfil::NoTypeId);
    REQUIRE(layer->childSchema(properties, "speed_limit") != simfil::NoTypeId);
    AuthHeaders headers{{"x-schema-access", "allowed"}};
    std::vector<std::pair<std::string, std::string>> parameters{
        {"mapSchemaId", info.schemaId_},
        {"layerId", "Road"},
        {"typeId", std::to_string(root)},
        {"find", "SPEED-LIMIT"}};
    CHECK(fixture.get("/schema/inspect", parameters)->statusCode() == drogon::k404NotFound);
    auto response = fixture.get("/schema/inspect", parameters, headers);
    REQUIRE(response->statusCode() == drogon::k200OK);
    CHECK(response->getHeader("cache-control") == "no-store");
    auto rest = Json::parse(response->body());
    INFO(rest.dump());
    REQUIRE(rest["items"].size() == 1);
    CHECK(rest["complete"] == true);
    CHECK(rest["discovery"]["scanComplete"] == true);
    CHECK(rest["items"][0]["contexts"][0]["path"] == Json::array({"properties", "speed_limit"}));
    CHECK_FALSE(rest["items"][0].contains("expandArguments"));
    CHECK_FALSE(rest.contains("rootMatches"));

    auto tools = std::make_shared<detail::McpMapgetTools>(
        fixture.service_,
        McpConfig{},
        [] { return Json::object(); },
        nullptr,
        [] { return Json::object(); },
        [](auto const&, auto const&) { return Json::object(); });
    detail::McpMapgetTools::Principal principal{
        "schema-search",
        "reader",
        std::chrono::system_clock::now() + std::chrono::hours(1),
        true,
        false};
    principal.datasourceAuthorization.headers = headers;
    auto promise = std::make_shared<std::promise<Json>>();
    auto ready = promise->get_future();
    (void)tools->invoke(
        principal,
        "mapget_query_schema",
        {{"mapSchemaId", info.schemaId_},
         {"layerId", "Road"},
         {"typeId", root},
         {"find", "SPEED-LIMIT"},
         {"limit", 32}},
        [promise](auto reply) { promise->set_value(std::move(reply)); });
    REQUIRE(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto reply = ready.get();
    INFO(reply.dump());
    REQUIRE(reply.contains("result"));
    auto mcp = reply["result"];
    CHECK(tools->acceptsResult("mapget_query_schema", mcp));
    for (auto& item : mcp["items"])
        item.erase("expandArguments");
    CHECK(mcp["items"] == rest["items"]);
    CHECK(mcp["discovery"] == rest["discovery"]);
    CHECK(mcp["complete"] == rest["complete"]);
    CHECK(mcp["reason"] == rest["reason"]);

    parameters.back().second = "not-present";
    auto absent = Json::parse(fixture.get("/schema/inspect", parameters, headers)->body());
    CHECK(absent["complete"] == true);
    CHECK(absent["items"].empty());
    parameters.back().second = " _- ";
    CHECK(
        fixture.get("/schema/inspect", parameters, headers)->statusCode() ==
        drogon::k400BadRequest);
    parameters.back().second = std::string(129, 'x');
    CHECK(
        fixture.get("/schema/inspect", parameters, headers)->statusCode() ==
        drogon::k400BadRequest);
    parameters.back().second = "speed";
    parameters[1].second = "missing";
    CHECK(
        fixture.get("/schema/inspect", parameters, headers)->statusCode() == drogon::k404NotFound);
    parameters[1].second = "Road";
    fixture.service_.remove(source);
    (void)fixture.add("SchemaExplorerSearch", "replacement", true, false, 0, true);
    CHECK(
        fixture.get("/schema/inspect", parameters, headers)->statusCode() == drogon::k404NotFound);
}

TEST_CASE(
    "Schema inspection bounds native feature contract text before JSON projection",
    "[HttpSchema][schema-contract-budget]")
{
    SchemaHttpFixture fixture;
    for (auto size : {size_t{32}, size_t{200000}}) {
        auto source =
            fixture.add("ContractBudget" + std::to_string(size), "speed", false, false, size);
        auto metadata = source->info();
        auto schema = metadata.mapSchema_->layer("Road");
        auto response = fixture.get(
            "/schema/inspect",
            {{"mapSchemaId", metadata.schemaId_},
             {"typeId", std::to_string(schema->featureSchema("Road"))}});
        CHECK(response->getHeader("cache-control") == "no-store");
        if (size == 32) {
            REQUIRE(response->statusCode() == drogon::k200OK);
            auto body = nlohmann::json::parse(response->body());
            CHECK(
                body["featureTypes"][0]["uniqueIdCompositions"][0][0]["description"] ==
                std::string(size, 'x'));
        }
        else {
            REQUIRE(response->statusCode() == drogon::k413RequestEntityTooLarge);
            CHECK(response->body().size() < 256);
            // The limit is enforced before copying this caller-controlled description,
            // not just by inspecting the size of an already expanded response.
            size_t remaining = 1024 * 1024;
            CHECK_THROWS_AS(
                schema->featureTypeContract(
                    "Road",
                    [&](size_t bytes)
                    {
                        if (bytes > remaining)
                            throw std::length_error("contract budget");
                        remaining -= bytes;
                    }),
                std::length_error);
        }
    }
}

TEST_CASE(
    "Native feature contract traversal is budgeted without changing its shape",
    "[HttpSchema][schema-contract-budget]")
{
    auto owner = std::make_shared<MapSchema>(MapFormat{"contract-budget", "1", {}});
    auto layer = owner->newLayer("Road", LayerType::Features);
    SchemaTestSource source("ContractIdentity");
    auto identity = source.info().mapSchema_->layer("Road")->getTypeInfo("Road");
    owner->addFeatureType(identity);
    layer->addFeatureType(identity.name());
    auto root = layer->featureSchema(identity.name());
    auto declarations = nlohmann::json::array();
    for (size_t i = 0; i < 32; ++i)
        declarations.push_back(
            {{"ownerType", "Road"},
             {"name", "link" + std::to_string(i)},
             {"targetTypes", {"Road"}},
             {"complete", true},
             {"locality", "same-container"}});
    layer->setJsonSchemaAnnotations(root, {{"x-mapget", {{"relationDeclarations", declarations}}}});
    layer->setRootSchema(root);
    owner->finalize();

    size_t work = 0, bytes = 0;
    auto contract = layer->featureTypeContract(
        "Road",
        [&](size_t size)
        {
            ++work;
            bytes += size;
        });
    CHECK(contract == layer->featureTypeContract("Road"));
    CHECK(contract["uniqueIdCompositions"] == identity.toJson()["uniqueIdCompositions"]);
    REQUIRE(contract["forwardRelations"].size() == 32);
    CHECK(bytes >= contract.dump().size());
    CHECK(work > 32);
    size_t remaining = 12;
    CHECK_THROWS_AS(
        layer->featureTypeContract(
            "Road",
            [&](size_t)
            {
                if (remaining == 0)
                    throw std::length_error("contract traversal");
                --remaining;
            }),
        std::length_error);
    CHECK(remaining == 0);
    CHECK(layer->featureTypeContract("Road") == contract);
}

TEST_CASE(
    "Effective schema publication is atomic and respects contribution authorization",
    "[HttpSchema][schema-composition]")
{
    SchemaHttpFixture fixture;
    auto primary = fixture.add("SchemaComposition");
    auto sourceId = fixture.sourceId(primary);
    auto original = primary->info().mapSchema_;
    auto addon = fixture.add("SchemaComposition", "added", true, true);
    CHECK_FALSE(fixture.service_.getSchema(sourceId, original->schemaId(), AuthHeaders{}));
    CHECK(
        fixture.service_.schemaInventory(AuthHeaders{}).dump().find("SchemaComposition") ==
        std::string::npos);

    auto catalog = fixture.service_.sourceCatalog(AuthHeaders{{"x-schema-access", "allowed"}});
    auto row = std::ranges::find_if(
        catalog.sources,
        [&](auto const& entry) { return entry.descriptor.sourceId == sourceId; });
    REQUIRE(row != catalog.sources.end());
    auto effective = row->info->mapSchema_;
    REQUIRE(effective);
    CHECK(effective->schemaId() != original->schemaId());
    // A changed graph must publish even when both providers advertise the same format tuple.
    CHECK(effective->format() == original->format());
    CHECK(
        fixture.service_.getSchema(
            sourceId,
            effective->schemaId(),
            AuthHeaders{{"x-schema-access", "allowed"}}) == effective);
    CHECK_FALSE(fixture.service_.getSchema(sourceId, effective->schemaId(), AuthHeaders{}));

    fixture.service_.remove(addon);
    CHECK(fixture.service_.getSchema(sourceId, original->schemaId(), AuthHeaders{}) == original);
    CHECK_FALSE(fixture.service_.getSchema(sourceId, effective->schemaId(), AuthHeaders{}));
    // The observer must not keep retired graphs alive; this local snapshot does.
    auto memory = fixture.service_.getMemoryStatistics();
    auto components = memory["mapget"]["components"];
    CHECK(std::ranges::any_of(
        components.items(),
        [](auto const& item)
        {
            return item.key().starts_with("metadata.schemas.retired-pinned.") &&
                item.value().at("allocated-bytes").template get<size_t>() > 0;
        }));
    std::weak_ptr<MapSchema const> retired = effective;
    effective.reset();
    catalog.sources.clear();
    CHECK(retired.expired());
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
    CHECK(json.at("schemaId").get<std::string>() == info.mapSchema_->schemaId());
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
    REQUIRE(escaped->schemaPool(escaped->schemaId()));
    CHECK_FALSE(escaped->schemaPool("uninstalled-after-client-destruction"));
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
