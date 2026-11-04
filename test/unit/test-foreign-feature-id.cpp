#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <sstream>

#include "mapget/model/featurelayer.h"
#include "mapget/model/geojson-import.h"
#include "mapget/model/incoming-relations.h"
#include "mapget/model/subsetlayer.h"

using namespace mapget;

namespace
{
/** The source intentionally has no declaration for the foreign Road type. */
std::shared_ptr<LayerInfo> localIdentityLayer()
{
    return LayerInfo::fromJson(R"({
        "layerId":"LocalLayer", "type":"Features", "featureTypes":[{
            "name":"Local", "uniqueIdCompositions":[[
                {"partId":"region", "datatype":"STR"},
                {"partId":"localId", "datatype":"U32"}
            ]]
        }]
    })"_json);
}

/** Construct a fresh independent tile owner for clone and wire tests. */
std::shared_ptr<PartitionFeatureLayer>
localIdentityTile(std::shared_ptr<LayerInfo> const& info, std::string const& map = "Source")
{
    return std::make_shared<PartitionFeatureLayer>(
        TileId::fromWgs84(11, 48, 13),
        "ForeignIds",
        map,
        info,
        std::make_shared<StringPool>("ForeignIds"));
}
}  // namespace

TEST_CASE(
    "Foreign feature IDs are self-describing without source schema or prefix",
    "[foreign-feature-id]")
{
    auto info = localIdentityLayer();
    auto tile = localIdentityTile(info);
    tile->setIdPrefix({{"region", "source-prefix"}});
    auto host = tile->newFeature("Local", {{"localId", 1}});
    KeyValueViewPairs const parts{
        {"tileId", int64_t{33587669}},
        {"roadId", int64_t{42}},
        {"text", std::string_view{"42.with%characters"}},
        {"large", std::numeric_limits<int64_t>::max()}};
    auto reference = tile->newFeatureId("Road", parts, "Target/Map");
    nlohmann::json const expected{
        {"mapId", "Target/Map"},
        {"typeId", "Road"},
        {"id",
         {{"tileId", 33587669},
          {"roadId", 42},
          {"text", "42.with%characters"},
          {"large", std::numeric_limits<int64_t>::max()}}}};

    CHECK(info->getTypeInfo("Road", false) == nullptr);
    CHECK(reference->toJson() == expected);
    CHECK(reference->keyValuePairs() == parts);
    CHECK(reference->mapId() == "Target/Map");
    CHECK(reference->typeId() == "Road");
    CHECK_THROWS(reference->toString());
    CHECK(host->id()->toString() == "Local.source-prefix.1");
    auto local = tile->newFeatureId("Local", {{"region", "other"}, {"localId", 2}}, "Source");
    CHECK_FALSE(local->externalMapId());
    CHECK(local->toString() == "Local.other.2");
    CHECK_THROWS(tile->newFeatureId("Road", parts));
    CHECK_THROWS(tile->newFeatureId("Road", parts, "Source"));
    CHECK_THROWS(tile->newFeature("Local", {{"localId", 1}}));

    host->addRelation(tile->newRelation("foreign", reference));
    auto rules = host->attributeLayers()->newLayer("rules");
    auto attribute = rules->newAttribute("target");
    attribute->validity()->newFeatureId(reference);
    auto transition = rules->newAttribute("transition");
    transition->validity()
        ->newFeatureTransition(reference, Validity::End, host->id(), Validity::Start, 3);
    auto before = tile->toJson();
    CHECK(before["features"][0]["relations"][0]["target"] == expected);
    CHECK(
        before["features"][0]["properties"]["layer"]["rules"]["target"]["validity"]["featureId"] ==
        expected);
    CHECK(
        before["features"][0]["properties"]["layer"]["rules"]["transition"]["validity"]["from"] ==
        expected);
    CHECK(
        before["features"][0]["properties"]["layer"]["rules"]["transition"]["validity"]["to"] ==
        host->id()->toString());

    SECTION("Binary roundtrip preserves the named container and exact integers")
    {
        std::ostringstream output;
        REQUIRE(tile->write(output));
        auto bytes = output.str();
        auto restored = std::make_shared<PartitionFeatureLayer>(
            std::vector<uint8_t>(bytes.begin(), bytes.end()),
            [&](auto const&, auto const&) { return info; },
            [&](auto const&) { return tile->strings(); });
        CHECK(restored->toJson() == before);
        CHECK(restored->resolve<FeatureId>(reference->addr())->toJson() == expected);
    }

    SECTION("Cloning into another source map does not import the target schema")
    {
        auto clone = localIdentityTile(info, "Clone");
        PartitionFeatureLayer::CloneCache cache;
        clone->clone(cache, tile, *host, "Local", {{"region", "cloned"}, {"localId", 1}});
        auto copied = clone->at(0)->getRelation(0)->target();
        CHECK(copied->toJson() == expected);
        CHECK(copied->externalMapId() == reference->externalMapId());
        CHECK(info->getTypeInfo("Road", false) == nullptr);
    }

    SECTION("Strict GeoJSON roundtrip does not require the foreign type")
    {
        auto imported = localIdentityTile(info);
        REQUIRE_NOTHROW(imported->fromJson(before));
        CHECK(imported->toJson() == before);
    }
}

