#include "../../libs/http-service/src/mcp-native-tools.h"
#include "mapget/http-service/cli.h"
#include "mapget/model/layerschema.h"
#include "mapget/model/sourcedata.h"
#include "mapget/service/locate.h"
#include "mapget/service/memcache.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <future>

using namespace mapget;
using NativeJson = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
/** Exercise real cache, worker and model paths without a browser or network datasource. */
class NativeSource : public DataSource
{
public:
    DataSourceInfo metadata = DataSourceInfo::fromJson(NativeJson::parse(R"({
        "mapId":"Native", "stringPoolId":"native", "maxParallelJobs":1,
        "layers":{"Road":{"featureTypes":[{"name":"Road","uniqueIdCompositions":[[
            {"partId":"id","datatype":"U64"}]]}]},"Raw":{"type":"SourceData"}}
    })"));
    std::atomic_size_t fills{0};
    bool selfContainedValidity = false;
    size_t nestedPropertyLevels = 0;

    /** Attach a small native schema, including a recursive domain. */
    NativeSource()
    {
        auto schema = std::make_shared<LayerSchema>();
        auto root = schema->addSchema(
            simfil::Schema::Kind::Object,
            LayerSchema::featureKey("Road"),
            "Feature");
        auto text = schema->addSchema(simfil::Schema::Kind::String);
        schema->addFieldSchema(root, "typeId", text);
        schema->setOpen(root, true);
        schema->finalize();
        metadata.layers_.at("Road")->featureModelSchema_ = schema;
    }
    /** Expose stable metadata owned by this synthetic source. */
    DataSourceInfo info() override { return metadata; }
    /** Produce scalars, multiple/compound values, attribute validity and a source reference. */
    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        ++fills;
        if (tile->partitionId().kind() == PartitionKind::Object)
            tile->setGeometryAnchor({11, 48, 0});
        auto feature = tile->newFeature("Road", {{"id", int64_t{1}}});
        auto properties = feature->attributes();
        auto values = tile->newArray();
        values->append(tile->newValue(int64_t{4}));
        values->append(tile->newValue(int64_t{7}));
        properties->addField("numbers", values);
        properties->addField("large", tile->newValue(INT64_MAX));
        properties->addField("bytes", tile->newValue(simfil::ByteArray::fromHex("00ff").value()));
        auto literal = tile->newObject();
        literal->addField("$mapget", tile->newValue("user-value"));
        properties->addField("literal", literal);
        if (nestedPropertyLevels) {
            auto child = tile->newObject();
            properties->addField("deep", child);
            for (size_t i = 0; i < nestedPropertyLevels; ++i) {
                auto next = tile->newObject();
                child->addField("next", next);
                child = next;
            }
            child->addField("value", int64_t{42});
        }
        auto geometry = tile->newGeometry(GeomType::Line, 2);
        geometry->append({11, 48, 0});
        geometry->append({11.1, 48.1, 0});
        if (!selfContainedValidity)
            feature->addGeometry(geometry);
        auto attribute = feature->attributeLayers()->newLayer("rules")->newAttribute("speed");
        attribute->addField("limit", tile->newValue(int64_t{80}));
        if (selfContainedValidity)
            attribute->validity()->newGeometry(geometry);
        else
            attribute->validity()->newPoint(Validity::RelativeLengthOffset, 0.5);
        std::vector<QualifiedSourceDataReference> references{
            {SourceDataAddress{32, 64},
             tile->strings()->emplace("Raw").value(),
             tile->strings()->emplace("origin").value()}};
        feature->setSourceDataReferences(tile->newSourceDataReferenceCollection(references));
    }
    /** Retain absolute addresses under a presentation scope and two equally specific matches. */
    void fill(PartitionSourceDataLayer::Ptr const& tile) override
    {
        ++fills;
        tile->setSourceDataAddressFormat(
            PartitionSourceDataLayer::SourceDataAddressFormat::BitRange);
        auto root = tile->newCompound(2);
        root->setSourceDataAddress({0, 128});
        root->setSourceDataAddressScope();
        for (auto name : {"first", "second"}) {
            auto child = tile->newCompound(1);
            child->setSourceDataAddress({32, 64});
            child->setSchemaName("example.Payload");
            child->object()->addField("count", tile->newValue(int64_t{3}));
            root->object()->addField(name, child);
        }
        tile->addRoot(root);
    }
    /** Primary-ID routing does not load data or recursively wait on the worker pool. */
    std::vector<LocateCandidate> locate(LocateRequest const&) override
    {
        return {LocateCandidate(
            MapPartitionKey(LayerType::Features, "Native", "Road", TileId::fromValue(131073)),
            "Road.1")};
    }
};

