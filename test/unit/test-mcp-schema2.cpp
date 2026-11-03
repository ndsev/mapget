#include "../../libs/http-service/src/mcp-native-tools.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/sourcedatalayer.h"
#include "mapget/service/memcache.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <regex>

using namespace mapget;
using namespace std::chrono_literals;
using Schema2Json = nlohmann::json;

namespace
{
/** A native-only contract, including reference-only IDs and domains with identical raw codes. */
class Schema2NativeSource final : public DataSource
{
public:
    DataSourceInfo metadata;
    std::atomic_size_t fills{0};
    simfil::SchemaId speed = 0, count = 0, flags = 0, properties = 0;
    simfil::SchemaId enabled = 0, payload = 0;

    /** Publish one finalized graph without any legacy inline schema fields. */
    explicit Schema2NativeSource(
        std::string map = "Schema2",
        std::string typeName = "fixture.Speed",
        bool declareRelations = false,
        std::string layerName = "Road",
        size_t extraRawLayers = 0)
    {
        auto owner = std::make_shared<MapSchema>(MapFormat{"fixture", "2", {{"roads", "1"}}});
        auto layer = owner->newLayer(layerName, LayerType::Features);
        auto root = layer->addSchema(simfil::Schema::Kind::Object, LayerSchema::featureKey("Road"));
        properties = layer->addSchema(
            simfil::Schema::Kind::Object,
            LayerSchema::featurePropertiesKey("Road"));
        speed = layer->addSchema(simfil::Schema::Kind::Int);
        count = layer->addSchema(simfil::Schema::Kind::Int);
        flags = layer->addSchema(simfil::Schema::Kind::Bitmask);
        enabled = layer->addSchema(simfil::Schema::Kind::Bool);
        payload = layer->addSchema(simfil::Schema::Kind::Bytes);
        layer->setTypeName(speed, std::move(typeName));
        layer->setTypeName(count, "fixture.Count");
        layer->setTypeName(flags, "fixture.Access");
        simfil::Schema::NumericRange ordinary{int64_t{1}, int64_t{253}};
        layer->setNumericDomain(
            speed,
            {.encoding = "uint8",
             .unit = "km/h",
             .minimum = int64_t{0},
             .maximum = int64_t{255},
             .ordinaryRanges = {&ordinary, 1},
             .complete = true});
        layer->addNamedValue(
            speed,
            {.name = "UNLIMITED",
             .value = int64_t{255},
             .role = simfil::Schema::NamedValue::Role::Special,
             .meaning = simfil::Schema::NamedValue::Meaning::Unbounded});
        layer->addNamedValue(
            count,
            {.name = "UNKNOWN_COUNT",
             .value = int64_t{255},
             .role = simfil::Schema::NamedValue::Role::Special,
             .meaning = simfil::Schema::NamedValue::Meaning::Unknown});
        layer->setBitWidth(flags, 64);
        layer->addNamedValue(
            flags,
            {.name = "CAR", .value = uint64_t{1}, .role = simfil::Schema::NamedValue::Role::Flag});
        layer->setOpen(properties, true);
        layer->addFieldSchema(root, "properties", properties);
        layer->addFieldSchema(properties, "speed", speed);
        layer->addFieldSchema(properties, "count", count);
        layer->addFieldSchema(properties, "flags", flags);
        layer->addFieldSchema(properties, "enabled", enabled);
        layer->addFieldSchema(properties, "payload", payload);
        auto contextOnly = layer->addSchema(simfil::Schema::Kind::Int);
        layer->setTypeName(contextOnly, "fixture.ContextOnly");
        layer->addFieldSchema(properties, "$context", contextOnly);
        layer->setFieldRole(properties, "$context", simfil::Schema::FieldRole::Context);
        layer->addFieldSchema(properties, "$ordinary", enabled);
        layer->addFeatureType(
            FeatureTypeInfo::fromJson(Schema2Json::parse(R"({
            "name":"Road","uniqueIdCompositions":[[
                {"partId":"id","datatype":"U64"},
                {"partId":"variant","datatype":"STR","isOptional":true,"isSynthetic":true}
            ],[{"partId":"alias","datatype":"STR"}]]
        })")),
            root);
        layer->addFeatureType(
            FeatureTypeInfo::fromJson(Schema2Json::parse(R"({
            "name":"Target","uniqueIdCompositions":[[{"partId":"id","datatype":"U64"}]]
        })")),
            simfil::NoSchemaId,
            false);
        layer->setRootSchema(root);
        if (declareRelations)
            layer->setJsonSchemaAnnotations(
                root,
                {{"x-mapget",
                  {{"relationDeclarations",
                    Schema2Json::array(
                        {{{"ownerType", "Road"},
                          {"name", "next"},
                          {"targetTypes", {"Road"}},
                          {"complete", true},
                          {"locality", "same-container"}}})}}}});
        auto raw = owner->newLayer("Raw", LayerType::SourceData);
        raw->setRootSchema(properties);
        auto unavailable = owner->newLayer("NotAdvertised", LayerType::SourceData);
        unavailable->setRootSchema(properties);
        for (size_t index = 0; index < extraRawLayers; ++index)
            owner->newLayer("ExtraRaw" + std::to_string(index), LayerType::SourceData)
                ->setRootSchema(properties);
        owner->finalize();

        metadata = DataSourceInfo::fromJson(
            {{"mapId", map},
             {"stringPoolId", "schema2-" + map},
             {"mapFormat", owner->format().toJson()},
             {"schemaId", owner->schemaId()},
             {"layers", {{layerName, {{"type", "Features"}}}, {"Raw", {{"type", "SourceData"}}}}}},
            [owner](std::string_view) { return owner; });
    }

