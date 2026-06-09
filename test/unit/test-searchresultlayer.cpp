#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include "mapget/model/featurelayer-search.h"
#include "mapget/model/searchresultlayer.h"
#include "mapget/model/stream.h"
#include "mapget/service/memcache.h"
#include "mapget/service/service.h"
#include "simfil/simfil.h"
#include "../../libs/http-service/src/tiles-request-json.h"

using namespace mapget;

namespace
{

class SearchResultTestDataSourceError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

DataSourceInfo makeStagedSearchDataSourceInfo()
{
    return DataSourceInfo::fromJson(R"({
        "nodeId": "SearchStageNode",
        "mapId": "TestMap",
        "maxParallelJobs": 1,
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
    })"_json);
}

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

std::shared_ptr<LayerInfo> makeSchemaBackedSearchResultLayerInfo()
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
        ],
        "featureModelSchema": {
            "$schema": "http://json-schema.org/draft-07/schema#",
            "oneOf": [
                {"$ref": "#/$defs/RoadFeature"}
            ],
            "$defs": {
                "RoadFeature": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Feature",
                        "featureType": "Road"
                    },
                    "properties": {
                        "typeId": {"const": "Road"},
                        "properties": {"$ref": "#/$defs/RoadProperties"}
                    }
                },
                "RoadProperties": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "FeatureProperties",
                        "featureType": "Road"
                    },
                    "properties": {
                        "layer": {"$ref": "#/$defs/RoadLayerMap"}
                    }
                },
                "RoadLayerMap": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "AttributeLayerMap",
                        "featureType": "Road"
                    },
                    "properties": {
                        "rules": {"$ref": "#/$defs/RulesLayer"}
                    }
                },
                "RulesLayer": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "AttributeContainer"
                    },
                    "properties": {
                        "speedLimit": {"$ref": "#/$defs/SpeedLimitAttribute"},
                        "warningSign": {"$ref": "#/$defs/WarningSignAttribute"}
                    }
                },
                "SpeedLimitAttribute": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Attribute",
                        "attributeTypeCode": "speedLimit",
                        "attributeType": "synthetic.SpeedLimitAttribute"
                    },
                    "properties": {
                        "limit": {"type": "number"}
                    }
                },
                "WarningSignAttribute": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Attribute",
                        "attributeTypeCode": "warningSign",
                        "attributeType": "synthetic.WarningSignAttribute"
                    },
                    "properties": {
                        "kind": {"type": "string", "enum": ["SPEED_LIMIT", "SPEED_LIMIT_END"]}
                    }
                }
            }
        }
    })"_json);
}

std::shared_ptr<LayerInfo> makeLiveLikeSchemaBackedSearchResultLayerInfo()
{
    return LayerInfo::fromJson(R"({
        "layerId": "Road",
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
        ],
        "featureModelSchema": {
            "$schema": "http://json-schema.org/draft-07/schema#",
            "oneOf": [
                {"$ref": "#/$defs/RoadFeature"}
            ],
            "$defs": {
                "RoadFeature": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Feature",
                        "featureType": "Road"
                    },
                    "properties": {
                        "typeId": {"const": "Road"},
                        "properties": {"$ref": "#/$defs/RoadProperties"}
                    }
                },
                "RoadProperties": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "FeatureProperties",
                        "featureType": "Road"
                    },
                    "properties": {
                        "layer": {"$ref": "#/$defs/RoadLayerMap"}
                    }
                },
                "RoadLayerMap": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "AttributeLayerMap",
                        "featureType": "Road"
                    },
                    "properties": {
                        "RoadRulesLayer": {"$ref": "#/$defs/RoadRulesLayer"}
                    }
                },
                "RoadRulesLayer": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "AttributeContainer"
                    },
                    "properties": {
                        "SPEED_LIMIT_METRIC": {"$ref": "#/$defs/SpeedLimitMetricAttribute"},
                        "WARNING_SIGN": {"$ref": "#/$defs/WarningSignAttribute"},
                        "MOVABLE_WARNING_SIGN": {"$ref": "#/$defs/MovableWarningSignAttribute"}
                    }
                },
                "SpeedLimitMetricAttribute": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Attribute",
                        "attributeTypeCode": "SPEED_LIMIT_METRIC",
                        "attributeType": "nds.rules.SpeedLimitMetric"
                    },
                    "properties": {
                        "attributeValue": {
                            "type": "object",
                            "properties": {
                                "speedLimitKmh": {"type": "number"}
                            }
                        }
                    }
                },
                "WarningSignAttribute": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Attribute",
                        "attributeTypeCode": "WARNING_SIGN",
                        "attributeType": "nds.rules.WarningSign"
                    },
                    "properties": {
                        "attributeValue": {
                            "type": "object",
                            "properties": {
                                "warningSign": {"type": "string", "enum": ["SPEED_LIMIT", "SPEED_LIMIT_END"]}
                            }
                        }
                    }
                },
                "MovableWarningSignAttribute": {
                    "type": "object",
                    "x-mapget": {
                        "metaType": "Attribute",
                        "attributeTypeCode": "MOVABLE_WARNING_SIGN",
                        "attributeType": "nds.rules.MovableWarningSign"
                    },
                    "properties": {
                        "attributeValue": {
                            "type": "object",
                            "properties": {
                                "movableWarningSign": {"type": "string", "enum": ["SPEED_LIMIT", "SPEED_LIMIT_END"]}
                            }
                        }
                    }
                }
            }
        }
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
        throw SearchResultTestDataSourceError("Source data is not used by this test datasource.");
    }

    std::vector<uint32_t> requestedStages() const
    {
        std::lock_guard lock(mutex_);
        return requestedStages_;
    }