/** A tiny deterministic place provider avoids requiring the optional WOF artifact. */
class NativeLocation : public LocationLookup
{
public:
    /** Return one provider-owned match; the tool must preserve coordinates and provenance. */
    std::vector<LocationMatch> search(std::string_view name, uint32_t) const override
    {
        return {{.id = "test:1", .name = std::string(name), .lonLat = {11, 48}, .source = "test"}};
    }
    /** Exact identity lookup does not depend on a name query finding the same record again. */
    std::optional<LocationMatch> find(std::string_view id) const override
    {
        if (id == "test:boundary") {
            auto match = search("Large Boundary", 1).front();
            match.id = id;
            match.geometryAvailable = true;
            {
                auto ring = NativeJson::array();
                for (size_t i = 0; i < 4000; ++i) {
                    auto angle = static_cast<double>(i) * 6.283185307179586 / 4000;
                    ring.push_back({11 + std::cos(angle), 48 + std::sin(angle)});
                }
                ring.push_back(ring.front());
                match.geometry = {{"type", "Polygon"}, {"coordinates", NativeJson::array({ring})}};
            }
            return match;
        }
        return id == "test:1" ? std::optional(search("Munich", 1).front()) : std::nullopt;
    }
};

/** Own one worker, a source and a headless native catalog, with deterministic host callbacks. */
class NativeFixture
{
public:
    Service service{std::make_shared<MemCache>(8), false, 0ms, 1};
    NativeLocation location;
    std::shared_ptr<NativeSource> source;
    detail::McpViewerRelay::Principal
        principal{"test", "reader", std::chrono::system_clock::now() + 1h, true, false};
    std::shared_ptr<detail::McpNativeTools> tools;

    /** Enable native contracts while retaining independent caller permissions. */
    explicit NativeFixture(
        bool objects = false,
        std::shared_ptr<NativeSource> nativeSource = std::make_shared<NativeSource>())
        : source(std::move(nativeSource))
    {
        if (objects) {
            for (auto const& [_, layer] : source->metadata.layers_) {
                layer->partitionKind_ = PartitionKind::Object;
                layer->tileAssociationLevel_ = 13;
            }
        }
        service.add(source);
        resetTools();
    }
    /** Recreate immutable catalog settings for boundary tests, draining the previous owner. */
    void resetTools(McpConfig config = {})
    {
        if (tools)
            tools->stop();
        tools = std::make_shared<detail::McpNativeTools>(
            service,
            config,
            []
            {
                return NativeJson{
                    {"timestampMs", 1},
                    {"service", {{"workers", 1}}},
                    {"memory", NativeJson::object()},
                    {"cache", NativeJson::object()},
                    {"tilesWebsocket", NativeJson::object()},
                    {"tilesHttp", NativeJson::object()}};
            },
            &location,
            [] {
                return NativeJson{{"model", {{"sources", NativeJson::array()}}}, {"revision", "1"}};
            },
            [](auto const&, auto const&) {
                return NativeJson{{"persisted", true}, {"revision", "2"}};
            });
    }
    /** Await completion outside the service pool and validate successful wire payloads. */
    NativeJson call(std::string name, NativeJson arguments = NativeJson::object())
    {
        auto promise = std::make_shared<std::promise<NativeJson>>();
        auto future = promise->get_future();
        (void)tools->invoke(
            principal,
            name,
            std::move(arguments),
            [promise](NativeJson result) { promise->set_value(std::move(result)); });
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        auto reply = future.get();
        INFO(reply.dump());
        if (reply.contains("result"))
            CHECK(tools->acceptsResult(name, reply["result"]));
        return reply;
    }
    /** Ordinary tile input shared across query tests. */
    static NativeJson selection(std::string layer = "Road")
    {
        return {
            {"mapId", "Native"},
            {"layerId", layer},
            {"partitions", NativeJson::array({{{"kind", "tile"}, {"id", 131073}}})}};
    }
};
}  // namespace