    /** Return the same immutable owner in every small metadata snapshot. */
    DataSourceInfo info() override { return metadata; }

    /** Exercise explicit qualification, parent-derived provenance and intrinsically typed flags. */
    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        ++fills;
        auto feature = tile->newFeature("Road", {{"id", int64_t{1}}});
        auto values = feature->attributes();
        auto binding = metadata.mapSchema_->binding();
        values->addField("speed", tile->newValue(int64_t{255}));
        values->addField("count", tile->newValue(int64_t{255}));
        values->addField("detached", tile->newQualifiedValue(int64_t{255}, {binding, speed}));
        values->addField("plain", tile->newValue(int64_t{255}));
        values->addField("flags", tile->newBitmask(UINT64_C(0x8000000000000001), {binding, flags}));
        values->addField("zeroFlags", tile->newBitmask(0, {binding, flags}));
        values->addField("allFlags", tile->newBitmask(UINT64_MAX, {binding, flags}));
        values->addField("enabled", tile->newQualifiedValue(true, {binding, enabled}));
        simfil::ByteArray bytes(std::string("\x00\x01\xff", 3));
        values->addField("payload", tile->newQualifiedValue(bytes, {binding, payload}));
        values->addField("plainPayload", tile->newValue(bytes));
        auto compound = tile->newObject();
        if (auto result = compound->setSchema(properties); !result)
            throw std::runtime_error(result.error().message);
        compound->addField("speed", tile->newValue(int64_t{50}));
        values->addField("compound", compound);
        auto literal = tile->newObject();
        literal->addBool("_bitmask", true);
        values->addField("literal", literal);
    }

    /** Raw source records use the same graph, not a schema-name or field-name heuristic. */
    void fill(PartitionSourceDataLayer::Ptr const& tile) override
    {
        ++fills;
        auto object = tile->newObject();
        if (auto result = object->setSchema(properties); !result)
            throw std::runtime_error(result.error().message);
        object->addField("speed", tile->newValue(int64_t{255}));
        object->addField(
            "flags",
            tile->newBitmask(
                UINT64_C(0x8000000000000001),
                {metadata.mapSchema_->binding(), flags}));
        tile->addRoot(object);
    }
};

/** Invoke the real native dispatcher on a service worker, without transport or external providers.
 */
class Schema2NativeFixture
{
public:
    Service service{std::make_shared<MemCache>(8), false, 0ms, 1};
    std::shared_ptr<Schema2NativeSource> source = std::make_shared<Schema2NativeSource>();
    detail::McpNativeTools::Principal
        principal{"schema2", "reader", std::chrono::system_clock::now() + 1h, true, false};
    std::shared_ptr<detail::McpNativeTools> tools;