TEST_CASE(
    "Named local GeoJSON references validate and foreign forms stay structural",
    "[foreign-feature-id]")
{
    auto info = localIdentityLayer();
    auto source = localIdentityTile(info);
    auto host = source->newFeature("Local", {{"region", "DE"}, {"localId", 1}});
    host->addRelation(source->newRelation("reference", host->id()));
    auto json = source->toJson();
    auto& target = json["features"][0]["relations"][0]["target"];

    SECTION("Explicit local names are ordered and typed by the local schema")
    {
        target = {
            {"mapId", "Source"},
            {"typeId", "Local"},
            {"id", {{"localId", "7"}, {"region", "DE"}}}};
        auto imported = localIdentityTile(info);
        REQUIRE_NOTHROW(imported->fromJson(json));
        auto id = imported->at(0)->getRelation(0)->target();
        CHECK_FALSE(id->externalMapId());
        CHECK(id->toJson() == "Local.DE.7");
        CHECK(id->keyValuePairs() == KeyValueViewPairs{{"region", "DE"}, {"localId", 7}});
    }

    SECTION("Malformed or ambiguous foreign references fail before canonicalization")
    {
        for (auto const& invalid :
             {nlohmann::json{{"mapId", "Target"}, {"id", "Local.DE.7"}},
              nlohmann::json{{"typeId", "Road"}, {"id", {{"localId", 7}}}},
              nlohmann::json{{"mapId", "Target"}, {"id", {{"localId", 7}}}},
              nlohmann::json{{"mapId", "Source"}, {"typeId", "Road"}, {"id", {{"localId", 7}}}},
              nlohmann::json{{"mapId", "Target"}, {"typeId", "Road"}, {"id", {{"localId", true}}}}})
        {
            CAPTURE(invalid);
            target = invalid;
            auto imported = localIdentityTile(info);
            CHECK_THROWS(imported->fromJson(json));
        }
    }
}

TEST_CASE(
    "Foreign validity never resolves a same-ID feature in the source map",
    "[foreign-feature-id]")
{
    auto tile = localIdentityTile(localIdentityLayer());
    auto host = tile->newFeature("Local", {{"region", "DE"}, {"localId", 1}});
    host->addPoint({11, 48, 0});
    auto foreign = tile->newFeatureId("Local", {{"region", "DE"}, {"localId", 1}}, "Other");
    auto validity =
        host->attributeLayers()
            ->newLayer("rules")
            ->newAttribute("foreign")
            ->validity()
            ->newFeatureId(foreign);
    std::string error;
    CHECK(validity->computeGeometry(host->geom(), &error).points_.empty());
    CHECK(error == "Foreign validity geometry requires target-map resolution.");
}

TEST_CASE(
    "Incoming foreign references compare named parts using the target schema",
    "[foreign-feature-id]")
{
    auto info = localIdentityLayer();
    auto source = localIdentityTile(info);
    auto target = localIdentityTile(info, "Target");
    auto feature = target->newFeature("Local", {{"region", "DE"}, {"localId", 1}});
    auto reference = source->newFeatureId("Local", {{"localId", 1}, {"region", "DE"}}, "Target");
    auto relation = source->newRelation("target", reference);
    CHECK(relationTargetsFeature(*relation, *feature));
    auto wrongMap = source->newRelation(
        "target",
        source->newFeatureId("Local", {{"localId", 1}, {"region", "DE"}}, "Other"));
    CHECK_FALSE(relationTargetsFeature(*wrongMap, *feature));
}