TEST_CASE(
    "Native MCP catalogs enforce schemas and independent administrative permissions",
    "[mcp-native]")
{
    NativeFixture fixture;
    auto tools = fixture.tools->tools(fixture.principal);
    CHECK(tools.size() == 11);
    for (auto const& tool : tools) {
        INFO(tool["name"]);
        CHECK_FALSE(
            fixture.tools->acceptsArguments(tool["name"].get<std::string>(), {{"unknown", true}}));
        CHECK_FALSE(
            fixture.tools->acceptsResult(tool["name"].get<std::string>(), NativeJson::array()));
        CHECK(tool["inputSchema"]["type"] == "object");
        CHECK(tool["outputSchema"]["type"] == "object");
    }
    CHECK(fixture.call("mapget_get_config").contains("error"));
    auto missingSelection =
        fixture.call("mapget_extract_features", {{"mapId", "Native"}, {"layerId", "Road"}});
    CHECK(
        missingSelection["error"]["message"].get<std::string>().find("partitions") !=
        std::string::npos);
    CHECK_FALSE(fixture.tools->acceptsArguments(
        "mapget_extract_features",
        {{"mapId", "Native"}, {"layerId", "Road"}}));
    for (auto selector : {"partitions", "featureIds"}) {
        auto empty =
            NativeJson{{"mapId", "Native"}, {"layerId", "Road"}, {selector, NativeJson::array()}};
        CHECK_FALSE(fixture.tools->acceptsArguments("mapget_extract_features", empty));
        CHECK(fixture.call("mapget_extract_features", empty).contains("error"));
    }
    CHECK(fixture.call("mapget_get_diagnostics").contains("error"));
    auto config = McpConfig{};
    config.configReadEnabled = config.configWriteEnabled = config.directConfigPersistence = true;
    fixture.resetTools(config);
    auto oldRead = isGetConfigEndpointEnabled(), oldWrite = isPostConfigEndpointEnabled();
    setGetConfigEndpointEnabled(true);
    setPostConfigEndpointEnabled(true);
    fixture.principal.configRead = fixture.principal.configWrite = fixture.principal.diagnostics =
        true;
    CHECK(fixture.tools->tools(fixture.principal).size() == 14);
    CHECK(fixture.call("mapget_get_config")["result"]["items"][0]["revision"] == "1");
    CHECK(
        fixture.call(
            "mapget_set_config",
            {{"model", NativeJson::object()},
             {"expectedRevision", "1"}})["result"]["items"][0]["persisted"] == true);
    CHECK(
        fixture.call(
            "mapget_get_diagnostics",
            {{"sections", {"workers"}}})["result"]["items"][0]["value"]["workers"] == 1);
    setGetConfigEndpointEnabled(oldRead);
    setPostConfigEndpointEnabled(oldWrite);
}

TEST_CASE(
    "Native MCP help uses ordinary read authority and drains coalesced refreshes",
    "[mcp-native][mcp-help]")
{
    NativeFixture fixture;
    auto result = fixture.call("mapget_docs", {{"query", "cardinality"}})["result"];
    REQUIRE(result["complete"] == true);
    REQUIRE_FALSE(result["items"].empty());
    CHECK(result["revision"].is_string());
    CHECK(result["items"][0].contains("content"));
    CHECK(fixture.source->fills == 0);
    CHECK(fixture.call("mapget_docs", {{"query", "array"}, {"title", "Array"}}).contains("error"));
    fixture.principal.read = false;
    CHECK(fixture.call("mapget_docs").contains("error"));
    fixture.principal.read = true;
    for (size_t i = 0; i < 20; ++i)
        fixture.tools->refreshHelp();
    fixture.resetTools();
    auto config = McpConfig{};
    config.limits.resultBytes = 2048;
    fixture.resetTools(config);
    auto bounded = fixture.call("mapget_docs", {{"query", "cardinality"}});
    REQUIRE(bounded.contains("result"));
    CHECK(bounded["result"]["complete"] == false);
    CHECK(bounded["result"]["reason"] == "byte_limit");
}