    /** Register the already-ready source before native discovery. */
    Schema2NativeFixture()
    {
        service.add(source);
        tools = std::make_shared<detail::McpNativeTools>(
            service,
            McpConfig{},
            [] { return Schema2Json::object(); },
            nullptr,
            [] { return Schema2Json::object(); },
            [](auto const&, auto const&) { return Schema2Json::object(); });
    }

    /** Validate every successful response against the advertised native contract. */
    Schema2Json call(std::string const& name, Schema2Json arguments = Schema2Json::object())
    {
        auto promise = std::make_shared<std::promise<Schema2Json>>();
        auto future = promise->get_future();
        (void)tools->invoke(
            principal,
            name,
            std::move(arguments),
            [promise](Schema2Json result) { promise->set_value(std::move(result)); });
        REQUIRE(future.wait_for(5s) == std::future_status::ready);
        auto reply = future.get();
        INFO(reply.dump());
        if (reply.contains("result"))
            CHECK(tools->acceptsResult(name, reply["result"]));
        return reply;
    }

    /** Bound scans to one explicit, ordinary packed tile. */
    static Schema2Json selection(std::string layer = "Road")
    {
        return {
            {"mapId", "Schema2"},
            {"layerId", std::move(layer)},
            {"partitions", Schema2Json::array({{{"kind", "tile"}, {"id", 131073}}})}};
    }
};
}  // namespace

TEST_CASE(
    "Schema2 MCP discovery reads native identity definitions without inline graphs",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto before = fixture.source->metadata.mapSchema_;
    auto result = fixture.call("mapget_list_sources", {{"mapId", "Schema2"}}).at("result");
    REQUIRE(result["complete"] == true);
    REQUIRE(result["items"].size() == 1);
    auto item = result["items"][0];
    CHECK(item["schemaId"].get<std::string>() == before->schemaId());
    CHECK(item["mapFormat"] == before->format().toJson());
    REQUIRE(item["layers"].size() == 2);
    auto road = item["layers"][1];  // Canonical layer IDs are sorted: Raw, Road.
    REQUIRE(road["layerId"] == "Road");
    REQUIRE(road["featureTypes"].size() == 2);
    CHECK(road["hasSchema"] == true);
    for (auto const& type : road["featureTypes"]) {
        if (type["name"] == "Road") {
            CHECK(type["produced"] == true);
            CHECK(type["uniqueIdCompositions"][0][0]["datatype"] == "U64");
            CHECK(type["uniqueIdCompositions"][0][1]["isOptional"] == true);
            CHECK(type["uniqueIdCompositions"][0][1]["isSynthetic"] == true);
            CHECK(type["uniqueIdCompositions"][1][0]["partId"] == "alias");
        }
        else {
            CHECK(type["name"] == "Target");
            CHECK(type["produced"] == false);
        }
    }
    CHECK(item.dump().find("featureModelSchema") == std::string::npos);
    CHECK(item.dump().find("schemaFeatureTypes") == std::string::npos);
    CHECK(item.dump().find("NotAdvertised") == std::string::npos);
    CHECK(fixture.source->fills == 0);
    CHECK(fixture.source->metadata.mapSchema_ == before);
    auto compact = fixture.call("mapget_list_sources").at("result")["items"][0];
    CHECK(compact["schemaId"].get<std::string>() == before->schemaId());
    CHECK(compact["layers"][0]["featureTypes"][0].contains("uniqueIdCompositions") == false);
}

