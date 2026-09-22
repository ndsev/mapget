#include <algorithm>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/model/hash.h"

using namespace mapget;
using Catch::Matchers::ContainsSubstring;

namespace
{
/** Own a small layer and its dictionary, including binary corruption helpers for validation tests.
 */
class FeatureIdIndexTest
{
public:
    std::shared_ptr<LayerInfo> info = LayerInfo::fromJson(R"({
        "layerId": "Ways", "type": "Features", "featureTypes": [
            {"name": "Way", "uniqueIdCompositions": [[
                {"partId": "area", "datatype": "STR"},
                {"partId": "hint", "datatype": "STR", "isOptional": true},
                {"partId": "featureId", "datatype": "U64"}]]},
            {"name": "Other", "uniqueIdCompositions": [[
                {"partId": "area", "datatype": "STR"},
                {"partId": "featureId", "datatype": "U64"}]]}
        ]})"_json);
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>("id-index");
    PartitionFeatureLayer::Ptr layer = emptyLayer();

    /** Create another layer using the same metadata and dictionary. */
    PartitionFeatureLayer::Ptr emptyLayer() const
    {
        return std::make_shared<PartitionFeatureLayer>(TileId{}, "id-index", "Test", info, strings);
    }

    /** Serialize the layer without a stream envelope to permit targeted malformed-input tests. */
    std::vector<uint8_t> bytes() const
    {
        std::stringstream stream;
        layer->write(stream);
        auto data = stream.str();
        return {data.begin(), data.end()};
    }

    /** Restore a layer without implicitly running semantic validation. */
    PartitionFeatureLayer::Ptr read(std::vector<uint8_t> const& data) const
    {
        return std::make_shared<PartitionFeatureLayer>(
            data,
            [&](auto&&, auto&&) { return info; },
            [&](auto&&) { return strings; });
    }

    /** Show validation diagnostics without skipping the remaining behavioral assertions. */
    void checkValid(PartitionFeatureLayer const& value) const
    {
        auto validation = value.validate();
        INFO((validation ? "Valid layer" : validation.error().message));
        CHECK(validation);
    }

    /** Compute the required-parts hash for this fixture's Way IDs. */
    uint32_t hash(int64_t id) const
    {
        return static_cast<uint32_t>(
            Hash().mix("Way").mix(KeyValueViewPairs{{"area", "A"}, {"featureId", id}}).value());
    }

    /** Locate an unambiguous little-endian record before corrupting a serialized payload. */
    size_t offsetOf(std::vector<uint8_t> const& data, uint64_t record) const
    {
        std::vector<uint8_t> pattern(8);
        put(pattern, 0, record, 8);
        auto found = std::search(data.begin(), data.end(), pattern.begin(), pattern.end());
        REQUIRE(found != data.end());
        REQUIRE(std::search(found + 1, data.end(), pattern.begin(), pattern.end()) == data.end());
        return static_cast<size_t>(found - data.begin());
    }