TEST_CASE("Native metadata and conversions need neither browser nor tile loads", "[mcp-native]")
{
    NativeFixture fixture;
    auto sources = fixture.call("mapget_list_sources")["result"];
    REQUIRE(sources["items"].size() == 1);
    CHECK(sources.dump().find("featureModelSchema\"") == std::string::npos);
    CHECK_FALSE(sources["items"][0]["layers"][1].contains("coverage"));
    CHECK(
        sources["items"][0]["layers"][1]["featureTypes"][0]["uniqueIdCompositions"][0][0]
               ["partId"] == "id");
    auto schema = fixture.call(
        "mapget_query_schema",
        {{"mapId", "Native"}, {"layerId", "Road"}, {"query", "fields"}});
    REQUIRE(schema.contains("result"));
    CHECK(schema["result"]["complete"] == true);
    auto validate = fixture.call(
        "mapget_validate_expression",
        {{"mapId", "Native"}, {"layerId", "Road"}, {"expression", "typeId == 'Road'"}});
    CHECK(validate["result"]["items"][0]["valid"] == true);
    auto invalid = fixture.call(
        "mapget_validate_expression",
        {{"mapId", "Native"}, {"layerId", "Road"}, {"expression", "("}});
    CHECK(invalid["result"]["items"][0]["valid"] == false);
    CHECK(fixture.source->fills == 0);
    auto tile = fixture.call(
        "mapget_convert_tile_id",
        {{"longitude", 11.5}, {"latitude", 48.1}, {"level", 13}})["result"]["items"][0];
    REQUIRE(tile.contains("tileId"));
    CHECK(
        fixture
            .call("mapget_convert_tile_id", {{"tileId", tile["tileId"]}})["result"]["items"][0] ==
        tile);
    CHECK(fixture.call("mapget_convert_tile_id", {{"tileId", 0}}).contains("error"));
    CHECK(fixture.call("mapget_convert_tile_id", {{"x", 0}, {"level", 1}}).contains("error"));
    auto coords =
        fixture.call("mapget_convert_coordinates", {{"from", "wgs84"}, {"x", 0}, {"y", 0}});
    CHECK(coords["result"]["items"][0]["x"] == 0);
    CHECK(
        fixture.call("mapget_lookup_place", {{"name", "Munich"}})["result"]["items"][0]["lonLat"] ==
        NativeJson::array({11, 48}));
    CHECK(
        fixture.call(
            "mapget_get_place_geometry",
            {{"id", "test:1"}, {"limit", 1}})["result"]["complete"] == true);
    CHECK(fixture.call("mapget_get_place_geometry", {{"id", "test:404"}})["result"]["items"].empty());
    CHECK(fixture.call("mapget_lookup_place", {{"id", "test:1"}, {"name", "Munich"}})
              .contains("error"));
    CHECK(fixture.call("mapget_lookup_place", {{"name", "Munich"}, {"geometry", true}})
              .contains("error"));
    CHECK(fixture.call("mapget_lookup_place").contains("error"));
    auto boundedPlace = fixture.call(
        "mapget_get_place_geometry",
        {{"id", "test:boundary"}})["result"];
    CHECK(boundedPlace["complete"] == false);
    CHECK(boundedPlace["reason"] == "byte_limit");
    REQUIRE(boundedPlace["items"].size() == 1);
    CHECK(boundedPlace["items"][0]["geometryAvailable"] == true);
    CHECK_FALSE(boundedPlace["items"][0].contains("geometry"));
}