TEST_CASE(
    "Schema2 MCP schema queries and validation retain exact graph identity",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    Schema2Json args{{"mapId", "Schema2"}, {"layerId", "Road"}};
    auto roots = fixture.call("mapget_query_schema", args).at("result");
    CHECK(roots["mapSchemaId"] == fixture.source->metadata.schemaId_);
    REQUIRE(roots["items"].size() == 1);
    CHECK(roots["items"][0]["featureType"] == "Road");
    args["expression"] = "properties.speed == 255";
    args["featureType"] = "Road";
    auto valid = fixture.call("mapget_validate_expression", args).at("result");
    CHECK(valid["mapSchemaId"] == fixture.source->metadata.schemaId_);
    CHECK(valid["items"][0]["valid"] == true);
    auto raw = fixture.call("mapget_query_schema", {{"mapId", "Schema2"}, {"layerId", "Raw"}})
                   .at("result");
    REQUIRE(raw["items"].size() == 1);
    CHECK(raw["items"][0]["schemaId"] == fixture.source->properties);
    CHECK(raw["items"][0]["layerId"] == "Raw");
    CHECK_FALSE(raw["items"][0].contains("featureType"));
    CHECK(fixture.source->fills == 0);
}

TEST_CASE("Schema2 MCP exposes native numeric and named-value descriptors", "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    Schema2Json args{
        {"mapId", "Schema2"},
        {"layerId", "Road"},
        {"schemaId", fixture.source->speed},
        {"query", "numericDomain"}};
    auto numeric = fixture.call("mapget_query_schema", args).at("result");
    REQUIRE(numeric["complete"] == true);
    auto domain = numeric["items"][0]["values"][0];
    CHECK(domain["unit"] == "km/h");
    CHECK(domain["minimum"] == 0);
    CHECK(domain["maximum"] == 255);
    CHECK(domain["ordinaryRanges"][0] == Schema2Json({{"min", 1}, {"max", 253}}));
    args["query"] = "namedValues.*";
    auto special = fixture.call("mapget_query_schema", args).at("result");
    CHECK(special["items"][0]["values"][0]["name"] == "UNLIMITED");
    CHECK(special["items"][0]["values"][0]["value"] == 255);
    CHECK(special["items"][0]["values"][0]["meaning"] == "unbounded");
    args.erase("schemaId");
    args.erase("query");
    args["find"] = "UNLIMITED";
    auto found = fixture.call("mapget_query_schema", args).at("result");
    REQUIRE_FALSE(found["items"].empty());
    CHECK(found["items"][0]["schemaId"] == fixture.source->speed);
    CHECK(found["items"][0]["namedValues"]["matchingNames"][0] == "UNLIMITED");
    args.erase("find");
    args["schemaId"] = fixture.source->flags;
    args["query"] = "bitWidth";
    CHECK(fixture.call("mapget_query_schema", args)["result"]["items"][0]["values"][0] == 64);
    CHECK(fixture.source->fills == 0);
}

TEST_CASE(
    "Schema2 MCP projections preserve exact scalar domains and full-width flags",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto args = fixture.selection();
    args["expressions"] = {
        "properties.speed",
        "properties.count",
        "properties.detached",
        "properties.plain",
        "properties.speed + 0",
        "properties.flags",
        "properties.compound",
        "null",
        "properties.literal",
        "properties.zeroFlags",
        "properties.allFlags",
        "trace(properties.speed, 1, 'speed')",
        "properties.enabled",
        "properties.payload",
        "true",
        "properties.plainPayload"};
    args["trace"] = true;
    auto result = fixture.call("mapget_extract_features", args).at("result");
    REQUIRE(result["complete"] == true);
    REQUIRE(result["items"].size() == 1);
    auto values = result["items"][0]["values"];
    INFO(values.dump());
    auto token = fixture.source->metadata.schemaId_;
    for (size_t i : {0, 1, 2}) {
        CHECK(values[i][0]["$mapget"] == "qualified");
        CHECK(values[i][0]["value"] == 255);
        CHECK(values[i][0]["schema"] == token);
    }
    CHECK(values[0][0]["domain"] == fixture.source->speed);
    CHECK(values[1][0]["domain"] == fixture.source->count);
    CHECK(values[2][0]["domain"] == fixture.source->speed);
    CHECK(values[3][0] == 255);
    CHECK(values[4][0] == 255);
    CHECK(
        values[5][0] ==
        Schema2Json(
            {{"_bitmask", true},
             {"bits", "0x8000000000000001"},
             {"schema", token},
             {"domain", fixture.source->flags}}));
    CHECK(values[6][0]["speed"]["value"] == 50);
    CHECK(values[6][0]["speed"]["domain"] == fixture.source->speed);
    CHECK(values[7][0].is_null());
    CHECK(values[8][0] == Schema2Json({{"$mapget", "object"}, {"value", {{"_bitmask", true}}}}));
    CHECK(values[9][0]["bits"] == "0x0000000000000000");
    CHECK(values[10][0]["bits"] == "0xffffffffffffffff");
    CHECK(values[11][0] == values[0][0]);
    CHECK(result["traces"]["speed"]["values"][0] == values[0][0]);
    CHECK(
        values[12][0] ==
        Schema2Json(
            {{"$mapget", "qualified"},
             {"value", true},
             {"schema", token},
             {"domain", fixture.source->enabled}}));
    Schema2Json bytes{{"$mapget", "bytes"}, {"hex", "0001ff"}};
    CHECK(
        values[13][0] ==
        Schema2Json(
            {{"$mapget", "qualified"},
             {"value", bytes},
             {"schema", token},
             {"domain", fixture.source->payload}}));
    CHECK(values[14][0] == true);
    CHECK(values[15][0] == bytes);
    CHECK(result.dump().find("resultSchema") == std::string::npos);

    auto rawArgs = fixture.selection("Raw");
    rawArgs["query"] = "_";
    auto raw = fixture.call("mapget_extract_source_data", rawArgs).at("result");
    REQUIRE(raw["complete"] == true);
    CHECK(raw["items"][0]["values"][0]["speed"]["$mapget"] == "qualified");
    CHECK(raw["items"][0]["values"][0]["speed"]["schema"] == token);
    CHECK(raw["items"][0]["values"][0]["speed"] == values[0][0]);
    CHECK(raw["items"][0]["values"][0]["flags"] == values[5][0]);
}