private:
    DataSourceInfo info_ = makeStagedSearchDataSourceInfo();
    mutable std::mutex mutex_;
    std::vector<uint32_t> requestedStages_;
};

bool containsDiagnosticMessage(simfil::Diagnostics const& diagnostics, std::string_view needle)
{
    auto messages = simfil::diagnostics(diagnostics);
    REQUIRE(messages.has_value());
    return std::any_of(messages->begin(), messages->end(), [needle](auto const& message) {
        return message.message.find(needle) != std::string::npos;
    });
}

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

TEST_CASE("TileSearchResultLayer materializes non-scalar result values as placeholders", "[search-result-layer]")
{
    auto layer = makeSearchResultLayer();
    layer->setResultFields({"blobValue", "objectValue", "listValue"});

    auto featureId = layer->newFeatureId("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    auto geometry = layer->newGeometryCollection();
    geometry->newGeometry(GeomType::Points)->append(Point(11.0, 48.0, 0.0));

    auto listValue = layer->newArray(1, true);
    listValue->append(layer->newValue(int64_t(1)));
    std::vector<simfil::ModelNode::Ptr> values{
        layer->materializeValue(simfil::Value::make(simfil::ByteArray{"AB"})),
        layer->materializeValue(simfil::Value{
            simfil::ValueType::Object,
            simfil::model_ptr<simfil::ModelNode>(featureId)}),
        layer->materializeValue(simfil::Value{
            simfil::ValueType::Array,
            simfil::model_ptr<simfil::ModelNode>(listValue)}),
    };
    layer->newSearchResult(featureId, geometry, values);

    REQUIRE(layer->toJson()["results"][0]["values"] == nlohmann::json::array({"blob", "object", "list"}));
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
    simfil::Trace trace;
    trace.calls = 2;
    trace.totalus = std::chrono::microseconds{17};
    trace.values.push_back(simfil::Value::make(std::string("trace-value")));
    trace.values.push_back(simfil::Value::make(int64_t(5)));
    std::map<std::string, simfil::Trace> traces;
    traces.try_emplace("debug-label", std::move(trace));
    layer->setTraces(std::move(traces));

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
    REQUIRE(parsed->traceCount() == 1);
    auto parsedTrace = parsed->traceAt(0);
    REQUIRE(parsedTrace);
    REQUIRE(parsedTrace->name() == "debug-label");
    REQUIRE(parsedTrace->calls() == 2);
    REQUIRE(parsedTrace->totalUs().count() == 17);
    REQUIRE(parsedTrace->values()->size() == 2);
    REQUIRE(parsedTrace->toJson()["values"] == nlohmann::json::array({"trace-value", 5}));
    REQUIRE(parsed->toJson()["traces"].size() == 1);
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
            .query_ = "trace(typeId == 'Road', -1, 'match')",
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
    REQUIRE_FALSE(searchResult->layer_->info().contains("traces"));
    REQUIRE(searchResult->layer_->traceCount() == 1);
    auto trace = searchResult->layer_->traceAt(0);
    REQUIRE(trace);
    REQUIRE(trace->name() == "match");
    REQUIRE(trace->calls() == 1);
    REQUIRE(trace->values()->size() == 1);
    REQUIRE(trace->values()->at(0)->toJson() == true);
    REQUIRE(searchResult->layer_->toJson()["traces"][0]["name"] == "match");
}

TEST_CASE("Feature-layer search stores diagnostics on the result layer", "[feature-layer-search][search-result-layer]")
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("SearchDiagnosticsNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "SearchDiagnosticsNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});

    auto searchResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "diagnostics-search",
            .query_ = "**.not_a_field > 0",
            .scope_ = FeatureLayerSearchScope::Feature,
        });

    REQUIRE(searchResult.has_value());
    REQUIRE(searchResult->layer_);
    REQUIRE(searchResult->layer_->size() == 0);
    REQUIRE(containsDiagnosticMessage(searchResult->layer_->diagnostics(), "No matches for field"));
    REQUIRE_FALSE(searchResult->layer_->toJson()["diagnostics"].empty());

    std::string streamBytes;
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer(
        [&](std::string bytes, TileLayerStream::MessageType) { streamBytes.append(bytes); },
        offsets);
    writer.write(searchResult->layer_);

    TileSearchResultLayer::Ptr parsed;
    TileLayerStream::Reader reader(
        [&](std::string_view const&, std::string_view const&) { return layerInfo; },
        [&](TileLayer::Ptr parsedLayer) { parsed = std::dynamic_pointer_cast<TileSearchResultLayer>(parsedLayer); });
    reader.read(streamBytes);

    REQUIRE(parsed);
    REQUIRE(containsDiagnosticMessage(parsed->diagnostics(), "No matches for field"));
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