TEST_CASE("Native schema discovery retains every feature type beside deep domains", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    auto deep =
        schema
            ->addSchema(simfil::Schema::Kind::Object, LayerSchema::featureKey("ADeep"), "Feature");
    auto parent = deep;
    for (size_t i = 0; i < 20; ++i) {
        auto child = schema->addSchema(simfil::Schema::Kind::Object);
        schema->addFieldSchema(parent, "next", child);
        parent = child;
    }
    schema->setTypeName(parent, "DeepTarget");
    auto intersection = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::featureKey("Intersection"),
        "Feature");
    auto properties = schema->addSchema(simfil::Schema::Kind::Object);
    auto roads = schema->addSchema(simfil::Schema::Kind::Array);
    auto road = schema->addSchema(simfil::Schema::Kind::Int);
    schema->addFieldSchema(intersection, "properties", properties);
    schema->addFieldSchema(properties, "connectedRoads", roads);
    schema->addFieldSchema(properties, "choice", roads);
    schema->addFieldSchema(properties, "choice", road);
    schema->addElementSchema(roads, road);
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    auto args = NativeJson{{"mapId", "Native"}, {"layerId", "Road"}};
    auto overview = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(overview["complete"] == true);
    REQUIRE(overview["items"].size() == 2);
    CHECK(overview["items"][1]["featureType"] == "Intersection");
    auto descriptor =
        overview["items"][1]["values"][0]["fields"]["properties"]["fields"]["connectedRoads"];
    CHECK(descriptor["kind"] == "array");
    CHECK(descriptor["$ref"] == roads);
    CHECK_FALSE(descriptor.contains("elements"));
    CHECK_FALSE(descriptor.contains("truncated"));
    args["schemaId"] = roads;
    auto detail = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(detail["complete"] == true);
    CHECK(detail["items"][0]["values"][0]["elements"][0]["kind"] == "integer");
    args["schemaId"] = properties;
    args["query"] = "_";
    auto unionDetail = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(unionDetail["complete"] == true);
    CHECK(
        unionDetail["items"][0]["values"][0]["fields"]["choice"]["$ref"] ==
        NativeJson::array({roads, road}));
    args.erase("schemaId");
    args["featureType"] = "Intersection";
    args["query"] = "fields.properties.fields.connectedRoads";
    auto focused = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(focused["complete"] == true);
    CHECK(focused["items"][0]["values"][0]["elements"][0]["kind"] == "integer");
    args.erase("featureType");
    args["query"] = "_";
    auto bounded = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(bounded["complete"] == true);
    CHECK(bounded["items"].size() == 2);
    auto nested = bounded["items"][0]["values"][0];
    for (size_t i = 0; i < 20; ++i) {
        REQUIRE(nested.contains("fields"));
        nested = nested["fields"]["next"].get<NativeJson>();
    }
    CHECK_FALSE(nested.contains("truncated"));
    args["query"] = "**.typename";
    auto projection = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(projection["complete"] == true);
    CHECK(projection["items"][0]["values"] == NativeJson::array({"DeepTarget"}));
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native source discovery always includes identity but keeps coverage separate",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->metadata.layers_.at("Road")->coverage_.resize(5000, Coverage::fromJson(131073));
    NativeFixture fixture(false, source);
    auto args = NativeJson{{"mapId", "Native"}, {"layerId", "Road"}};
    auto summary = fixture.call("mapget_list_sources", args)["result"];
    REQUIRE(summary["complete"] == true);
    auto layer = summary["items"][0]["layers"][0];
    CHECK(layer["coverageRangeCount"] == 5000);
    CHECK(layer["featureTypes"][0]["name"] == "Road");
    CHECK_FALSE(layer.contains("coverage"));
    CHECK(layer["featureTypes"][0]["uniqueIdCompositions"][0][0]["partId"] == "id");
    auto coverage = fixture.call("mapget_get_coverage", args)["result"];
    CHECK(coverage["complete"] == false);
    CHECK(coverage["reason"] == "item_limit");
    CHECK(coverage["items"][0]["coverageKnown"] == true);
    CHECK(coverage["items"][0]["ranges"].size() == 100);
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native schema overview stops at attribute domains rather than JSON depth",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    auto feature =
        schema->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"), "Feature");
    auto properties = schema->addSchema(simfil::Schema::Kind::Object);
    auto layers = schema->addSchema(simfil::Schema::Kind::Object);
    auto rules = schema->addSchema(simfil::Schema::Kind::Object);
    auto speed = schema->addSchema(LayerSchema::AttributeKind);
    auto value = schema->addSchema(simfil::Schema::Kind::Int);
    auto geometry = schema->addSchema(simfil::Schema::Kind::Object);
    schema->addFieldSchema(feature, "properties", properties);
    schema->addFieldSchema(feature, "geometry", geometry);
    schema->addFieldSchema(properties, "layer", layers);
    schema->addFieldSchema(layers, "rules", rules);
    schema->addFieldSchema(rules, "speed", speed);
    schema->addFieldSchema(speed, "speedLimitKmh", value);
    schema->setTypeName(speed, "SpeedLimit");
    schema->setAttributeMetadata(speed, {"Road", "rules", "speed", speed}, "SpeedLimit");
    schema->addFieldSchema(geometry, "coordinateCount", value);
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    NativeJson args{{"mapId", "Native"}, {"layerId", "Road"}};
    auto response = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(response["complete"] == true);
    auto fields = response["items"][0]["values"][0]["fields"];
    auto attribute = fields["properties"]["fields"]["layer"]["fields"]["rules"]["fields"]["speed"];
    CHECK(attribute["kind"] == "Attribute");
    CHECK(attribute["typename"] == "SpeedLimit");
    CHECK(attribute["$ref"] == speed);
    CHECK_FALSE(attribute.contains("fields"));
    CHECK_FALSE(attribute.contains("truncated"));
    CHECK_FALSE(fields["geometry"].contains("fields"));

    args["schemaId"] = speed;
    auto detail = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(detail["complete"] == true);
    CHECK(detail["items"][0]["values"][0]["fields"]["speedLimitKmh"]["kind"] == "integer");
    args.erase("schemaId");
    args["query"] = "**.typename";
    auto queried = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(queried["complete"] == true);
    CHECK(queried["items"][0]["values"] == NativeJson::array({"SpeedLimit"}));
    args["maxWork"] = 4;
    CHECK(fixture.call("mapget_query_schema", args)["result"]["complete"] == false);
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native coverage preserves sparse masks and distinguishes unspecified coverage",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto sw = TileId::fromTileXY(0, 0, 3), ne = TileId::fromTileXY(1, 1, 3);
    source->metadata.layers_.at("Road")->coverage_ = {{sw, ne, {true, false, false, true}}};
    NativeFixture fixture(false, source);
    auto args = NativeJson{{"mapId", "Native"}, {"layerId", "Road"}, {"level", 3}};
    auto response = fixture.call("mapget_get_coverage", args)["result"];
    REQUIRE(response["complete"] == true);
    auto info = response["items"][0];
    CHECK(info["coverageKnown"] == true);
    REQUIRE(info["ranges"].size() == 1);
    CHECK(info["ranges"][0]["filled"] == NativeJson::array({true, false, false, true}));
    CHECK(info["ranges"][0]["min"] == sw.value());
    CHECK(info["ranges"][0]["max"] == ne.value());
    args["level"] = 4;
    info = fixture.call("mapget_get_coverage", args)["result"]["items"][0];
    CHECK(info["coverageKnown"] == true);
    CHECK(info["ranges"].empty());
    args["layerId"] = "Raw";
    info = fixture.call("mapget_get_coverage", args)["result"]["items"][0];
    CHECK(info["coverageKnown"] == false);
    CHECK(info["ranges"].empty());
    CHECK(fixture.call("mapget_get_coverage", {{"mapId", "missing"}, {"layerId", "Road"}})
              .contains("error"));
    CHECK(source->fills == 0);
}