TEST_CASE("Schema2 MCP schema access cannot cross an unauthorized source", "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto hidden = std::make_shared<Schema2NativeSource>("Hidden", "private.SecretSpeed");
    hidden->requireAuthHeaderRegexMatchOption("x-schema-test", std::regex("allowed"));
    fixture.service.add(hidden);
    auto inventory = fixture.call("mapget_list_sources").at("result");
    CHECK(inventory["items"].size() == 1);
    CHECK(inventory.dump().find(hidden->metadata.schemaId_) == std::string::npos);
    CHECK(fixture.call("mapget_query_schema", {{"mapId", "Hidden"}, {"layerId", "Road"}})
              .contains("error"));
    CHECK(hidden->fills == 0);
}

TEST_CASE(
    "Schema2 MCP selects exact formats and artifacts without tile IO",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto owner = fixture.source->metadata.mapSchema_;
    auto inventory = fixture.call("mapget_list_schemas").at("result");
    REQUIRE(inventory["items"].size() == 1);
    CHECK(inventory["items"][0]["mapSchemaId"] == owner->schemaId());
    CHECK(inventory["items"][0]["mapFormat"] == owner->format().toJson());
    CHECK(inventory["items"][0]["consumers"].size() == 1);

    Schema2Json args{{"mapFormat", owner->format().toJson()}, {"layerId", "Road"}};
    auto result = fixture.call("mapget_query_schema", args).at("result");
    CHECK(result["complete"] == true);
    CHECK(result["mapSchemaId"] == owner->schemaId());
    args["expression"] = "attributes.speed > 10";
    auto validation = fixture.call("mapget_validate_expression", args).at("result");
    CHECK(validation["items"][0]["valid"] == true);

    // Format selection sees schema roots, not only the chosen map's advertised data layers.
    auto extra =
        fixture
            .call(
                "mapget_query_schema",
                {{"mapSchemaId", owner->schemaId()}, {"layerId", "NotAdvertised"}})
            .at("result");
    CHECK(extra["complete"] == true);
    CHECK(fixture.call("mapget_query_schema", {{"mapId", "Schema2"}, {"layerId", "NotAdvertised"}})
              .contains("error"));
    CHECK(fixture.source->fills == 0);
}