    /** Replace one fixed-width wire value without relying on host endianness or alignment. */
    void put(std::vector<uint8_t>& data, size_t offset, uint64_t value, size_t width) const
    {
        REQUIRE(offset + width <= data.size());
        for (size_t i = 0; i < width; ++i)
            data[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
};
}  // namespace

TEST_CASE_METHOD(
    FeatureIdIndexTest,
    "Feature ID duplicates leave storage unchanged",
    "[feature-id-index]")
{
    layer->setIdPrefix({{"area", "A"}});
    auto first = layer->newFeature("Way", {{"featureId", int64_t(-1)}});
    auto before = bytes();
    auto stringsBefore = strings->size();
    auto jsonBefore = layer->toJson();
    REQUIRE_THROWS_WITH(
        layer->newFeature("Way", {{"hint", "previously-unseen"}, {"featureId", int64_t(-1)}}),
        ContainsSubstring("Duplicate feature ID"));
    REQUIRE(bytes() == before);
    REQUIRE(strings->size() == stringsBefore);
    REQUIRE(layer->toJson() == jsonBefore);
    REQUIRE(layer->size() == 1);
    REQUIRE(
        layer->find("Way", KeyValueViewPairs{{"area", "A"}, {"featureId", int64_t(-1)}})
            ->addr()
            .value_ == first->addr().value_);
    REQUIRE(first->id()->toString() == "Way.A.18446744073709551615");

    // A present optional component must not allow another feature without that component either.
    layer->newFeature("Way", {{"hint", "one"}, {"featureId", int64_t(2)}});
    REQUIRE_THROWS_WITH(
        layer->newFeature("Way", {{"featureId", int64_t(2)}}),
        ContainsSubstring("Duplicate feature ID"));
    REQUIRE_THROWS_WITH(
        layer->newFeature("Way", {{"hint", "two"}, {"featureId", int64_t(2)}}),
        ContainsSubstring("Duplicate feature ID"));
    checkValid(*layer);
}

TEST_CASE_METHOD(
    FeatureIdIndexTest,
    "Feature IDs distinguish types and required parts",
    "[feature-id-index]")
{
    layer->newFeature("Way", {{"area", "A"}, {"featureId", int64_t(1)}});
    layer->newFeature("Other", {{"area", "A"}, {"featureId", int64_t(1)}});
    layer->newFeature("Way", {{"area", "B"}, {"featureId", int64_t(1)}});
    REQUIRE(layer->size() == 3);
    REQUIRE(layer->serializationSizeStats()["feature-layer"]["feature-hash-index"] == 24);
    auto indexMemory = layer->memoryUsage().components.at("feature-layer.feature-hash-index");
    REQUIRE(indexMemory.logicalBytes == 24);
    REQUIRE(indexMemory.allocatedBytes > indexMemory.logicalBytes);
    auto restored = read(bytes());
    checkValid(*restored);
    REQUIRE(restored->toJson() == layer->toJson());
    REQUIRE(restored->find("Other.A.1"));
    REQUIRE(restored->find("Way.B.1"));
    REQUIRE_THROWS_WITH(
        restored->newFeature("Way", {{"area", "A"}, {"featureId", int64_t(1)}}),
        ContainsSubstring("Duplicate feature ID"));

    auto otherPartition = emptyLayer();
    REQUIRE_NOTHROW(otherPartition->newFeature("Way", {{"area", "A"}, {"featureId", int64_t(1)}}));
    REQUIRE_NOTHROW(layer->newFeatureId("Way", {{"area", "A"}, {"featureId", int64_t(1)}}));
    REQUIRE_NOTHROW(layer->newFeatureId("Way", {{"area", "A"}, {"featureId", int64_t(1)}}));
    REQUIRE(layer->size() == 3);
}

TEST_CASE_METHOD(
    FeatureIdIndexTest,
    "Feature ID hash collisions remain distinct",
    "[feature-id-index]")
{
    constexpr int64_t firstId = 429969831814639362;
    constexpr int64_t secondId = 8087836240262186748;
    REQUIRE(firstId != secondId);
    REQUIRE(hash(firstId) == hash(secondId));
    auto first = layer->newFeature("Way", {{"area", "A"}, {"featureId", firstId}});
    auto second = layer->newFeature("Way", {{"area", "A"}, {"featureId", secondId}});
    auto restored = read(bytes());
    checkValid(*restored);
    REQUIRE(
        restored->find("Way", KeyValueViewPairs{{"area", "A"}, {"featureId", firstId}})
            ->addr()
            .value_ == first->addr().value_);
    REQUIRE(
        restored->find("Way", KeyValueViewPairs{{"area", "A"}, {"featureId", secondId}})
            ->addr()
            .value_ == second->addr().value_);
    REQUIRE_THROWS_WITH(
        restored->newFeature("Way", {{"area", "A"}, {"featureId", secondId}}),
        ContainsSubstring("Duplicate feature ID"));
}

TEST_CASE_METHOD(
    FeatureIdIndexTest,
    "GeoJSON rejects duplicate IDs but clone still merges",
    "[feature-id-index]")
{
    auto feature = layer->newFeature("Way", {{"area", "A"}, {"featureId", int64_t(1)}});
    feature->attributes()->addField("value", layer->newValue(int64_t(42)));
    auto json = layer->toJson();
    json["features"].push_back(json["features"][0]);
    auto imported = emptyLayer();
    REQUIRE_THROWS_WITH(imported->fromJson(json), ContainsSubstring("Duplicate feature ID"));
    REQUIRE(imported->size() == 1);
    checkValid(*imported);

    auto target = emptyLayer();
    PartitionFeatureLayer::CloneCache cache;
    target->newFeature("Way", {{"area", "A"}, {"featureId", int64_t(1)}});
    target->clone(cache, layer, *feature, "Way", {{"area", "A"}, {"featureId", int64_t(1)}});
    REQUIRE(target->size() == 1);
    REQUIRE(target->toJson()["features"][0]["properties"]["value"] == 42);
    checkValid(*target);
}

TEST_CASE_METHOD(
    FeatureIdIndexTest,
    "Binary feature index structure is checked before restoration",
    "[feature-id-index]")
{
    constexpr int64_t id = 123456789;
    auto feature = layer->newFeature("Way", {{"area", "A"}, {"featureId", id}});
    auto data = bytes();
    auto offset = offsetOf(data, uint64_t(feature->addr().value_) | (uint64_t(hash(id)) << 32));
    SECTION("Wrong column")
    {
        put(data, offset, simfil::ModelNodeAddress{simfil::ModelPool::Arrays, 0}.value_, 4);
    }
    SECTION("Out of bounds feature")
    {
        put(data,
            offset,
            simfil::ModelNodeAddress{PartitionFeatureLayer::ColumnId::Features, 1}.value_,
            4);
    }
    SECTION("Wrong entry count")
    {
        put(data, offset - 8, 0, 8);
    }
    SECTION("Huge entry count")
    {
        put(data, offset - 8, UINT64_MAX, 8);
    }
    SECTION("Truncated entry")
    {
        data.resize(offset + 7);
    }
    REQUIRE_THROWS(read(data));
}

TEST_CASE_METHOD(
    FeatureIdIndexTest,
    "Explicit validation detects binary feature identity corruption",
    "[feature-id-index]")
{
    constexpr int64_t firstId = 0x123456789abcdef;
    constexpr int64_t secondId = firstId + 1;
    auto first = layer->newFeature("Way", {{"area", "A"}, {"featureId", firstId}});
    auto second = layer->newFeature("Way", {{"area", "A"}, {"featureId", secondId}});
    auto data = bytes();
    auto offset =
        offsetOf(data, uint64_t(second->addr().value_) | (uint64_t(hash(secondId)) << 32));
    std::string expected;
    SECTION("Duplicate identity with a consistent index")
    {
        put(data, offsetOf(data, secondId), firstId, 8);
        put(data, offset + 4, hash(firstId), 4);
        expected = "Duplicate feature ID";
    }
    SECTION("Incorrect hash")
    {
        put(data, offset + 4, hash(secondId) ^ 1U, 4);
        expected = "correct hash";
    }
    SECTION("Repeated index address")
    {
        put(data, offset, first->addr().value_, 4);
        put(data, offset + 4, hash(firstId), 4);
        expected = "exactly once";
    }
    auto restored = read(data);
    auto validation = restored->validate();
    REQUIRE_FALSE(validation);
    REQUIRE_THAT(validation.error().message, ContainsSubstring(expected));
}