TEST_CASE("Native coverage never returns a truncated sparse mask", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto full = Coverage::fromJson(131073);
    auto sparse = Coverage{
        TileId::fromTileXY(0, 0, 13),
        TileId::fromTileXY(199, 99, 13),
        std::vector<bool>(20000, false)};
    source->metadata.layers_.at("Road")->coverage_ = {full, sparse};
    NativeFixture fixture(true, source);
    auto response =
        fixture.call("mapget_get_coverage", {{"mapId", "Native"}, {"layerId", "Road"}})["result"];
    CHECK(response["complete"] == false);
    CHECK(response["reason"] == "byte_limit");
    REQUIRE(response["items"].size() == 1);
    auto info = response["items"][0];
    CHECK(info["partitionKind"] == "object");
    CHECK(info["tileAssociationLevel"] == 13);
    REQUIRE(info["ranges"].size() == 1);
    CHECK(info["ranges"][0]["filled"].empty());
}

TEST_CASE("Native compound results are not limited by a public depth setting", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto depth = GENERATE(size_t{36}, size_t{300});
    source->nestedPropertyLevels = depth;
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["query"] = "properties.deep";
    auto response = fixture.call("mapget_extract_features", args)["result"];
    if (depth == 36) {
        REQUIRE(response["complete"] == true);
        auto value = response["items"][0]["values"][0][0];
        for (size_t i = 0; i < depth; ++i)
            value = value.at("next").get<NativeJson>();
        CHECK(value["value"] == 42);
    }
    else {
        CHECK(response["complete"] == false);
        CHECK(response["reason"] == "serialization_limit");
    }
    args["maxDepth"] = 64;
    CHECK_FALSE(fixture.tools->acceptsArguments("mapget_extract_features", args));
}

TEST_CASE("Native validation recognizes feature attributes aliases", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    auto root = schema->addSchema(
        simfil::Schema::Kind::Object,
        LayerSchema::featureKey("Intersection"),
        "Feature");
    auto properties = schema->addSchema(simfil::Schema::Kind::Object);
    auto roads = schema->addSchema(simfil::Schema::Kind::Array);
    auto road = schema->addSchema(simfil::Schema::Kind::Int);
    schema->addFieldSchema(root, "properties", properties);
    schema->addFieldSchema(properties, "connectedRoads", roads);
    schema->addElementSchema(roads, road);
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    for (auto expression : {"#attributes.connectedRoads > 3", "#properties.connectedRoads > 3"}) {
        auto result = fixture.call(
            "mapget_validate_expression",
            {{"mapId", "Native"}, {"layerId", "Road"}, {"expression", expression}})["result"];
        REQUIRE(result["complete"] == true);
        auto context = result["items"][0]["contexts"][0];
        CHECK_FALSE(context["unresolvedAccess"].get<bool>());
        CHECK(context["schemaCertain"] == true);
    }
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native extraction returns sequences, compounds, exact scalars and effective validity geometry",
    "[mcp-native]")
{
    NativeFixture fixture;
    auto args = NativeFixture::selection();
    args["expressions"] = {
        "properties.numbers.*",
        "properties",
        "null",
        "missing",
        "properties.large",
        "trace(typeId, 1, 'types')"};
    args["trace"] = true;
    args["predicate"] = "typeId == 'Road'";
    auto reply = fixture.call("mapget_extract_features", args);
    REQUIRE(reply.contains("result"));
    INFO(reply.dump());
    REQUIRE(reply["result"]["complete"] == true);
    auto const& values = reply["result"]["items"][0]["values"];
    CHECK(values[0] == NativeJson::array({4, 7}));
    CHECK(values[1][0]["numbers"] == NativeJson::array({4, 7}));
    CHECK(values[1][0]["bytes"] == NativeJson{{"$mapget", "bytes"}, {"hex", "00ff"}});
    CHECK(values[1][0]["literal"]["$mapget"] == "object");
    CHECK(values[2] == NativeJson::array({nullptr}));
    CHECK(values[4][0]["value"] == std::to_string(INT64_MAX));
    CHECK(reply["result"]["traces"]["types"]["values"] == NativeJson::array({"Road"}));
    args.erase("expressions");
    args["scope"] = "attribute";
    args["query"] = "$feature.typeId";
    args["predicate"] = "limit == 80";
    args["geometry"] = true;
    args["limit"] = 1;
    auto attribute = fixture.call("mapget_extract_features", args)["result"];
    INFO(attribute.dump());
    REQUIRE(attribute["items"].size() == 1);
    CHECK(attribute["items"][0]["values"][0] == NativeJson::array({"Road"}));
    CHECK(attribute["items"][0]["geometry"]["type"] == "MultiPoint");
    CHECK(attribute["items"][0]["geometry"]["coordinates"].size() == 1);
    CHECK(
        std::abs(attribute["items"][0]["geometry"]["coordinates"][0][0].get<double>() - 11.05) <
        0.00001);
    auto locate = fixture.call(
        "mapget_extract_features",
        {{"mapId", "Native"},
         {"layerId", "Road"},
         {"featureIds", {"Road.1"}},
         {"query", "typeId"}});
    CHECK(locate["result"]["items"].size() == 1);
}

