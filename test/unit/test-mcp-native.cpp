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
            {"partId":"id","datatype":"U64"}],[{"partId":"alias","datatype":"STR"}]]}]},"Raw":{"type":"SourceData"}}
    })"));
    std::atomic_size_t fills{0};
    bool selfContainedValidity = false;
    size_t nestedPropertyLevels = 0;
    size_t additionalFeatures = 0;
    size_t additionalGeometryPoints = 0;
    size_t additionalSourceCompounds = 0;
    size_t additionalAttributes = 0;
    std::string lastFeatureText;
    std::string textProperty;
    std::string secondaryLocateMode = "canonical";

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
        if (!textProperty.empty())
            properties->addField("text", tile->newValue(textProperty));
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
        for (size_t i = 0; i < additionalGeometryPoints; ++i)
            geometry->append({11.1 + double(i) * 1e-7, 48.1, 0});
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
        for (size_t i = 0; i < additionalAttributes; ++i)
            feature->attributeLayers()->newLayer("rules")->newAttribute("speed")->addField(
                "limit",
                tile->newValue(int64_t(90 + i)));
        for (size_t i = 0; i < additionalFeatures; ++i) {
            auto extra = tile->newFeature("Road", {{"id", int64_t(i + 2)}});
            if (i + 1 == additionalFeatures && !lastFeatureText.empty())
                extra->attributes()->addField("text", tile->newValue(lastFeatureText));
        }
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
        for (size_t i = 0; i < additionalSourceCompounds; ++i) {
            auto extra = tile->newCompound(0);
            extra->setSourceDataAddress({static_cast<uint32_t>(256 + i), 1});
        }
    }
    /** Primary and secondary routing remain cheap, including computed/filter selectors. */
    std::vector<LocateCandidate> locate(LocateRequest const& request) override
    {
        auto key =
            MapPartitionKey(LayerType::Features, "Native", "Road", TileId::fromValue(131073));
        if (request.getStrIdPart("alias") == "secondary") {
            if (secondaryLocateMode == "filter")
                return {LocateCandidate(key, "Road", "typeId == 'Road'")};
            if (secondaryLocateMode == "expression")
                return {LocateCandidate::fromFeatureIdExpression(key, "Road", "'Road.1'")};
            return {LocateCandidate(key, "Road.1")};
        }
        if (request.getIntIdPart("id") == 1)
            return {LocateCandidate(key, "Road.1")};
        return {};
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
    auto sources = fixture.call("mapget_list_sources", {{"details", true}})["result"];
    REQUIRE(sources["items"].size() == 1);
    CHECK(sources.dump().find("featureModelSchema\"") == std::string::npos);
    CHECK_FALSE(sources["items"][0]["layers"][1].contains("coverage"));
    CHECK(sources["items"][0]["layers"][1]["schemaFeatureTypes"] == NativeJson::array({"Road"}));
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
    auto mistakenLegacy = fixture.call("mapget_convert_tile_id", {{"legacyTileId", "545377861"}});
    CHECK(mistakenLegacy["error"]["code"] == "invalid_arguments");
    CHECK(
        mistakenLegacy["error"]["message"].get<std::string>().find("Classic") != std::string::npos);
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
    CHECK_FALSE(overview["items"][1]["values"][0].contains("fields"));
    args["featureType"] = "Intersection";
    auto selected = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(selected["complete"] == true);
    auto descriptor =
        selected["items"][0]["values"][0]["fields"]["properties"]["fields"]["connectedRoads"];
    args.erase("featureType");
    CHECK(descriptor["kind"] == "array");
    CHECK(descriptor["$ref"] == roads);
    CHECK_FALSE(descriptor.contains("elements"));
    CHECK(descriptor["childrenOmitted"] == true);
    CHECK(selected["schemaView"] == "shallow_overview");
    CHECK(selected["guidance"].get<std::string>().find("find") != std::string::npos);
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
    CHECK_FALSE(focused.contains("schemaView"));
    CHECK_FALSE(focused["items"][0]["values"][0].contains("childrenOmitted"));
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
    "Native compact discovery pages sources without expanding identifier descriptions",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->metadata.layers_.at("Road")->featureTypes_[0].uniqueIdCompositions_[0][0].description_ =
        std::string(100000, 'x');
    NativeFixture fixture(false, source);
    auto second = std::make_shared<NativeSource>();
    second->metadata.mapId_ = "Second";
    second->metadata.stringPoolId_ = "second";
    fixture.service.add(second);
    auto first = fixture.call("mapget_list_sources", {{"limit", 1}})["result"];
    REQUIRE(first["items"].size() == 1);
    CHECK(first["items"][0]["mapId"] == "Native");
    CHECK(first["complete"] == false);
    CHECK(first["nextOffset"] == 1);
    auto next = fixture.call(
        "mapget_list_sources",
        {{"limit", 1}, {"offset", first["nextOffset"]}})["result"];
    REQUIRE(next["complete"] == true);
    CHECK(next["items"][0]["mapId"] == "Second");
    auto filtered =
        fixture.call("mapget_list_sources", {{"mapId", "Second"}, {"offset", 0}})["result"];
    CHECK(filtered["complete"] == true);
    CHECK(filtered["items"].size() == 1);
    McpConfig config;
    config.limits.resultBytes = 4000;
    fixture.resetTools(config);
    auto bounded = fixture.call("mapget_list_sources")["result"];
    REQUIRE(bounded["items"].size() == 1);
    CHECK(bounded["reason"] == "byte_limit");
    REQUIRE(bounded["nextOffset"] == 1);
    auto resumed =
        fixture.call("mapget_list_sources", {{"offset", bounded["nextOffset"]}})["result"];
    CHECK(resumed["complete"] == true);
    CHECK(resumed["items"][0]["mapId"] == "Second");
    CHECK(source->fills == 0);
    CHECK(second->fills == 0);
}