TEST_CASE("Foreign feature references reject malformed named parts", "[foreign-feature-id]")
{
    auto tile = localIdentityTile(localIdentityLayer());
    CHECK_THROWS(tile->newFeatureId("Road", {}, "Target"));
    CHECK_THROWS(tile->newFeatureId("Road", {{"localId", 1}, {"localId", 2}}, "Target"));
    CHECK_THROWS(tile->newFeatureId("Road", {{"", 1}}, "Target"));
    CHECK_THROWS(tile->newFeatureId("Road", {{"bad.part", 1}}, "Target"));
    CHECK_THROWS(tile->newFeatureId("", {{"localId", 1}}, "Target"));
    CHECK_THROWS(tile->newFeatureId("Road", {{"localId", 1}}, ""));
    auto longName = std::string(1025, 'a');
    CHECK_THROWS(tile->newFeatureId(longName, {{"localId", 1}}, "Target"));
    CHECK_THROWS(tile->newFeatureId("Road", {{longName, 1}}, "Target"));

    for (auto const& value :
         {nlohmann::json(nullptr),
          nlohmann::json(true),
          nlohmann::json(1.5),
          nlohmann::json::array({1}),
          nlohmann::json::object({{"nested", 1}}),
          nlohmann::json(std::numeric_limits<uint64_t>::max())})
    {
        CAPTURE(value);
        CHECK_THROWS(parseFeatureIdPartsJson(nlohmann::json{{"localId", value}}));
    }
    auto excessive = nlohmann::json::object();
    for (unsigned i = 0; i < 256; ++i)
        excessive["part" + std::to_string(i)] = i;
    CHECK_THROWS(parseFeatureIdPartsJson(excessive));
    CHECK(
        parseFeatureIdPartsJson(nlohmann::json{{"text", "001"}, {"number", -1}}) ==
        KeyValuePairs{{"number", int64_t{-1}}, {"text", std::string{"001"}}});
}

TEST_CASE(
    "Named local references normalize only through target compositions",
    "[foreign-feature-id]")
{
    auto info = localIdentityLayer();
    ParsedFeatureId parsed;
    CHECK(normalizeFeatureIdParts(
        "Local",
        {{"localId", int64_t{7}}, {"region", std::string{"DE"}}},
        *info,
        parsed));
    CHECK(
        parsed.keyValuePairs_ ==
        KeyValuePairs{{"region", std::string{"DE"}}, {"localId", int64_t{7}}});
    CHECK_FALSE(normalizeFeatureIdParts("Road", {{"localId", int64_t{7}}}, *info, parsed));
    CHECK_FALSE(normalizeFeatureIdParts("Local", {{"region", std::string{"DE"}}}, *info, parsed));
    CHECK_FALSE(normalizeFeatureIdParts(
        "Local",
        {{"region", std::string{"DE"}}, {"localId", int64_t{-1}}},
        *info,
        parsed));
    CHECK_FALSE(normalizeFeatureIdParts(
        "Local",
        {{"region", std::string{"DE"}}, {"localId", int64_t{1}}, {"localId", int64_t{2}}},
        *info,
        parsed));
}

TEST_CASE(
    "Subset feature IDs retain unknown foreign named parts on the wire",
    "[foreign-feature-id][test.subsetlayer]")
{
    auto info = localIdentityLayer();
    auto strings = std::make_shared<StringPool>("ForeignSubset");
    auto subset = std::make_shared<PartitionSubsetLayer>(
        TileId::fromWgs84(11, 48, 13),
        "ForeignSubset",
        "Source",
        info,
        strings);
    auto foreign = subset->newFeatureId("Unknown", {{"value", "001"}, {"index", -7}}, "Target");
    auto expected = foreign->toJson();
    auto channel = subset->newChannel("references", Scope::Feature, 0, std::nullopt);
    auto entry = channel->newFeatureEntry(foreign, subset->newGeometryCollection(1, true));
    CHECK(entry->toJson()["featureId"] == expected);
    std::ostringstream output;
    REQUIRE(subset->write(output));
    auto bytes = output.str();
    auto restored = std::make_shared<PartitionSubsetLayer>(
        std::vector<uint8_t>(bytes.begin(), bytes.end()),
        [&](auto const&, auto const&) { return info; },
        [&](auto const&) { return strings; });
    CHECK(restored->resolve<FeatureId>(foreign->addr())->toJson() == expected);
    CHECK(restored->toJson() == subset->toJson());
}