TEST_CASE("Native validity geometry does not require primary feature geometry", "[mcp-native]")
{
    NativeFixture fixture;
    fixture.source->selfContainedValidity = true;
    auto args = NativeFixture::selection();
    args["scope"] = "attribute";
    args["query"] = "limit";
    args["geometry"] = true;
    auto reply = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(reply["complete"] == true);
    REQUIRE(reply["items"].size() == 1);
    CHECK(reply["items"][0]["geometry"]["type"] == "LineString");
    CHECK(reply["items"][0]["geometry"]["coordinates"].size() == 2);
}

TEST_CASE(
    "Native source-data links resolve absolute addresses and report tied containing ranges",
    "[mcp-native]")
{
    NativeFixture fixture;
    auto args = NativeJson{
        {"mapId", "Native"},
        {"partition", {{"kind", "tile"}, {"id", 131073}}},
        {"reference",
         {{"layerId", "Raw"}, {"address", std::to_string(SourceDataAddress{33, 2}.u64())}}},
        {"match", "containing"},
        {"query", "_"}};
    auto reply = fixture.call("mapget_extract_source_data", args)["result"];
    INFO(reply.dump());
    REQUIRE(reply["items"].size() == 2);
    CHECK(reply["items"][0]["values"][0]["count"] == 3);
    CHECK(reply["items"][0]["resolution"]["ambiguous"] == true);
    args["match"] = "exact";
    CHECK(fixture.call("mapget_extract_source_data", args)["result"]["items"].empty());
    args["reference"]["address"] = std::to_string(SourceDataAddress{32, 64}.u64());
    CHECK(fixture.call("mapget_extract_source_data", args)["result"]["items"].size() == 2);
    args["maxWork"] = 1;
    CHECK(fixture.call("mapget_extract_source_data", args)["result"]["complete"] == false);
}

TEST_CASE("Native feature source links can be reused without frontend conversion", "[mcp-native]")
{
    NativeFixture fixture;
    auto args = NativeFixture::selection();
    args["query"] = "_sourceData.*";
    auto projected = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(projected["complete"] == true);
    auto const& item = projected["items"][0];
    REQUIRE(item["values"][0].size() == 1);
    auto reference = item["values"][0][0];
    CHECK(reference["address"] == std::to_string(SourceDataAddress{32, 64}.u64()));
    for (auto const& qualifier : {"origin", ""}) {
        reference["qualifier"] = qualifier;
        auto result = fixture.call(
            "mapget_extract_source_data",
            {{"mapId", item["mapId"]},
             {"partition", item["partition"]},
             {"reference", reference},
             {"query", "count"}})["result"];
        REQUIRE(result["complete"] == true);
        REQUIRE(result["items"].size() == 2);
        CHECK(result["items"][0]["values"] == NativeJson::array({3}));
    }
}

TEST_CASE(
    "Native source authorization is rechecked and canceled queued calls retire",
    "[mcp-native]")
{
    NativeFixture fixture;
    fixture.source->requireAuthHeaderRegexMatchOption("x-user", std::regex("allowed"));
    CHECK(fixture.call("mapget_list_sources")["result"]["items"].empty());
    CHECK(fixture.call("mapget_extract_features", NativeFixture::selection()).contains("error"));
    fixture.principal.datasourceHeaders["x-user"] = "allowed";
    CHECK(
        fixture.call("mapget_extract_features", NativeFixture::selection())["result"]["items"]
            .size() == 1);

    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    REQUIRE(fixture.service.scheduleTask(
        [gate](bool admitted)
        {
            if (admitted)
                gate.wait();
        }));
    auto promise = std::make_shared<std::promise<NativeJson>>();
    auto future = promise->get_future();
    auto token = fixture.tools->invoke(
        fixture.principal,
        "mapget_list_sources",
        NativeJson::object(),
        [promise](auto result) { promise->set_value(std::move(result)); });
    fixture.tools->cancel(token);
    release->set_value();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    CHECK(future.get()["result"]["complete"] == false);
}