TEST_CASE(
    "Native source discovery groups raw layer IDs without losing focused metadata",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    for (size_t i = 0; i < 80; ++i) {
        auto raw = std::make_shared<LayerInfo>(*source->metadata.layers_.at("Raw"));
        raw->layerId_ = "Raw-" + std::to_string(i);
        source->metadata.layers_[raw->layerId_] = raw;
    }
    NativeFixture fixture(false, source);
    auto summary = fixture.call("mapget_list_sources")["result"];
    REQUIRE(summary["complete"] == true);
    REQUIRE(summary["items"].size() == 1);
    auto const& item = summary["items"][0];
    REQUIRE(item["layers"].size() == 1);
    CHECK(item["layers"][0]["layerId"] == "Road");
    CHECK(item["sourceDataLayers"].size() == 81);
    CHECK(item["sourceDataLayers"][0] == "Raw");
    auto focused = fixture.call("mapget_list_sources", {{"layerId", "Raw-79"}})["result"];
    REQUIRE(focused["complete"] == true);
    REQUIRE(focused["items"][0]["layers"].size() == 1);
    CHECK(focused["items"][0]["layers"][0]["type"] == "SourceData");
    CHECK_FALSE(focused["items"][0].contains("sourceDataLayers"));
    auto expanded = fixture.call("mapget_list_sources", {{"details", true}})["result"];
    REQUIRE(expanded["complete"] == true);
    CHECK(expanded["items"][0]["layers"].size() == 82);
    CHECK_FALSE(expanded["items"][0].contains("sourceDataLayers"));
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native source discovery defaults to details for a selected map or source",
    "[mcp-native]")
{
    NativeFixture fixture;
    auto inventory = fixture.call("mapget_list_sources")["result"];
    REQUIRE(inventory["complete"] == true);
    auto const& compact = inventory["items"][0];
    CHECK_FALSE(compact.contains("protocolVersion"));
    REQUIRE(compact["layers"].size() == 1);
    CHECK_FALSE(compact["layers"][0]["featureTypes"][0].contains("uniqueIdCompositions"));
    CHECK(compact["sourceDataLayers"] == NativeJson::array({"Raw"}));

    auto selector = GENERATE("mapId", "sourceId");
    NativeJson arguments{{selector, compact.at(selector)}};
    auto focused = fixture.call("mapget_list_sources", arguments)["result"];
    REQUIRE(focused["complete"] == true);
    REQUIRE(focused["items"].size() == 1);
    auto const& detailed = focused["items"][0];
    CHECK(detailed.contains("protocolVersion"));
    REQUIRE(detailed["layers"].size() == 2);
    CHECK_FALSE(detailed.contains("sourceDataLayers"));
    for (auto const& layer : detailed["layers"]) {
        CHECK_FALSE(layer.contains("featureModelSchema"));
        CHECK_FALSE(layer.contains("coverage"));
        if (layer["layerId"] == "Road") {
            auto const& compositions = layer["featureTypes"][0]["uniqueIdCompositions"];
            REQUIRE(compositions.size() == 2);
            CHECK(compositions[0][0]["partId"] == "id");
            CHECK(compositions[1][0]["partId"] == "alias");
        }
    }

    arguments["details"] = false;
    auto overridden = fixture.call("mapget_list_sources", arguments)["result"];
    REQUIRE(overridden["complete"] == true);
    CHECK(overridden["items"][0] == compact);
    arguments["details"] = true;
    CHECK(fixture.call("mapget_list_sources", arguments)["result"]["items"][0] == detailed);
    CHECK(fixture.source->fills == 0);
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
    auto compactArgs = args;
    compactArgs["details"] = false;
    auto compact = fixture.call("mapget_list_sources", compactArgs)["result"];
    CHECK_FALSE(
        compact["items"][0]["layers"][0]["featureTypes"][0].contains("uniqueIdCompositions"));
    auto coverage = fixture.call("mapget_get_coverage", args)["result"];
    CHECK(coverage["complete"] == false);
    CHECK(coverage["reason"] == "item_limit");
    CHECK(coverage["items"][0]["coverageKnown"] == true);
    CHECK(coverage["items"][0]["ranges"].size() == 100);
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native schema discovery lists types and follows compact descriptor references",
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
    CHECK(response["items"][0]["schemaId"] == feature);
    CHECK_FALSE(response["items"][0]["values"][0].contains("fields"));
    args["featureType"] = "ReferenceOnly";
    auto missing = fixture.call("mapget_query_schema", args)["error"];
    CHECK(missing["code"] == "unknown_feature_type");
    CHECK(missing["message"].get<std::string>().find("reference-only") != std::string::npos);
    auto validationArgs = args;
    validationArgs["expression"] = "true";
    CHECK(
        fixture.call("mapget_validate_expression", validationArgs)["error"]["code"] ==
        "unknown_feature_type");
    args["featureType"] = "Road";
    response = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(response["complete"] == true);
    auto fields = response["items"][0]["values"][0]["fields"];
    auto layerReference = fields["properties"]["fields"]["layer"];
    CHECK(layerReference["$ref"] == layers);
    CHECK_FALSE(layerReference.contains("fields"));
    CHECK_FALSE(fields["geometry"].contains("fields"));
    args.erase("featureType");
    args["schemaId"] = rules;
    response = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(response["complete"] == true);
    auto attribute = response["items"][0]["values"][0]["fields"]["speed"];
    CHECK(attribute["kind"] == "Attribute");
    CHECK(attribute["typename"] == "SpeedLimit");
    CHECK(attribute["$ref"] == speed);
    CHECK_FALSE(attribute.contains("fields"));
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
    auto args =
        NativeJson{{"mapId", "Native"}, {"layerId", "Road"}, {"level", 3}, {"format", "raw"}};
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

TEST_CASE("Native coverage summarizes large sparse masks with real covered samples", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto sparse = Coverage{
        TileId::fromTileXY(0, 0, 13),
        TileId::fromTileXY(199, 99, 13),
        std::vector<bool>(20000, false)};
    sparse.filled_[201] = true;
    sparse.filled_[19999] = true;
    source->metadata.layers_.at("Road")->coverage_ = {sparse};
    NativeFixture fixture(false, source);
    auto args = NativeJson{{"mapId", "Native"}, {"layerId", "Road"}};
    auto response = fixture.call("mapget_get_coverage", args)["result"];
    REQUIRE(response["complete"] == true);
    auto range = response["items"][0]["ranges"][0];
    CHECK(range["coverageShape"] == "sparse");
    CHECK(range["coveredTileCount"] == 2);
    CHECK_FALSE(range.contains("filled"));
    CHECK(
        range["sampleTileIds"] ==
        NativeJson::array(
            {TileId::fromTileXY(1, 1, 13).value(), TileId::fromTileXY(199, 99, 13).value()}));
    args["maxWork"] = 100;
    response = fixture.call("mapget_get_coverage", args)["result"];
    CHECK(response["complete"] == false);
    CHECK(response["reason"] == "work_limit");
    CHECK(response["items"][0]["ranges"].empty());
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
    auto response = fixture.call(
        "mapget_get_coverage",
        {{"mapId", "Native"}, {"layerId", "Road"}, {"format", "raw"}})["result"];
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

TEST_CASE(
    "Native string budgets count JSON escapes without penalizing ordinary text",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto escaped = GENERATE(false, true);
    source->textProperty = std::string(30000, escaped ? '\x01' : 'a');
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["query"] = "properties.text";
    auto response = fixture.call("mapget_extract_features", args)["result"];
    if (escaped) {
        CHECK(response["complete"] == false);
        CHECK(response["reason"] == "byte_limit");
        REQUIRE_FALSE(response["issues"].empty());
        CHECK(response["issues"].back()["phase"] == "budget");
    }
    else {
        REQUIRE(response["complete"] == true);
        CHECK(response["items"][0]["values"][0][0] == source->textProperty);
    }
}

TEST_CASE("Native schema enum previews retain explicit access to all choices", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    auto root =
        schema->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"), "Feature");
    auto category = schema->addSchema(simfil::Schema::Kind::Int);
    for (int64_t i = 0; i < 40; ++i)
        schema->addEnumValue(category, i);
    schema->addFieldSchema(root, "category", category);
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    NativeJson args{{"mapId", "Native"}, {"layerId", "Road"}, {"schemaId", category}};
    auto response = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(response["complete"] == true);
    auto descriptor = response["items"][0]["values"][0];
    CHECK(descriptor["enumCount"] == 40);
    CHECK(descriptor["enumPreview"].size() == 8);
    CHECK_FALSE(descriptor.contains("enum"));
    args["query"] = "enum.*";
    args["limit"] = 100;
    response = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(response["complete"] == true);
    CHECK(response["items"][0]["values"].size() == 40);
    args.erase("query");
    args.erase("schemaId");
    args["find"] = "category";
    auto discovery = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(discovery["items"].size() == 1);
    CHECK(
        discovery["items"][0]["enum"]["literalPreview"] ==
        NativeJson::array({0, 1, 2, 3, 4, 5, 6, 7}));
    CHECK(discovery["items"][0]["enum"]["literalCount"] == 40);
    CHECK(discovery["items"][0]["enum"]["literalsOmitted"] == true);
}

TEST_CASE("Native validation pages independently compiled feature contexts", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    for (auto type : {"First", "Second", "Third"}) {
        auto root =
            schema->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey(type), "Feature");
        auto value = schema->addSchema(simfil::Schema::Kind::Int);
        schema->addFieldSchema(root, "value", value);
    }
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    NativeJson
        args{{"mapId", "Native"}, {"layerId", "Road"}, {"expression", "value > 0"}, {"limit", 2}};
    auto response = fixture.call("mapget_validate_expression", args)["result"];
    REQUIRE(response["complete"] == false);
    CHECK(response["nextOffset"] == 2);
    CHECK(response["items"][0]["contextCount"] == 3);
    CHECK(response["items"][0]["contexts"].size() == 2);
    auto first = response["items"][0]["contexts"][0]["featureType"];
    auto second = response["items"][0]["contexts"][1]["featureType"];
    args["offset"] = response["nextOffset"];
    response = fixture.call("mapget_validate_expression", args)["result"];
    REQUIRE(response["complete"] == true);
    REQUIRE(response["items"][0]["contexts"].size() == 1);
    CHECK(response["items"][0]["contextOffset"] == 2);
    CHECK(response["items"][0]["valid"] == true);
    auto third = response["items"][0]["contexts"][0]["featureType"];
    CHECK(third != first);
    CHECK(third != second);
    CHECK(source->fills == 0);
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

TEST_CASE(
    "Native malformed feature identifiers explain identity recovery before tile IO",
    "[mcp-native]")
{
    NativeFixture fixture;
    auto invalid = GENERATE("1", "MissingType.1", "Road.1.2");
    auto result = fixture.call(
        "mapget_extract_features",
        {{"mapId", "Native"}, {"layerId", "Road"}, {"featureIds", {invalid}}});
    REQUIRE(result.contains("error"));
    CHECK(result["error"]["code"] == "invalid_feature_id");
    auto message = result["error"]["message"].get<std::string>();
    CHECK(message.find("Known types in this layer: Road") != std::string::npos);
    CHECK(message.find("uniqueIdCompositions") != std::string::npos);
    CHECK(fixture.source->fills == 0);
}

TEST_CASE(
    "Native extraction resolves secondary selectors without blocking its single worker",
    "[mcp-native]")
{
    NativeFixture fixture;
    fixture.source->secondaryLocateMode = GENERATE("canonical", "filter", "expression");
    auto args = NativeJson{
        {"mapId", "Native"},
        {"layerId", "Road"},
        {"featureIds", {"Road.secondary", "Road.1", "Road.missing"}},
        {"query", "typeId"}};
    if (GENERATE(false, true))
        args["partitions"] = NativeFixture::selection()["partitions"];
    auto result = fixture.call("mapget_extract_features", args)["result"];
    INFO(result.dump());
    REQUIRE(result["complete"] == true);
    REQUIRE(result["items"].size() == 1);
    CHECK(result["items"][0]["featureId"] == "Road.1");
    REQUIRE(result["issues"].size() == 1);
    CHECK(result["issues"][0]["featureId"] == "Road.missing");
    CHECK(fixture.source->fills == 1);

    // A filtered-out resolved feature must not be reported as an unresolved identity.
    args["featureIds"] = {"Road.secondary"};
    args["predicate"] = "false";
    result = fixture.call("mapget_extract_features", args)["result"];
    CHECK(result["complete"] == true);
    CHECK(result["items"].empty());
    CHECK(result["issues"].empty());
    CHECK(fixture.source->fills == 1);
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
    auto rootArguments = reply["items"][0]["rootReadArguments"];
    CHECK_FALSE(rootArguments.contains("reference"));
    CHECK(rootArguments["layerId"] == "Raw");
    rootArguments["query"] = "first.count";
    auto rootReply = fixture.call("mapget_extract_source_data", rootArguments)["result"];
    REQUIRE(rootReply["complete"] == true);
    CHECK(rootReply["items"][0]["values"] == NativeJson::array({3}));
    CHECK_FALSE(rootReply["items"][0].contains("rootReadArguments"));
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
    CHECK(result["items"][0]["rowComplete"] == false);
    CHECK_FALSE(result.contains("nextOffset"));
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
    "Native source addresses ignore scalar payload size and presentation roots",
    "[mcp-native]")
{
    NativeSource source;
    auto tile = std::make_shared<PartitionSourceDataLayer>(
        TileId{},
        "native",
        "Native",
        source.metadata.layers_.at("Raw"),
        std::make_shared<StringPool>("native"));
    auto root = tile->newCompound(1);
    root->setSourceDataAddress(SourceDataAddress{0, 1000});
    auto values = tile->newArray();
    for (auto i = 0; i < 10000; ++i)
        values->append(tile->newValue(int64_t{i}));
    root->object()->addField("coordinates", values);
    tile->addRoot(root);
    // Absolute references address records directly, including records which are
    // not linked into the presentation tree yet.
    auto target = tile->newCompound(0);
    target->setSourceDataAddress(SourceDataAddress{32, 64});
    size_t work = 0;
    auto result = tile->findSourceData(
        SourceDataAddress{32, 64},
        false,
        2,
        10,
        [&]
        {
            ++work;
            return false;
        });
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front()->addr() == target->addr());
    CHECK(work == 2);
    CHECK_FALSE(tile->findSourceData(SourceDataAddress{32, 64}, false, 1));
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

TEST_CASE(
    "Native schema find locates fields and symbols through arrays without expanding cycles",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    auto root =
        schema->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"), "Feature");
    auto properties = schema->addSchema(simfil::Schema::Kind::Object);
    auto layers = schema->addSchema(simfil::Schema::Kind::Object);
    auto rules = schema->addSchema(simfil::Schema::Kind::Object);
    auto speed = schema->addSchema(LayerSchema::AttributeKind);
    auto number = schema->addSchema(simfil::Schema::Kind::Int);
    auto conditions = schema->addSchema(simfil::Schema::Kind::Array);
    auto vehicle = schema->addSchema(simfil::Schema::Kind::String);
    schema->addFieldSchema(root, "properties", properties);
    schema->addFieldSchema(properties, "layer", layers);
    schema->addFieldSchema(layers, "rules", rules);
    schema->addFieldSchema(rules, "SPEED_LIMIT", speed);
    schema->setAttributeMetadata(speed, {"Road", "rules", "SPEED_LIMIT", speed}, "SPEED_LIMIT");
    schema->addFieldSchema(speed, "speedLimit", number);
    schema->addFieldSchema(speed, "conditions", conditions);
    schema->addElementSchema(conditions, vehicle);
    schema->addEnumSymbol(vehicle, "TRUCK");
    schema->addEnumSymbol(vehicle, "SPEED_SIGN");
    schema->addFieldSchema(properties, "cycle", properties);
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    auto args =
        NativeJson{{"mapId", "Native"}, {"layerId", "Road"}, {"find", "SpEeD"}, {"limit", 1}};
    auto first = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(first["items"].size() == 1);
    CHECK(first["items"][0]["schemaId"] == speed);
    CHECK(first["complete"] == false);
    CHECK(first["reason"] == "matches_omitted");
    CHECK(first["discovery"]["scanComplete"] == true);
    CHECK(first["discovery"]["matchedDomains"] == 3);
    CHECK_FALSE(first.contains("nextOffset"));
    REQUIRE_FALSE(first["narrowing"].empty());
    auto expand =
        fixture.call("mapget_query_schema", first["items"][0]["expandArguments"])["result"];
    CHECK(expand["complete"] == true);
    CHECK(expand["items"][0]["schemaId"] == speed);
    args["limit"] = 12;
    auto all = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(all["items"].size() == 3);
    CHECK(
        all["items"][1]["contexts"][0]["path"] ==
        NativeJson::array({"properties", "layer", "rules", "SPEED_LIMIT", "speedLimit"}));
    CHECK(all["items"][1]["contexts"][0]["attributeContext"]["schemaId"] == speed);
    args["find"] = "truck";
    auto symbol = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(symbol["complete"] == true);
    CHECK(symbol["items"][0]["enum"]["matchingSymbols"] == NativeJson::array({"TRUCK"}));
    CHECK(symbol["items"][0]["schemaId"] == vehicle);
    CHECK(symbol["items"][0]["contexts"][0]["path"].back() == "*");
    CHECK(symbol["items"][0]["contexts"][0]["attributeContext"]["name"] == "SPEED_LIMIT");
    auto values = fixture.call(
        "mapget_query_schema",
        symbol["items"][0]["enum"]["valuesArguments"])["result"];
    CHECK(values["items"][0]["values"].size() == 2);
    args["find"] = "speed limit";
    auto separated = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE_FALSE(separated["items"].empty());
    CHECK(separated["items"][0]["contexts"][0]["fieldName"] == "SPEED_LIMIT");
    args["find"] = " _- ";
    CHECK(fixture.call("mapget_query_schema", args)["error"]["code"] == "invalid_arguments");
    args["find"] = "not-present";
    auto absent = fixture.call("mapget_query_schema", args)["result"];
    CHECK(absent["items"].empty());
    CHECK(absent["guidance"].get<std::string>().find("shorter") != std::string::npos);
    args["offset"] = 100;
    CHECK_FALSE(fixture.tools->acceptsArguments("mapget_query_schema", args));
    args.erase("offset");
    args["maxWork"] = 1;
    auto incomplete = fixture.call("mapget_query_schema", args)["result"];
    CHECK(incomplete["complete"] == false);
    CHECK_FALSE(incomplete.contains("guidance"));
    CHECK(source->fills == 0);
}

TEST_CASE(
    "Native feature extraction accepts a singular type selector consistently with schema lookup",
    "[mcp-native]")
{
    NativeFixture fixture;
    NativeJson args{
        {"mapId", "Native"},
        {"layerId", "Road"},
        {"featureIds", {"Road.1"}},
        {"featureType", "Road"},
        {"query", "_.id"}};
    auto selected = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(selected["complete"] == true);
    REQUIRE(selected["items"].size() == 1);
    CHECK(selected["items"][0]["featureId"] == "Road.1");
    args["featureType"] = "Other";
    auto absent = fixture.call("mapget_extract_features", args)["result"];
    CHECK(absent["complete"] == true);
    CHECK(absent["items"].empty());
    args["featureTypes"] = {"Road"};
    CHECK(fixture.call("mapget_extract_features", args).contains("error"));
}

TEST_CASE(
    "Native extraction pages matching rows without skipping partial projections",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 2;
    source->additionalAttributes = 1;
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["query"] = "_.id";
    args["limit"] = 1;
    SECTION("Feature pages preserve exact order and complete exact-limit pages")
    {
        for (size_t i = 0; i < 3; ++i) {
            args["offset"] = i;
            auto result = fixture.call("mapget_extract_features", args)["result"];
            REQUIRE(result["items"].size() == 1);
            CHECK(result["items"][0]["featureId"] == "Road." + std::to_string(i + 1));
            CHECK(result["items"][0]["rowComplete"] == true);
            CHECK(result["complete"] == (i == 2));
            if (i < 2)
                CHECK(result["nextOffset"] == i + 1);
            else
                CHECK_FALSE(result.contains("nextOffset"));
        }
        args["offset"] = 3;
        auto empty = fixture.call("mapget_extract_features", args)["result"];
        CHECK(empty["complete"] == true);
        CHECK(empty["items"].empty());
    }
    SECTION("Offset follows predicate filtering")
    {
        args["predicate"] = "_.id != 'Road.2'";
        auto first = fixture.call("mapget_extract_features", args)["result"];
        CHECK(first["nextOffset"] == 1);
        args["offset"] = first["nextOffset"];
        auto second = fixture.call("mapget_extract_features", args)["result"];
        REQUIRE(second["items"].size() == 1);
        CHECK(second["items"][0]["featureId"] == "Road.3");
        CHECK(second["complete"] == true);
    }
    SECTION("Attribute contexts page independently within one feature")
    {
        args["scope"] = "attribute";
        args["query"] = "limit";
        auto first = fixture.call("mapget_extract_features", args)["result"];
        CHECK(first["nextOffset"] == 1);
        CHECK(first["items"][0]["values"] == NativeJson::parse("[[80]]"));
        args["offset"] = first["nextOffset"];
        auto second = fixture.call("mapget_extract_features", args)["result"];
        REQUIRE(second["items"].size() == 1);
        CHECK(second["items"][0]["values"] == NativeJson::parse("[[90]]"));
        CHECK(second["complete"] == true);
    }
    SECTION("Byte-limited continuation retries the incomplete row")
    {
        source->lastFeatureText = std::string(10000, 'x');
        auto settings = McpConfig{};
        settings.limits.resultBytes = 6000;
        fixture.resetTools(settings);
        args["query"] = "properties";
        args["limit"] = 100;
        auto result = fixture.call("mapget_extract_features", args)["result"];
        CHECK(result["reason"] == "byte_limit");
        CHECK(result["nextOffset"] == 2);
        REQUIRE(result["items"].size() >= 2);
        CHECK(result["items"][0]["rowComplete"] == true);
        CHECK(result["items"][1]["rowComplete"] == true);
        if (result["items"].size() == 3)
            CHECK(result["items"][2]["rowComplete"] == false);
        args["offset"] = result["nextOffset"];
        auto blocked = fixture.call("mapget_extract_features", args)["result"];
        CHECK(blocked["complete"] == false);
        CHECK_FALSE(blocked.contains("nextOffset"));
        args["query"] = "_.id";
        auto narrowed = fixture.call("mapget_extract_features", args)["result"];
        REQUIRE(narrowed["items"].size() == 1);
        CHECK(narrowed["items"][0]["featureId"] == "Road.3");
        CHECK(narrowed["complete"] == true);
    }
}

TEST_CASE("Native coverage bounds preserve full-world and antimeridian extents", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    SECTION("Whole world at ordinary and coarsest levels")
    {
        auto level = GENERATE(0, 3, 15);
        source->metadata.layers_.at("Road")->coverage_ = {
            {TileId::fromTileXY(0, 0, level),
             TileId::fromTileXY((1u << (level + 1)) - 1, (1u << level) - 1, level),
             {}}};
        NativeFixture fixture(false, source);
        auto result = fixture.call(
            "mapget_get_coverage",
            {{"mapId", "Native"}, {"layerId", "Road"}})["result"];
        CHECK(result["complete"] == true);
        auto range = result["items"][0]["ranges"][0];
        CHECK(range["bounds"] == NativeJson::array({-180, -90, 180, 90}));
        CHECK(range["coveredTileCount"] == (uint64_t{1} << (2 * level + 1)));
    }
    SECTION("Narrow antimeridian range retains west greater than east")
    {
        source->metadata.layers_.at("Road")->coverage_ = {
            {TileId::fromTileXY(7, 1, 3), TileId::fromTileXY(8, 2, 3), {}}};
        NativeFixture fixture(false, source);
        auto result = fixture.call(
            "mapget_get_coverage",
            {{"mapId", "Native"}, {"layerId", "Road"}})["result"];
        CHECK(
            result["items"][0]["ranges"][0]["bounds"] ==
            NativeJson::array({157.5, 22.5, -157.5, 67.5}));
    }
}

TEST_CASE("Native feature extraction accepts a single provenance partition", "[mcp-native]")
{
    NativeFixture fixture;
    auto args = NativeFixture::selection();
    auto partition = args["partitions"][0];
    args["query"] = "_.id";
    auto expected = fixture.call("mapget_extract_features", args)["result"];
    args.erase("partitions");
    args["partition"] = partition;
    auto single = fixture.call("mapget_extract_features", args);
    INFO(single.dump());
    CHECK(single["result"] == expected);
    args["partitions"] = NativeJson::array({partition});
    CHECK(fixture.call("mapget_extract_features", args).contains("error"));
    args.erase("partitions");
    args["partition"] = PartitionId::object(1).toJson();
    CHECK(fixture.call("mapget_extract_features", args).contains("error"));
}

TEST_CASE(
    "Native default feature summary exposes actual assignments without large payloads",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalGeometryPoints = 5000;
    source->textProperty = std::string(10000, 'x');
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    auto response = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(response["complete"] == true);
    REQUIRE(response["items"].size() == 1);
    auto const& row = response["items"][0];
    CHECK(row["rowComplete"] == true);
    CHECK_FALSE(row.contains("values"));
    auto const& summary = row["summary"];
    CHECK(summary["typeId"] == "Road");
    CHECK(summary["idParts"] == NativeJson({{"id", 1}}));
    CHECK(summary["geometries"][0]["pointCount"] == 5002);
    CHECK(summary["geometries"][0]["type"] == "LineString");
    CHECK(summary["attributeAssignments"][0]["layer"] == "rules");
    CHECK(summary["attributeAssignments"][0]["name"] == "speed");
    CHECK(summary["attributeAssignments"][0]["attributeIndex"] == 0);
    CHECK(summary["attributeAssignments"][0]["validityCount"] == 1);
    auto const& fields = summary["attributeAssignments"][0]["fields"];
    auto limit = std::find_if(
        fields.begin(),
        fields.end(),
        [](auto const& f) { return f["name"] == "limit"; });
    REQUIRE(limit != fields.end());
    CHECK((*limit)["value"] == 80);
    auto const& properties = summary["properties"];
    auto large = std::find_if(
        properties.begin(),
        properties.end(),
        [](auto const& f) { return f["name"] == "large"; });
    REQUIRE(large != properties.end());
    CHECK((*large)["value"]["value"] == std::to_string(INT64_MAX));
    auto text = std::find_if(
        properties.begin(),
        properties.end(),
        [](auto const& f) { return f["name"] == "text"; });
    REQUIRE(text != properties.end());
    CHECK((*text)["stringBytes"] == 10000);
    CHECK_FALSE(text->contains("value"));
    CHECK(summary["sourceDataReferences"][0]["layerId"] == "Raw");
    CHECK(response.dump().size() < 6000);
    args["query"] = "properties.layer.rules.speed.limit";
    auto focused = fixture.call("mapget_extract_features", args)["result"];
    CHECK(focused["items"][0]["values"] == NativeJson::parse("[[80]]"));
    args["query"] = "_";
    CHECK(fixture.call("mapget_extract_features", args)["result"]["complete"] == false);
}

TEST_CASE("Native assignment selectors preserve index and matching pagination", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalAttributes = 2;
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["attributeIndex"] = 1;
    CHECK(fixture.call("mapget_extract_features", args).contains("error"));
    CHECK(source->fills == 0);
    args["scope"] = "attribute";
    args["query"] = "limit";
    auto selected = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(selected["complete"] == true);
    REQUIRE(selected["items"].size() == 1);
    CHECK(selected["items"][0]["attributeIndex"] == 1);
    CHECK(selected["items"][0]["values"] == NativeJson::parse("[[90]]"));
    args["attributeName"] = "missing";
    CHECK(fixture.call("mapget_extract_features", args)["result"]["items"].empty());
    args["attributeName"] = "speed";
    args["attributeLayer"] = "wrong";
    CHECK(fixture.call("mapget_extract_features", args)["result"]["items"].empty());
    args["attributeLayer"] = "rules";
    CHECK(fixture.call("mapget_extract_features", args)["result"]["items"].size() == 1);
    args.erase("attributeIndex");
    args["predicate"] = "limit >= 90";
    args["limit"] = 1;
    auto page = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(page["complete"] == false);
    CHECK(page["items"][0]["attributeIndex"] == 1);
    REQUIRE(page["nextOffset"] == 1);
    args["offset"] = page["nextOffset"];
    page = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(page["complete"] == true);
    CHECK(page["items"][0]["attributeIndex"] == 2);
    CHECK(page["items"][0]["values"] == NativeJson::parse("[[91]]"));
}

TEST_CASE("Native feature-scope root mistakes explain the matching scope", "[mcp-native]")
{
    NativeFixture fixture;
    auto args = NativeFixture::selection();
    args["query"] = GENERATE("$feature.properties", "$.properties");
    auto result = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE_FALSE(result["issues"].empty());
    CHECK(
        result["issues"][0]["message"].get<std::string>().find("use _.properties") !=
        std::string::npos);
    args["query"] = "_.properties.large";
    result = fixture.call("mapget_extract_features", args)["result"];
    CHECK(result["issues"].empty());
    CHECK(result["items"][0]["values"][0][0]["value"] == std::to_string(INT64_MAX));
}

TEST_CASE(
    "Native source lookup defaults support large compound columns but honor explicit budgets",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalSourceCompounds = 100001;
    NativeFixture fixture(false, source);
    NativeJson args = {
        {"mapId", "Native"},
        {"partition", {{"kind", "tile"}, {"id", 131073}}},
        {"reference",
         {{"layerId", "Raw"}, {"address", std::to_string(SourceDataAddress(32, 64).u64())}}}};
    auto result = fixture.call("mapget_extract_source_data", args)["result"];
    REQUIRE(result["complete"] == true);
    CHECK(result["items"].size() == 2);
    args["maxWork"] = 100000;
    result = fixture.call("mapget_extract_source_data", args)["result"];
    CHECK(result["complete"] == false);
    CHECK(result["reason"] == "work_limit");
    CHECK(result["items"].empty());
}

TEST_CASE("Native sparse predicates aggregate warnings and reach later matches", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 150;
    source->lastFeatureText = "found";
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["predicate"] = "properties.text == 'found'";
    args["query"] = "_.id";
    auto result = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(result["complete"] == true);
    REQUIRE(result["items"].size() == 1);
    CHECK(result["items"][0]["featureId"] == "Road.151");
    REQUIRE(result["issues"].size() <= 3);
    CHECK(std::any_of(
        result["issues"].begin(),
        result["issues"].end(),
        [](auto const& issue) { return issue["occurrences"].template get<size_t>() > 100; }));
    for (auto const& issue : result["issues"])
        CHECK(issue["expression"] == args["predicate"]);

    args.erase("predicate");
    args.erase("query");
    args["expressions"] = {"properties.absent or false", "properties.absent or true"};
    args["limit"] = 1;
    auto different = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(different["issues"].size() == 2);
    CHECK(different["issues"][0]["expression"] != different["issues"][1]["expression"]);
    CHECK(different["issues"][0]["occurrences"] == 1);
    CHECK(different["issues"][1]["occurrences"] == 1);
}

TEST_CASE("Native discarded predicates do not consume the response byte budget", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 1500;
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["predicate"] = "_.id == 'Road.1501'";
    args["query"] = "_.id";
    auto result = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(result["complete"] == true);
    REQUIRE(result["items"].size() == 1);
    CHECK(result["items"][0]["featureId"] == "Road.1501");
    CHECK(result["issues"].empty());
}

TEST_CASE("Native extraction cursors resume scans and reject altered authority", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 100;
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["query"] = "_.id";
    args["predicate"] = "trace(_.id, 100, 'visited') == 'Road.101'";
    args["trace"] = true;
    args["limit"] = 1;
    args["maxWork"] = 300;
    size_t calls = 0, evaluations = 0, rows = 0;
    bool emptyContinuation = false;
    for (;;) {
        REQUIRE(++calls < 100);
        auto result = fixture.call("mapget_extract_features", args)["result"];
        if (result["traces"].contains("visited"))
            evaluations += result["traces"]["visited"]["values"].size();
        rows += result["items"].size();
        if (result["complete"] == true)
            break;
        REQUIRE(result.contains("nextCursor"));
        emptyContinuation |= result["items"].empty();
        args = {{"cursor", result["nextCursor"]}};
    }
    CHECK(rows == 1);
    CHECK(emptyContinuation);
    CHECK(calls > 1);
    CHECK(evaluations >= 101);
    // At most the interrupted/look-ahead context may repeat, never the accumulated prefix.
    CHECK(evaluations < 101 + calls * 2);
    CHECK(source->fills == 1);

    args = NativeFixture::selection();
    args["query"] = "_.id";
    args["limit"] = 1;
    auto first = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(first.contains("nextCursor"));
    NativeJson next{{"cursor", first["nextCursor"]}};
    auto second = fixture.call("mapget_extract_features", next)["result"];
    auto repeated = fixture.call("mapget_extract_features", next)["result"];
    REQUIRE(second["items"].size() == 1);
    CHECK(second["items"][0]["featureId"] == "Road.2");
    CHECK(repeated["items"] == second["items"]);
    auto changed = next;
    changed["predicate"] = "false";
    CHECK_FALSE(fixture.tools->acceptsArguments("mapget_extract_features", changed));
    fixture.principal.subject = "someone-else";
    CHECK(fixture.call("mapget_extract_features", next)["error"]["code"] == "cursor_unavailable");
    fixture.principal.subject = "reader";
    fixture.principal.datasourceHeaders["changed"] = "authority";
    CHECK(fixture.call("mapget_extract_features", next)["error"]["code"] == "cursor_unavailable");
    fixture.principal.datasourceHeaders.clear();
    fixture.service.remove(source);
    CHECK(fixture.call("mapget_extract_features", next)["error"]["code"] == "cursor_stale");
}

TEST_CASE("Native extraction cursors retain assignment and partition positions", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalAttributes = 4;
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["partitions"].push_back({{"kind", "tile"}, {"id", 131074}});
    args["scope"] = "attribute";
    args["query"] = "limit";
    args["limit"] = 1;
    NativeJson observed = NativeJson::array();
    for (size_t page = 0; page < 12; ++page) {
        auto result = fixture.call("mapget_extract_features", args)["result"];
        for (auto const& row : result["items"])
            observed
                .push_back({row["partition"]["id"], row["attributeIndex"], row["values"][0][0]});
        if (result["complete"] == true)
            break;
        REQUIRE(result.contains("nextCursor"));
        args = {{"cursor", result["nextCursor"]}};
    }
    REQUIRE(observed.size() == 10);
    for (size_t i = 0; i < observed.size(); ++i) {
        CHECK(observed[i][0] == (i < 5 ? 131073 : 131074));
        CHECK(observed[i][1] == i % 5);
        CHECK(observed[i][2] == (i % 5 ? 89 + i % 5 : 80));
    }
    CHECK(source->fills == 2);
}

TEST_CASE(
    "Native extraction cursor storage is bounded and partial rows remain pending",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 2;
    source->lastFeatureText = std::string(10000, 'x');
    NativeFixture fixture(false, source);
    auto settings = McpConfig{};
    settings.limits.resultBytes = 6000;
    fixture.resetTools(settings);
    auto args = NativeFixture::selection();
    args["query"] = "properties";
    auto first = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(first.contains("nextCursor"));
    auto retry =
        fixture.call("mapget_extract_features", {{"cursor", first["nextCursor"]}})["result"];
    CHECK(retry["complete"] == false);
    CHECK_FALSE(retry.contains("nextCursor"));
    CHECK_FALSE(retry.contains("nextOffset"));
    if (!retry["items"].empty()) {
        CHECK(retry["items"][0]["featureId"] == "Road.3");
        CHECK(retry["items"][0]["rowComplete"] == false);
    }
    args["query"] = "_.id";
    args["limit"] = 1;
    first = fixture.call("mapget_extract_features", args)["result"];
    for (size_t i = 0; i < 4; ++i)
        REQUIRE(fixture.call("mapget_extract_features", args)["result"].contains("nextCursor"));
    CHECK(
        fixture
            .call("mapget_extract_features", {{"cursor", first["nextCursor"]}})["error"]["code"] ==
        "cursor_unavailable");
    CHECK(
        fixture
            .call("mapget_extract_features", {{"cursor", std::string(48, '0')}})["error"]["code"] ==
        "cursor_unavailable");
}

TEST_CASE(
    "Native schema discovery groups enum owners and prioritizes exact symbols",
    "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    auto schema = std::make_shared<LayerSchema>();
    auto root =
        schema->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey("Road"), "Feature");
    auto text = schema->addSchema(simfil::Schema::Kind::String);
    auto domain = schema->addSchema(simfil::Schema::Kind::String);
    schema->setTypeName(domain, "Direction");
    for (int i = 0; i < 40; ++i) {
        schema->addFieldSchema(root, "forwardField" + std::to_string(i), text);
        schema->addEnumSymbol(domain, "A_FORWARD_" + std::to_string(i));
    }
    schema->addEnumSymbol(domain, "FORWARD");
    schema->addFieldSchema(root, "direction", domain);
    schema->addFieldSchema(root, "otherDirection", domain);
    schema->finalize();
    source->metadata.layers_.at("Road")->featureModelSchema_ = schema;
    NativeFixture fixture(false, source);
    auto args =
        NativeJson{{"mapId", "Native"}, {"layerId", "Road"}, {"find", "forward"}, {"limit", 1}};
    auto result = fixture.call("mapget_query_schema", args)["result"];
    REQUIRE(result["items"].size() == 1);
    auto const& item = result["items"][0];
    CHECK(item["schemaId"] == domain);
    CHECK(item["enum"]["matchingSymbols"][0] == "FORWARD");
    CHECK(item["enum"]["matchingSymbolCount"] == 41);
    CHECK(item["enum"]["matchingSymbols"].size() == 8);
    CHECK(item["enum"]["symbolsOmitted"] == true);
    REQUIRE(item["contexts"].size() == 2);
    CHECK(item["contexts"][0]["featureType"] == "Road");
    CHECK(item["contexts"][0].contains("featureQuery"));
    auto values = fixture.call("mapget_query_schema", item["enum"]["valuesArguments"])["result"];
    CHECK(values["items"][0]["values"].size() == 41);
    args.erase("find");
    args["featureType"] = "Road";
    auto overview = fixture.call("mapget_query_schema", args)["result"];
    auto const& descriptor = overview["items"][0]["values"][0];
    CHECK(descriptor["fieldCount"] == 42);
    CHECK(descriptor["fields"].size() == 16);
    CHECK(descriptor["childrenOmitted"] == true);
    CHECK(source->fills == 0);
}

TEST_CASE("Native schema field discovery provides executable feature queries", "[mcp-native]")
{
    NativeFixture fixture;
    auto found = fixture.call(
        "mapget_query_schema",
        {{"mapId", "Native"}, {"layerId", "Road"}, {"find", "typeId"}})["result"];
    REQUIRE(found["items"].size() == 1);
    auto args = NativeFixture::selection();
    args["query"] = found["items"][0]["contexts"][0]["featureQuery"];
    auto values = fixture.call("mapget_extract_features", args)["result"];
    CHECK(values["complete"] == true);
    CHECK(values["items"][0]["values"][0] == NativeJson::array({"Road"}));
}

TEST_CASE("Native extraction cursors expire with originating authority", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 1;
    NativeFixture fixture(false, source);
    fixture.principal.expiresAt = std::chrono::system_clock::now() + 200ms;
    auto args = NativeFixture::selection();
    args["query"] = "id";
    args["limit"] = 1;
    auto result = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(result.contains("nextCursor"));
    std::this_thread::sleep_until(fixture.principal.expiresAt + 10ms);
    fixture.principal.expiresAt = std::chrono::system_clock::now() + 1h;
    CHECK(
        fixture
            .call("mapget_extract_features", {{"cursor", result["nextCursor"]}})["error"]["code"] ==
        "cursor_unavailable");
}

TEST_CASE("Native extraction rejects oversized retained checkpoints", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 1;
    source->textProperty = std::string(34 * 1024 * 1024, 'x');
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["query"] = "id";
    args["limit"] = 1;
    auto result = fixture.call("mapget_extract_features", args)["result"];
    CHECK(result["complete"] == false);
    CHECK_FALSE(result.contains("nextCursor"));
    CHECK(result["nextOffset"] == 1);
    REQUIRE_FALSE(result["issues"].empty());
    CHECK(result["issues"].back()["phase"] == "budget");
}

TEST_CASE("Native extraction does not charge service-owned metadata per checkpoint", "[mcp-native]")
{
    auto source = std::make_shared<NativeSource>();
    source->additionalFeatures = 1;
    source->metadata.layers_.at("Road")->featureTypes_[0].uniqueIdCompositions_[0][0].description_ =
        std::string(34 * 1024 * 1024, 'x');
    NativeFixture fixture(false, source);
    auto args = NativeFixture::selection();
    args["query"] = "id";
    args["limit"] = 1;
    auto result = fixture.call("mapget_extract_features", args)["result"];
    REQUIRE(result.contains("nextCursor"));
    auto next =
        fixture.call("mapget_extract_features", {{"cursor", result["nextCursor"]}})["result"];
    REQUIRE(next["items"].size() == 1);
    CHECK(next["items"][0]["featureId"] == "Road.2");
    CHECK(next["complete"] == true);
}