TEST_CASE("Attribute-scope search uses schema scalar shorthand", "[feature-layer-search]")
{
    auto layerInfo = makeSchemaBackedSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("AttributeShorthandSearchSourceNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "AttributeShorthandSearchSourceNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});
    auto attr = feature->attributeLayers()->newLayer("rules")->newAttribute("speedLimit");
    attr->addField("limit", source->newValue(int64_t(50)));

    auto comparisonResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "attribute-shorthand-search",
            .query_ = "speedLimit > 40",
            .scope_ = FeatureLayerSearchScope::Attribute,
            .withFields_ = {"limit"},
        });

    REQUIRE(comparisonResult.has_value());
    REQUIRE(comparisonResult->layer_->size() == 1);
    REQUIRE(comparisonResult->layer_->at(0)->toJson()["values"] == nlohmann::json::array({50}));

    auto standaloneResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "attribute-type-code-search",
            .query_ = "speedLimit",
            .scope_ = FeatureLayerSearchScope::Attribute,
        });

    REQUIRE(standaloneResult.has_value());
    REQUIRE(standaloneResult->layer_->size() == 1);
}

TEST_CASE("Search query normalization rewrites feature-root attribute paths", "[feature-layer-search]")
{
    auto layerInfo = makeSchemaBackedSearchResultLayerInfo();
    auto registry = layerInfo->schemaRegistry();
    REQUIRE(registry);

    auto normalized = registry->normalizeSearchQuery(
        "properties.layer.rules.speedLimit.limit > 40",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(normalized.has_value());
    INFO(normalized->normalizedQuery_);
    REQUIRE(normalized->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(normalized->attributeScopes_.size() == 1);
    REQUIRE(normalized->attributeScopes_.front().featureType_ == "Road");
    REQUIRE(normalized->attributeScopes_.front().attributeLayerName_ == "rules");
    REQUIRE(normalized->attributeScopes_.front().attributeName_ == "speedLimit");
    REQUIRE(normalized->normalizedQuery_.find("$feature.typeId == \"Road\"") != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find("$layer == \"rules\"") != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find("$name == \"speedLimit\"") != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find("limit > 40") != std::string::npos);
    REQUIRE(normalized->normalizedQuery_.find("properties.layer.rules.speedLimit") == std::string::npos);

    auto strings = std::make_shared<StringPool>("NormalizedAttributePathSearchNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "NormalizedAttributePathSearchNode",
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
            .searchId_ = "normalized-attribute-path-search",
            .query_ = "properties.layer.rules.speedLimit.limit > 40",
            .scope_ = FeatureLayerSearchScope::Auto,
            .rewriteQuery_ = true,
            .withFields_ = {"limit"},
        });

    REQUIRE(searchResult.has_value());
    REQUIRE(searchResult->layer_->size() == 1);
    REQUIRE(searchResult->layer_->at(0)->toJson()["values"] == nlohmann::json::array({50}));
    REQUIRE(searchResult->layer_->info()["searchScope"] == "attribute");
    REQUIRE(searchResult->layer_->info()["normalizedSearchQuery"].get<std::string>().find("limit > 40") != std::string::npos);
}

TEST_CASE("Search query normalization uses AST-derived attribute shorthands", "[feature-layer-search]")
{
    auto layerInfo = makeSchemaBackedSearchResultLayerInfo();
    auto registry = layerInfo->schemaRegistry();
    REQUIRE(registry);

    auto typeCode = registry->normalizeSearchQuery(
        "speedLimit",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(typeCode.has_value());
    INFO(typeCode->normalizedQuery_);
    REQUIRE(typeCode->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(typeCode->attributeScopes_.size() == 1);
    REQUIRE(typeCode->normalizedQuery_.find("$feature.typeId == \"Road\"") != std::string::npos);
    REQUIRE(typeCode->normalizedQuery_.find("$layer == \"rules\"") != std::string::npos);
    REQUIRE(typeCode->normalizedQuery_.find("$name == \"speedLimit\"") != std::string::npos);
    REQUIRE(typeCode->normalizedQuery_ == "$feature.typeId == \"Road\" and $layer == \"rules\" and $name == \"speedLimit\"");

    auto scalarOperand = registry->normalizeSearchQuery(
        "speedLimit > 40",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(scalarOperand.has_value());
    INFO(scalarOperand->normalizedQuery_);
    REQUIRE(scalarOperand->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(scalarOperand->normalizedQuery_.find("limit > 40") != std::string::npos);
    REQUIRE(scalarOperand->normalizedQuery_.find("speedLimit > 40") == std::string::npos);

    auto enumConstant = registry->normalizeSearchQuery(
        "\"SPEED_LIMIT\"",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(enumConstant.has_value());
    INFO(enumConstant->normalizedQuery_);
    REQUIRE(enumConstant->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(enumConstant->attributeScopes_.size() == 1);
    REQUIRE(enumConstant->attributeScopes_.front().attributeName_ == "warningSign");
    REQUIRE(enumConstant->normalizedQuery_.find("kind == \"SPEED_LIMIT\"") != std::string::npos);

    auto unquotedEnumConstant = registry->normalizeSearchQuery(
        "SPEED_LIMIT_END",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(unquotedEnumConstant.has_value());
    INFO(unquotedEnumConstant->normalizedQuery_);
    REQUIRE(unquotedEnumConstant->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(unquotedEnumConstant->attributeScopes_.size() == 1);
    REQUIRE(unquotedEnumConstant->attributeScopes_.front().attributeName_ == "warningSign");
    REQUIRE(unquotedEnumConstant->normalizedQuery_.find("kind == \"SPEED_LIMIT_END\"") != std::string::npos);

    auto strings = std::make_shared<StringPool>("NormalizedAttributeShorthandSearchNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "NormalizedAttributeShorthandSearchNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});
    auto attr = feature->attributeLayers()->newLayer("rules")->newAttribute("speedLimit");
    attr->addField("limit", source->newValue(int64_t(50)));
    auto warningSign = feature->attributeLayers()->newLayer("rules")->newAttribute("warningSign");
    warningSign->addField("kind", source->newValue("SPEED_LIMIT"));

    auto enumResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "normalized-enum-constant-search",
            .query_ = "\"SPEED_LIMIT\"",
            .scope_ = FeatureLayerSearchScope::Auto,
            .rewriteQuery_ = true,
            .withFields_ = {"kind"},
        });

    REQUIRE(enumResult.has_value());
    REQUIRE(enumResult->layer_->size() == 1);
    REQUIRE(enumResult->layer_->at(0)->toJson()["values"] == nlohmann::json::array({"SPEED_LIMIT"}));
    REQUIRE(enumResult->layer_->info()["searchScope"] == "attribute");
    REQUIRE(enumResult->layer_->info()["normalizedSearchQuery"].get<std::string>().find("kind == \"SPEED_LIMIT\"") != std::string::npos);
}

TEST_CASE("Search query normalization handles live-style attribute and enum expressions", "[feature-layer-search]")
{
    auto layerInfo = makeLiveLikeSchemaBackedSearchResultLayerInfo();
    auto registry = layerInfo->schemaRegistry();
    REQUIRE(registry);

    auto speedComparison = registry->normalizeSearchQuery(
        "SPEED_LIMIT_METRIC == 80",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(speedComparison.has_value());
    INFO(speedComparison->normalizedQuery_);
    REQUIRE(speedComparison->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(speedComparison->attributeScopes_.size() == 1);
    REQUIRE(speedComparison->attributeScopes_.front().attributeName_ == "SPEED_LIMIT_METRIC");
    REQUIRE(speedComparison->normalizedQuery_.find("$name == \"SPEED_LIMIT_METRIC\"") != std::string::npos);
    REQUIRE(speedComparison->normalizedQuery_.find("attributeValue.speedLimitKmh == 80") != std::string::npos);

    auto speedWildcard = registry->normalizeSearchQuery(
        "**.speedLimitKmh > 80",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(speedWildcard.has_value());
    INFO(speedWildcard->normalizedQuery_);
    REQUIRE(speedWildcard->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(speedWildcard->attributeScopes_.size() == 1);
    REQUIRE(speedWildcard->attributeScopes_.front().attributeName_ == "SPEED_LIMIT_METRIC");
    REQUIRE(speedWildcard->normalizedQuery_.find("$name == \"SPEED_LIMIT_METRIC\"") != std::string::npos);
    REQUIRE(speedWildcard->normalizedQuery_.find("**.attributeValue.speedLimitKmh > 80") != std::string::npos);

    auto enumSymbol = registry->normalizeSearchQuery(
        "SPEED_LIMIT_END",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(enumSymbol.has_value());
    INFO(enumSymbol->normalizedQuery_);
    REQUIRE(enumSymbol->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(enumSymbol->attributeScopes_.size() == 2);
    REQUIRE(enumSymbol->normalizedQuery_.find("attributeValue.warningSign == \"SPEED_LIMIT_END\"") != std::string::npos);
    REQUIRE(enumSymbol->normalizedQuery_.find("attributeValue.movableWarningSign == \"SPEED_LIMIT_END\"") != std::string::npos);

    auto explicitWarningPath = registry->normalizeSearchQuery(
        "**.WARNING_SIGN.attributeValue.warningSign == \"SPEED_LIMIT\"",
        SchemaRegistry::SearchQueryRequestedScope::Auto);
    REQUIRE(explicitWarningPath.has_value());
    INFO(explicitWarningPath->normalizedQuery_);
    REQUIRE(explicitWarningPath->concreteScope_ == SchemaRegistry::SearchQueryConcreteScope::Attribute);
    REQUIRE(explicitWarningPath->attributeScopes_.size() == 1);
    REQUIRE(explicitWarningPath->attributeScopes_.front().attributeName_ == "WARNING_SIGN");
    REQUIRE(explicitWarningPath->normalizedQuery_.find("$name == \"WARNING_SIGN\"") != std::string::npos);
    REQUIRE(explicitWarningPath->normalizedQuery_.find("attributeValue.warningSign == \"SPEED_LIMIT\"") != std::string::npos);
    REQUIRE(explicitWarningPath->normalizedQuery_.find("**.WARNING_SIGN") == std::string::npos);

    auto strings = std::make_shared<StringPool>("LiveLikeNormalizedSearchNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "LiveLikeNormalizedSearchNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(11.0, 48.0, 0.0), Point(11.1, 48.1, 0.0)});
    auto rules = feature->attributeLayers()->newLayer("RoadRulesLayer");
    auto speedLimitValue = source->newObject();
    REQUIRE(speedLimitValue->addField("speedLimitKmh", int64_t(80)).has_value());
    REQUIRE(rules->newAttribute("SPEED_LIMIT_METRIC")
                ->addField("attributeValue", speedLimitValue)
                .has_value());
    auto warningSignValue = source->newObject();
    REQUIRE(warningSignValue->addField("warningSign", "SPEED_LIMIT").has_value());
    REQUIRE(rules->newAttribute("WARNING_SIGN")
                ->addField("attributeValue", warningSignValue)
                .has_value());
    auto movableWarningSignValue = source->newObject();
    REQUIRE(movableWarningSignValue->addField("movableWarningSign", "SPEED_LIMIT_END").has_value());
    REQUIRE(rules->newAttribute("MOVABLE_WARNING_SIGN")
                ->addField("attributeValue", movableWarningSignValue)
                .has_value());

    auto runSearch = [&](std::string query, std::vector<std::string> withFields) {
        return searchFeatureLayerAsResultLayer(
            *source,
            FeatureLayerSearchRequest{
                .searchId_ = query,
                .query_ = std::move(query),
                .scope_ = FeatureLayerSearchScope::Auto,
                .rewriteQuery_ = true,
                .withFields_ = std::move(withFields),
            });
    };

    auto speedComparisonResult = runSearch("SPEED_LIMIT_METRIC == 80", {"attributeValue.speedLimitKmh"});
    REQUIRE(speedComparisonResult.has_value());
    REQUIRE(speedComparisonResult->layer_->info()["searchScope"] == "attribute");
    REQUIRE(speedComparisonResult->layer_->size() == 1);
    REQUIRE(speedComparisonResult->layer_->at(0)->toJson()["values"] == nlohmann::json::array({80}));

    auto speedWildcardResult = runSearch("**.speedLimitKmh > 80", {"attributeValue.speedLimitKmh"});
    REQUIRE(speedWildcardResult.has_value());
    REQUIRE(speedWildcardResult->layer_->info()["searchScope"] == "attribute");
    REQUIRE(speedWildcardResult->layer_->size() == 0);

    auto enumSymbolResult = runSearch("SPEED_LIMIT_END", {"attributeValue.movableWarningSign"});
    REQUIRE(enumSymbolResult.has_value());
    REQUIRE(enumSymbolResult->layer_->info()["searchScope"] == "attribute");
    REQUIRE(enumSymbolResult->layer_->size() == 1);
    REQUIRE(enumSymbolResult->layer_->at(0)->toJson()["values"] == nlohmann::json::array({"SPEED_LIMIT_END"}));

    auto explicitWarningResult = runSearch(
        "**.WARNING_SIGN.attributeValue.warningSign == \"SPEED_LIMIT\"",
        {"attributeValue.warningSign"});
    REQUIRE(explicitWarningResult.has_value());
    REQUIRE(explicitWarningResult->layer_->info()["searchScope"] == "attribute");
    REQUIRE(explicitWarningResult->layer_->size() == 1);
    REQUIRE(explicitWarningResult->layer_->at(0)->toJson()["values"] == nlohmann::json::array({"SPEED_LIMIT"}));
}

TEST_CASE("Attribute-scope search copies computed validity geometry", "[feature-layer-search]")
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("ValidityGeometrySearchNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "ValidityGeometrySearchNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(0.0, 0.0, 0.0), Point(1.0, 0.0, 0.0), Point(2.0, 0.0, 0.0)});
    auto attr = feature->attributeLayers()->newLayer("rules")->newAttribute("speedLimit");
    attr->addField("limit", source->newValue(int64_t(50)));
    attr->validity()->newRange(Validity::BufferOffset, int32_t(1), int32_t(2));

    auto searchResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "validity-geometry-search",
            .query_ = "$name == 'speedLimit'",
            .scope_ = FeatureLayerSearchScope::Attribute,
        });

    REQUIRE(searchResult.has_value());
    REQUIRE(searchResult->layer_->size() == 1);

    auto result = searchResult->layer_->at(0);
    REQUIRE(result);
    auto geometry = result->geometry()->geometryOfTypeAtPreferredStage(GeomType::Line);
    REQUIRE(geometry);
    REQUIRE(geometry->numPoints() == 2);
    REQUIRE(std::abs(geometry->pointAt(0).x - 1.0) < 1e-4);
    REQUIRE(std::abs(geometry->pointAt(1).x - 2.0) < 1e-4);
}

TEST_CASE("Attribute-scope search preserves offset point validity geometry type", "[feature-layer-search]")
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("PointValidityGeometrySearchNode");
    auto source = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        "PointValidityGeometrySearchNode",
        "TestMap",
        layerInfo,
        strings);
    auto feature = source->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    feature->addLine({Point(0.0, 0.0, 0.0), Point(1.0, 0.0, 0.0), Point(2.0, 0.0, 0.0)});
    auto attr = feature->attributeLayers()->newLayer("rules")->newAttribute("warningSign");
    attr->validity()->newPoint(Validity::BufferOffset, int32_t(1));

    auto searchResult = searchFeatureLayerAsResultLayer(
        *source,
        FeatureLayerSearchRequest{
            .searchId_ = "point-validity-geometry-search",
            .query_ = "$name == 'warningSign'",
            .scope_ = FeatureLayerSearchScope::Attribute,
        });

    REQUIRE(searchResult.has_value());
    REQUIRE(searchResult->layer_->size() == 1);

    auto result = searchResult->layer_->at(0);
    REQUIRE(result);
    auto geometry = result->geometry()->geometryOfTypeAtPreferredStage(GeomType::Points);
    REQUIRE(geometry);
    REQUIRE(geometry->numPoints() == 1);
    REQUIRE(std::abs(geometry->pointAt(0).x - 1.0) < 1e-4);
    REQUIRE_FALSE(result->geometry()->geometryOfTypeAtPreferredStage(GeomType::Line));
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

TEST_CASE("Service search requests staged source tiles in complete-tile order", "[feature-layer-search][Service]")
{
    auto cache = std::make_shared<MemCache>(32);
    Service service(cache, false);
    auto dataSource = std::make_shared<StagedSearchDataSource>();
    service.add(dataSource);

    auto request = std::make_shared<FeatureLayerSearchTilesRequest>(
        "TestMap",
        "SearchableLayer",
        std::vector<TileId>{TileId(0x1234), TileId(0x1235)},
        FeatureLayerSearchRequest{
            .searchId_ = "service-search-stage-order",
            .query_ = "$name == 'speedLimit'",
            .scope_ = FeatureLayerSearchScope::Attribute,
        });

    request->onSearchResult([](TileSearchResultLayer::Ptr) {
        // Intentionally ignored; this test only asserts staged datasource request ordering.
    });

    REQUIRE(service.request(request));
    request->wait();

    REQUIRE(request->getStatus() == RequestStatus::Success);
    // Large staged searches must not load stage 0 for every tile before any
    // higher stage. Complete-tile ordering bounds the number of partial staged
    // tiles retained by the search assembler.
    REQUIRE(dataSource->requestedStages() == std::vector<uint32_t>{0U, 1U, 0U, 1U});
}

TEST_CASE("Repeated staged search assembly does not duplicate overlay matches", "[feature-layer-search]")
{
    auto layerInfo = makeSearchResultLayerInfo();
    auto strings = std::make_shared<StringPool>("RepeatedSearchStageNode");
    auto base = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        strings->nodeId_,
        "TestMap",
        layerInfo,
        strings);
    base->setStage(0);
    auto baseFeature = base->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    baseFeature->addLine({Point(0.0, 0.0, 0.0), Point(1.0, 0.0, 0.0), Point(2.0, 0.0, 0.0)});

    auto overlay = std::make_shared<TileFeatureLayer>(
        TileId(0x1234),
        strings->nodeId_,
        "TestMap",
        layerInfo,
        strings);
    overlay->setStage(1);
    auto overlayFeature = overlay->newFeature("Road", {{"tileId", int64_t(7)}, {"roadId", int64_t(42)}});
    auto attr = overlayFeature->attributeLayers()->newLayer("details")->newAttribute("speedLimit");
    attr->addField("limit", overlay->newValue(int64_t(80)));
    // This validity is owned by the overlay stage; search must not reinterpret
    // its model address against the assembled base stage.
    attr->validity()->newPoint(Validity::BufferOffset, int32_t(1));
    auto const sourceStringHighWatermark = strings->highest();

    std::vector<TileFeatureLayer::Ptr> stages{base, overlay};
    auto request = FeatureLayerSearchRequest{
        .searchId_ = "repeat-assembly-search",
        .query_ = "$name == 'speedLimit'",
        .scope_ = FeatureLayerSearchScope::Attribute,
        .withFields_ = {"limit"},
        .sourceStageMask_ = {0, 1},
    };

    auto firstAssembly = assembleFeatureLayerStages(stages);
    REQUIRE(firstAssembly.has_value());
    REQUIRE(*firstAssembly == base);
    auto firstResult = searchFeatureLayerAsResultLayer(**firstAssembly, request);
    REQUIRE(firstResult.has_value());
    REQUIRE(firstResult->layer_->stage() == std::nullopt);
    REQUIRE(firstResult->layer_->size() == 1);
    REQUIRE(firstResult->layer_->toJson()["results"][0]["values"] == nlohmann::json::array({80}));
    auto firstGeometry = firstResult->layer_->at(0)->geometry()->geometryOfTypeAtPreferredStage(GeomType::Points);
    REQUIRE(firstGeometry);
    REQUIRE(firstGeometry->numPoints() == 1);
    REQUIRE(std::abs(firstGeometry->pointAt(0).x - 1.0) < 1e-4);

    auto secondAssembly = assembleFeatureLayerStages(stages);
    REQUIRE(secondAssembly.has_value());
    REQUIRE(*secondAssembly == base);
    auto secondResult = searchFeatureLayerAsResultLayer(**secondAssembly, request);
    REQUIRE(secondResult.has_value());
    REQUIRE(secondResult->layer_->size() == 1);
    REQUIRE(secondResult->layer_->toJson()["results"][0]["values"] == nlohmann::json::array({80}));
    auto secondGeometry = secondResult->layer_->at(0)->geometry()->geometryOfTypeAtPreferredStage(GeomType::Points);
    REQUIRE(secondGeometry);
    REQUIRE(secondGeometry->numPoints() == 1);
    REQUIRE(std::abs(secondGeometry->pointAt(0).x - 1.0) < 1e-4);
    REQUIRE(strings->highest() == sourceStringHighWatermark);
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

    request.erase("tileIds");
    request["tileIdsByNextStage"] = nlohmann::json::array({nlohmann::json::array({1, 2})});
    try {
        (void)detail::parseLayerTilesRequestJson(request);
        FAIL("search requests must reject tileIdsByNextStage");
    } catch (const std::runtime_error& e) {
        REQUIRE(std::string(e.what()) == "search requests must use tileIds; tileIdsByNextStage is not supported");
    }
}

TEST_CASE("REST search parser keeps one-shot search fields on envelope", "[feature-layer-search][tiles-request]")
{
    nlohmann::json envelope = {
        {"query", "typeId == 'Road'"},
        {"scope", "attribute"},
        {"withFields", {"$feature.typeId", "$name"}},
    };
    nlohmann::json request = {
        {"mapId", "TestMap"},
        {"layerId", "RoadLayer"},
        {"tileIds", {1, 2}},
    };

    auto search = detail::parseRestSearchEnvelopeJson(envelope);
    auto parsed = detail::parseRestSearchLayerRequestJson(request, search);

    REQUIRE(parsed.searchRequest.has_value());
    REQUIRE(parsed.searchRequest->searchId_.empty());
    REQUIRE(parsed.searchRequest->requestKey_.empty());
    REQUIRE(parsed.searchRequest->scope_ == FeatureLayerSearchScope::Attribute);
    REQUIRE(parsed.searchRequest->withFields_ == std::vector<std::string>{"$feature.typeId", "$name"});
    REQUIRE(detail::collectSearchTileIds(parsed) == std::vector<TileId>{TileId(1), TileId(2)});

    envelope["searchId"] = "interactive-only";
    try {
        (void)detail::parseRestSearchEnvelopeJson(envelope);
        FAIL("REST search must reject interactive search fields");
    } catch (const std::runtime_error& e) {
        REQUIRE(std::string(e.what()).find("WebSocket-only") != std::string::npos);
    }
}
