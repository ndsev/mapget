#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <mutex>
#include <optional>
#include <vector>

#include "mapget/model/featurelayer-search.h"
#include "mapget/model/searchresultlayer.h"
#include "mapget/model/stream.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "../../libs/http-service/src/tiles-request-json.h"

using namespace mapget;

namespace
{

std::shared_ptr<LayerInfo> makeSearchResultLayerInfo()
{
    return LayerInfo::fromJson(R"({
        "layerId": "SearchableLayer",
        "type": "Features",
        "featureTypes": [
            {
                "name": "Road",
                "uniqueIdCompositions": [
                    [
                        {"partId": "tileId", "description": "Synthetic tile id.", "datatype": "U32"},
                        {"partId": "roadId", "description": "Synthetic road id.", "datatype": "U64"}
                    ]
                ]
            }
        ]
    })"_json);
}

TileSearchResultLayer::Ptr makeSearchResultLayer()
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("SearchResultSourceNode");
    return std::make_shared<TileSearchResultLayer>(
        TileId(0x1234),
        strings->nodeId_,
        "TestMap",
        layerInfo,
        strings);
}

class StagedSearchDataSource : public DataSource
{
public:
    StagedSearchDataSource()
        : info_(DataSourceInfo::fromJson(R"({
            "nodeId": "SearchStageNode",
            "mapId": "TestMap",
            "layers": {
                "SearchableLayer": {
                    "type": "Features",
                    "stages": 2,
                    "stageLabels": ["base", "details"],
                    "highFidelityStage": 0,
                    "featureTypes": [
                        {
                            "name": "Road",
                            "uniqueIdCompositions": [[
                                {"partId": "tileId", "description": "Synthetic tile id.", "datatype": "U32"},
                                {"partId": "roadId", "description": "Synthetic road id.", "datatype": "U64"}
                            ]]
                        }
                    ]
                }
            }
        })"_json))
    {
    }

    DataSourceInfo info() override { return info_; }

    void fill(TileFeatureLayer::Ptr const& tile) override
    {
        {
            std::lock_guard lock(mutex_);
            requestedStages_.push_back(tile->stage().value_or(UnspecifiedStage));
        }

        auto feature = tile->newFeature(
            "Road",
            {{"tileId", static_cast<int64_t>(tile->tileId().value_)}, {"roadId", int64_t(42)}});
        if (tile->stage().value_or(0U) == 0U) {
            feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});
            return;
        }

        auto attr = feature->attributeLayers()->newLayer("details")->newAttribute("speedLimit");
        attr->addField("limit", tile->newValue(int64_t(80)));
    }

    void fill(TileSourceDataLayer::Ptr const&) override
    {
        throw std::runtime_error("Source data is not used by this test datasource.");
    }

    std::vector<uint32_t> requestedStages() const
    {
        std::lock_guard lock(mutex_);
        return requestedStages_;
    }

private:
    DataSourceInfo info_;
    mutable std::mutex mutex_;
    std::vector<uint32_t> requestedStages_;
};

} // namespace

