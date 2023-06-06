#include <catch2/catch_test_macros.hpp>

#include "mapget/model/info.h"
#include "mapget/model/stream.h"

using namespace mapget;

TEST_CASE("DataSourceInfo JSON Serialization", "[DataSourceInfo]")
{
    // Create a DataSourceInfo object
    std::map<std::string, std::shared_ptr<LayerInfo>> layers;
    layers["testLayer"] = std::make_shared<LayerInfo>(LayerInfo{
        "testLayer",
        LayerType::Features,
        std::vector<FeatureTypeInfo>(),
        std::vector<int>{0, 1, 2},
        std::vector<Coverage>(),
        true,
        false,
        Version{1, 0, 0}});

    DataSourceInfo info(DataSourceInfo{
        "testNodeId",
        "testMapId",
        layers,
        5,
        nlohmann::json::object(),
        TileLayerStream::CurrentProtocolVersion});

    // Serialize it to JSON
    nlohmann::json j = info.toJson();

    // Deserialize it back into a DataSourceInfo object, then serialize it again.
    auto j2 = DataSourceInfo::fromJson(j).toJson();

    // Check that the two DataSourceInfo objects are equal
    REQUIRE(j == j2);
}

TEST_CASE("DataSourceInfo JSON Deserialization", "[DataSourceInfo]")
{
    // create a JSON object with some mandatory fields missing
    nlohmann::json j = R"({
        "nodeId": "testNodeId",
        "protocolVersion": {
            "major": 1,
            "minor": 0,
            "patch": 0
        }
    })"_json;

    // Attempting to deserialize should throw an exception because "mapId" is missing.
    REQUIRE_THROWS_AS(DataSourceInfo::fromJson(j), std::runtime_error);
}