TEST_CASE(
    "Schema discovery skips lookup-only context but not dollar-named data",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    Schema2Json args{{"mapId", "Schema2"}, {"layerId", "Raw"}, {"find", "ContextOnly"}};
    auto implicit = fixture.call("mapget_query_schema", args).at("result");
    CHECK(implicit["complete"] == true);
    CHECK(implicit["items"].empty());
    args["find"] = "$ordinary";
    auto data = fixture.call("mapget_query_schema", args).at("result");
    CHECK_FALSE(data["items"].empty());
    args.erase("find");
    args["query"] = "fields[\"$context\"].typename";
    auto explicitLookup = fixture.call("mapget_query_schema", args).at("result");
    CHECK(explicitLookup["items"][0]["values"] == Schema2Json::array({"fixture.ContextOnly"}));
    CHECK(fixture.source->fills == 0);
}

TEST_CASE("Schema2 format ambiguity requires an authorized exact artifact", "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto other = std::make_shared<Schema2NativeSource>("Other", "fixture.OtherSpeed");
    fixture.service.add(other);
    Schema2Json args{{"mapFormat", other->metadata.mapFormat_.toJson()}, {"layerId", "Road"}};
    auto ambiguous = fixture.call("mapget_query_schema", args).at("result");
    CHECK(ambiguous["complete"] == false);
    CHECK(ambiguous["reason"] == "ambiguous_schema");
    REQUIRE(ambiguous["items"].size() == 2);
    for (auto const& item : ambiguous["items"])
        CHECK(
            fixture.call("mapget_query_schema", item["selectArguments"])["result"]["mapSchemaId"] ==
            item["mapSchemaId"]);
    args["mapSchemaId"] = other->metadata.schemaId_;
    CHECK(fixture.call("mapget_query_schema", args)["result"]["complete"] == true);
    args["mapId"] = "Other";
    CHECK(fixture.call("mapget_query_schema", args).contains("error"));
    fixture.service.remove(other);
    args.erase("mapId");
    CHECK(fixture.call("mapget_query_schema", args).contains("error"));
    CHECK(other->fills == 0);
}

TEST_CASE("Schema2 format selection intersects the requested layer", "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto other =
        std::make_shared<Schema2NativeSource>("Other", "fixture.OtherSpeed", false, "Lane");
    fixture.service.add(other);
    auto result = fixture.call(
        "mapget_query_schema",
        {{"mapFormat", fixture.source->metadata.mapFormat_.toJson()}, {"layerId", "Road"}});
    REQUIRE(result.contains("result"));
    CHECK(result["result"]["complete"] == true);
    CHECK(result["result"]["mapSchemaId"] == fixture.source->metadata.schemaId_);
    CHECK(fixture.source->fills == 0);
    CHECK(other->fills == 0);
}

TEST_CASE(
    "Schema2 inventory sharing and authorization are provider scoped",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    auto shared = std::make_shared<Schema2NativeSource>("Shared");
    fixture.service.add(shared);
    auto hidden = std::make_shared<Schema2NativeSource>("Hidden", "private.SecretSpeed");
    hidden->requireAuthHeaderRegexMatchOption("x-schema-test", std::regex("allowed"));
    fixture.service.add(hidden);
    auto inventory = fixture.call("mapget_list_schemas").at("result");
    REQUIRE(inventory["items"].size() == 1);
    CHECK(inventory["items"][0]["consumers"].size() == 2);
    CHECK(inventory.dump().find("Hidden") == std::string::npos);
    CHECK(fixture
              .call(
                  "mapget_query_schema",
                  {{"mapSchemaId", hidden->metadata.schemaId_}, {"layerId", "Road"}})
              .contains("error"));
    fixture.service.remove(fixture.source);
    CHECK(
        fixture.call(
            "mapget_query_schema",
            {{"mapSchemaId", shared->metadata.schemaId_},
             {"layerId", "Road"}})["result"]["complete"] == true);
    fixture.service.remove(shared);
    CHECK(fixture.call("mapget_list_schemas")["result"]["items"].empty());
}

