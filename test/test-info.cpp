#include <catch2/catch_test_macros.hpp>

#include "mapget/model/info.h"
#include "mapget/model/stream.h"
#include "log.h"

using namespace mapget;

TEST_CASE("InfoToJson", "[DataSourceInfo]")
{
    // Log all messages to the console if MAPGET_LOG_FILE is not specified.
    if (getenv("MAPGET_LOG_FILE") == nullptr) {
        setenv("MAPGET_LOG_LEVEL", "trace", 1);
    }

    // Create a DataSourceInfo object.
    std::map<std::string, std::shared_ptr<LayerInfo>> layers;
    layers["testLayer"] = std::make_shared<LayerInfo>(LayerInfo{
        "testLayer",
        LayerType::Features,
        std::vector<FeatureTypeInfo>(),
        std::vector<int>{0, 1, 2},
        std::vector<Coverage>{{1, 2, {}}, {3, 3, {}}},
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

    // Serialize it to JSON.
    nlohmann::json j = info.toJson();
    log().trace("Serialized data source info: {}", to_string(j));

    // Deserialize it back into a DataSourceInfo object, then serialize it again.
    auto j2 = DataSourceInfo::fromJson(j).toJson();

    // Check that the two DataSourceInfo objects are equal.
    REQUIRE(j == j2);
}

TEST_CASE("InfoFromJson", "[DataSourceInfo]")
{
    // Create a JSON object with some mandatory fields missing.
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