TEST_CASE("TileSearchResultLayer stores fixed result values and shared geometry", "[search-result-layer]")
{
    auto layer = makeSearchResultLayer();
    layer->setResultFields({"displayName", "speedLimitKmh"});

    auto featureId = layer->newFeatureId("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    auto geometry = layer->newGeometryCollection();
    auto line = geometry->newGeometry(GeomType::Line);
    line->append(Point(11.0, 48.0, 0.0));
    line->append(Point(11.1, 48.1, 0.0));

    std::vector<simfil::ModelNode::Ptr> values{
        layer->newValue("Main Street"),
        layer->newValue(int64_t(50)),
    };
    auto result = layer->newSearchResult(featureId, geometry, values, 3U);

    REQUIRE(layer->size() == 1);
    REQUIRE(result->featureId()->toString() == "Road.7.42");
    REQUIRE(result->attributeIndex() == 3U);
    REQUIRE(result->values()->size() == 2);
    REQUIRE(result->geometry()->numGeometries() == 1);

    auto json = layer->toJson();
    REQUIRE(json["type"] == "SearchResultCollection");
    REQUIRE(json["resultFields"] == nlohmann::json::array({"displayName", "speedLimitKmh"}));
    REQUIRE(json["results"].size() == 1);
    REQUIRE(json["results"][0]["featureId"] == "Road.7.42");
    REQUIRE(json["results"][0]["attributeIndex"] == 3);
    REQUIRE(json["results"][0]["values"] == nlohmann::json::array({"Main Street", 50}));
}

TEST_CASE("TileSearchResultLayer roundtrips through TileLayerStream", "[search-result-layer][stream]")
{
    auto layer = makeSearchResultLayer();
    layer->setStage(3);
    layer->setResultFields({"label"});

    auto featureId = layer->newFeatureId("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    auto geometry = layer->newGeometryCollection();
    geometry->newGeometry(GeomType::Points)->append(Point(11.0, 48.0, 0.0));
    std::vector<simfil::ModelNode::Ptr> values{layer->newValue("Result Label")};
    layer->newSearchResult(featureId, geometry, values);

    std::string streamBytes;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer(
        [&](std::string bytes, TileLayerStream::MessageType) { streamBytes.append(bytes); },
        offsets);
    writer.write(layer);

    TileSearchResultLayer::Ptr parsed;
    TileLayerStream::Reader reader(
        [&](std::string_view const&, std::string_view const&) { return layer->layerInfo(); },
        [&](TileLayer::Ptr parsedLayer) { parsed = std::dynamic_pointer_cast<TileSearchResultLayer>(parsedLayer); });
    reader.read(streamBytes);

    REQUIRE(parsed);
    REQUIRE(parsed->size() == 1);
    REQUIRE(parsed->stage() == std::optional<uint32_t>(3));
    REQUIRE(parsed->resultFields() == std::vector<std::string>{"label"});
    auto parsedResult = parsed->at(0);
    REQUIRE(parsedResult);
    REQUIRE(parsedResult->featureId()->toString() == "Road.7.42");
    REQUIRE(parsedResult->values()->size() == 1);
    REQUIRE(parsed->toJson()["results"][0]["values"] == nlohmann::json::array({"Result Label"}));
}

TEST_CASE("Feature-layer search produces TileSearchResultLayer", "[feature-layer-search]")
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("SearchSourceNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "SearchSourceNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});
    source->setStage(2);
    auto const sourceStringHighWatermark = strings->highest();

    auto searchResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "unit-search",
            .query_ = "typeId == 'Road'",
            .scope_ = FeatureLayerSearchScope::Feature,
            .withFields_ = {"'display label'", "typeId", "searchOnlyMissingField", "1 +"},
        });

    REQUIRE(searchResult.has_value());
    REQUIRE(searchResult->layer_);
    REQUIRE(searchResult->layer_->size() == 1);
    auto result = searchResult->layer_->at(0);
    REQUIRE(result->featureId()->toString() == "Road.7.42");
    REQUIRE(result->geometry()->numGeometries() == 1);
    REQUIRE(searchResult->layer_->stage() == std::optional<uint32_t>(2));
    REQUIRE(source->strings()->highest() == sourceStringHighWatermark);
    REQUIRE(searchResult->layer_->resultFields() == std::vector<std::string>{"'display label'", "typeId", "searchOnlyMissingField", "1 +"});
    REQUIRE(searchResult->layer_->toJson()["results"][0]["values"] == nlohmann::json::array({"display label", "Road", nullptr, nullptr}));
}