TEST_CASE(
    "Schema2 inventory filters providers before spending work on unrelated layers",
    "[mcp-native][schema2][schema-inventory]")
{
    Schema2NativeFixture fixture;
    auto excluded = std::make_shared<
        Schema2NativeSource>("Excluded", "fixture.OtherSpeed", false, "Road", 1024);
    auto shared = std::make_shared<Schema2NativeSource>("Shared");
    fixture.service.add(excluded);
    fixture.service.add(shared);
    std::string selectedSourceId;
    for (auto const& source : fixture.service.sourceCatalog(AuthHeaders{}, false).sources)
        if (source.info && source.info->mapId_ == "Schema2")
            selectedSourceId = source.descriptor.sourceId;
    REQUIRE_FALSE(selectedSourceId.empty());

    // JSON encoding has its own per-value work cost, in addition to registry traversal.
    auto unfiltered = fixture.call("mapget_list_schemas", {{"maxWork", 512}}).at("result");
    CHECK(unfiltered["complete"] == false);
    CHECK(unfiltered["reason"] == "work_limit");
    for (auto const& selector :
         {Schema2Json{{"mapId", "Schema2"}},
          Schema2Json{{"sourceId", selectedSourceId}},
          Schema2Json{{"mapId", "Schema2"}, {"sourceId", selectedSourceId}}})
    {
        auto arguments = selector;
        arguments["maxWork"] = 512;
        auto result = fixture.call("mapget_list_schemas", arguments).at("result");
        REQUIRE(result["complete"] == true);
        REQUIRE(result["items"].size() == 1);
        CHECK(result["items"][0]["mapSchemaId"] == fixture.source->metadata.schemaId_);
        REQUIRE(result["items"][0]["consumers"].size() == 1);
        CHECK(result["items"][0]["consumers"][0]["mapId"] == "Schema2");
        CHECK(result["items"][0]["consumers"][0]["sourceId"] == selectedSourceId);
    }
    auto mismatch =
        fixture
            .call(
                "mapget_list_schemas",
                {{"mapId", "Excluded"}, {"sourceId", selectedSourceId}, {"maxWork", 32}})
            .at("result");
    CHECK(mismatch["complete"] == true);
    CHECK(mismatch["items"].empty());
    CHECK(fixture.source->fills == 0);
    CHECK(excluded->fills == 0);
    CHECK(shared->fills == 0);
}

TEST_CASE(
    "Format-qualified schema discovery exposes native identity contracts",
    "[mcp-native][schema2]")
{
    Schema2NativeFixture fixture;
    Schema2Json args{
        {"mapFormat", fixture.source->metadata.mapFormat_.toJson()},
        {"layerId", "Road"},
        {"featureType", "Road"}};
    auto result = fixture.call("mapget_query_schema", args).at("result");
    auto contract = result["items"][0]["contract"];
    CHECK(contract["produced"] == true);
    CHECK(contract["uniqueIdCompositions"][0][0]["partId"] == "id");
    CHECK(contract["uniqueIdCompositions"][1][0]["partId"] == "alias");
    CHECK(contract["forwardRelationsKnown"] == false);
    CHECK(contract["forwardRelations"].empty());
    args["featureType"] = "Target";
    result = fixture.call("mapget_query_schema", args).at("result");
    REQUIRE(result["complete"] == true);
    CHECK(result["items"][0]["schemaId"].is_null());
    CHECK(result["items"][0]["contract"]["produced"] == false);
    CHECK(result["items"][0]["values"].empty());
    // An identity-only declaration does not authorize evaluation against a nonexistent root.
    args["expression"] = "true";
    CHECK(fixture.call("mapget_validate_expression", args).contains("error"));
    CHECK(fixture.source->fills == 0);

    auto declared = std::make_shared<Schema2NativeSource>("Declared", "fixture.Speed", true);
    fixture.service.add(declared);
    args = {
        {"mapSchemaId", declared->metadata.schemaId_},
        {"layerId", "Road"},
        {"featureType", "Road"}};
    contract = fixture.call("mapget_query_schema", args)["result"]["items"][0]["contract"];
    CHECK(contract["forwardRelationsKnown"] == true);
    REQUIRE(contract["forwardRelations"].size() == 1);
    CHECK(contract["forwardRelations"][0]["name"] == "next");
    CHECK(contract["forwardRelations"][0]["complete"] == true);
    CHECK(contract["forwardRelations"][0]["locality"] == "same-container");
    CHECK(declared->fills == 0);
}