TEST_CASE("Native object extraction and value budgets preserve exact identity", "[mcp-native]")
{
    NativeFixture fixture(true);
    auto args = NativeFixture::selection();
    args["partitions"] = NativeJson::array({PartitionId::object(UINT64_MAX).toJson()});
    args["query"] = "properties.numbers.*";
    args["limit"] = 1;
    auto result = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(result["items"].size() == 1);
    CHECK(result["items"][0]["partition"] == PartitionId::object(UINT64_MAX).toJson());
    CHECK(result["items"][0]["values"][0] == NativeJson::array({4}));
    CHECK_FALSE(result["complete"].get<bool>());
    CHECK(result["reason"] == "expression_result_limit");
    args["limit"] = 100;
    args["query"] = "_";
    CHECK(fixture.call("mapget_extract_features", args)["result"]["complete"] == true);
    CHECK_FALSE(fixture.tools->acceptsArguments("mapget_extract_features", {{"maxDepth", 1}}));
    auto settings = McpConfig{};
    settings.limits.resultBytes = 1500;
    fixture.resetTools(settings);
    auto bounded = fixture.call("mapget_extract_features", args);
    CHECK(bounded.dump().size() < settings.limits.resultBytes);
    CHECK((bounded.contains("error") || bounded["result"]["complete"] == false));
}

TEST_CASE(
    "Native source address traversal is cycle-safe and never publishes partial ambiguity",
    "[mcp-native]")
{
    NativeSource source;
    auto tile = std::make_shared<PartitionSourceDataLayer>(
        TileId{},
        "native",
        "Native",
        source.metadata.layers_.at("Raw"),
        std::make_shared<StringPool>("native"));
    source.fill(tile);
    auto count = tile->strings()->size();
    CHECK(tile->findSourceData(SourceDataAddress{32, 64})->size() == 2);
    CHECK_FALSE(tile->findSourceData(SourceDataAddress{32, 64}, false, 100, 1));
    CHECK_FALSE(tile->findSourceData(SourceDataAddress{32, 64}, false, 1));
    CHECK_FALSE(
        tile->findSourceData(SourceDataAddress{32, 64}, false, 100, 10, [] { return true; }));
    auto opaque = tile->newCompound(1);
    opaque->setSourceDataAddress(SourceDataAddress(UINT64_MAX));
    opaque->object()->addField("cycle", opaque);
    tile->addRoot(opaque);
    tile->setSourceDataAddressFormat(PartitionSourceDataLayer::SourceDataAddressFormat::Unknown);
    REQUIRE(tile->findSourceData(SourceDataAddress(UINT64_MAX)));
    CHECK(tile->findSourceData(SourceDataAddress(UINT64_MAX))->size() == 1);
    CHECK_FALSE(tile->findSourceData(SourceDataAddress(UINT64_MAX), true));
    CHECK(tile->strings()->size() == count + 1);
}

TEST_CASE("Native imported schemas preserve aliases in recursive queries", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = LayerSchema::fromJsonSchema(NativeJson::parse(R"({
        "oneOf":[{"$ref":"#/$defs/RoadFeature"}],"$defs":{"RoadFeature":{
            "type":"object", "additionalProperties":false,
            "x-mapget":{"metaType":"Feature","featureType":"Road"},
            "properties":{"properties":{"type":"object","additionalProperties":false,
                "properties":{"numbers":{"type":"array","items":{"type":"integer"}}}}}
        }}
    })"));
    schema->finalize();
    REQUIRE(schema->featureSchema("Road") != simfil::NoSchemaId);
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    for (std::string query :
         {"attributes.numbers",
          "properties.numbers",
          "**.attributes.numbers",
          "**.properties.numbers"})
    {
        auto args = NativeFixture::selection();
        args["query"] = query;
        auto reply = fixture.call("mapget_extract_features", args);
        CHECK(reply["result"]["items"][0]["values"] == NativeJson::parse("[[[4,7]]]"));
        auto validation = fixture.call(
            "mapget_validate_expression",
            {{"mapId", "Native"}, {"layerId", "Road"}, {"expression", query}});
        CHECK(validation["result"]["items"][0]["contexts"][0]["unresolvedAccess"] == false);
    }
}