TEST_CASE("Attribute-scope search records deterministic match metadata", "[feature-layer-search]")
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("AttributeSearchSourceNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "AttributeSearchSourceNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});
    auto attr = feature->attributeLayers()->newLayer("rules")->newAttribute("speedLimit");
    attr->addField("limit", source->newValue(int64_t(50)));

    auto searchResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "attribute-search",
            .query_ = "$name == 'speedLimit'",
            .scope_ = FeatureLayerSearchScope::Attribute,
            .withFields_ = {"limit", "$feature.typeId", "$layer", "$validityIndex", "$validityCount"},
        });

    REQUIRE(searchResult.has_value());
    REQUIRE(searchResult->layer_->size() == 1);
    auto result = searchResult->layer_->at(0);
    REQUIRE(result->attributeIndex() == 0U);
    REQUIRE(result->validityIndex() == 0U);
    REQUIRE(result->validityCount() == 1U);
    auto json = result->toJson();
    REQUIRE(json["match"]["attributeIndex"] == 0);
    REQUIRE(json["match"]["validityIndex"] == 0);
    REQUIRE(json["match"]["validityCount"] == 1);
    REQUIRE(json["values"] == nlohmann::json::array({50, "Road", "rules", 0, 1}));
}

TEST_CASE("Service search loads staged payloads and evaluates in scheduled search jobs", "[feature-layer-search][Service]")
{
    auto cache = std::make_shared<MemCache>(32);
    Service service(cache, false);
    auto dataSource = std::make_shared<StagedSearchDataSource>();
    service.add(dataSource);

    auto request = std::make_shared<FeatureLayerSearchTilesRequest>(
        "TestMap",
        "SearchableLayer",
        std::vector<TileId>{TileId(0x1234)},
        FeatureLayerSearchRequest{
            .searchId_ = "service-search",
            .requestKey_ = "service-search:1",
            .query_ = "$name == 'speedLimit'",
            .scope_ = FeatureLayerSearchScope::Attribute,
            .withFields_ = {"limit", "$feature.typeId", "$layer"},
            .refresh_ = 1,
        });

    std::vector<TileSearchResultLayer::Ptr> results;
    std::vector<nlohmann::json> statuses;
    request->onSearchResult([&](TileSearchResultLayer::Ptr layer) {
        results.push_back(std::move(layer));
    });
    request->onStatus([&](nlohmann::json const& status) {
        statuses.push_back(status);
    });

    REQUIRE(service.request(request));
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    REQUIRE(results.size() == 1);
    REQUIRE(results.front()->nodeId() == "SearchStageNode");
    REQUIRE(results.front()->size() == 1);
    REQUIRE(results.front()->info()["sourceStageMask"] == nlohmann::json::array({0, 1}));
    REQUIRE(results.front()->info()["searchRequestKey"] == "service-search:1");
    REQUIRE(results.front()->info()["resultCount"] == 1);
    REQUIRE(results.front()->toJson()["results"][0]["values"] == nlohmann::json::array({80, "Road", "details"}));

    auto requestedStages = dataSource->requestedStages();
    std::sort(requestedStages.begin(), requestedStages.end());
    REQUIRE(requestedStages.size() == 2);
    REQUIRE(requestedStages[0] == 0U);
    REQUIRE(requestedStages[1] == 1U);
    REQUIRE_FALSE(statuses.empty());
}

TEST_CASE("Tile request parser carries inherited search fields", "[feature-layer-search][tiles-request]")
{
    nlohmann::json envelope = {
        {"searchId", "query-42"},
        {"refresh", 7},
        {"searchQuery", "typeId == 'Road'"},
        {"searchScope", "attribute"},
        {"withFields", {"$feature.typeId", "$name"}},
    };
    nlohmann::json request = {
        {"mapId", "TestMap"},
        {"layerId", "RoadLayer"},
        {"tileIds", {1, 2}},
    };

    detail::inheritSearchFields(request, envelope);
    auto parsed = detail::parseLayerTilesRequestJson(request);

    REQUIRE(parsed.searchRequest.has_value());
    REQUIRE(parsed.searchRequest->searchId_ == "query-42");
    REQUIRE(parsed.searchRequest->refresh_ == 7);
    REQUIRE(parsed.searchRequest->scope_ == FeatureLayerSearchScope::Attribute);
    REQUIRE(parsed.searchRequest->withFields_ == std::vector<std::string>{"$feature.typeId", "$name"});
    REQUIRE_FALSE(parsed.searchRequest->requestKey_.empty());
}
