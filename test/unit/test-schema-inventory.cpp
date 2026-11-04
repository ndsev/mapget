#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mapget/model/mapschema.h"
#include "mapget/service/service.h"

#include <regex>
#include <set>
#include <stdexcept>
#include <system_error>

using namespace mapget;

namespace
{
/** A metadata-only provider whose layer/type breadth makes traversal work observable. */
class InventorySource final : public DataSource
{
public:
    /** Build real native roots, or share an existing immutable owner between consumers. */
    explicit InventorySource(
        std::string map,
        size_t layerCount = 1,
        size_t typeCount = 1,
        MapSchema::ConstPtr schema = {})
    {
        if (!schema) {
            auto builder = std::make_shared<MapSchema>(MapFormat{"inventory-test", "1", {}});
            for (size_t i = 0; i < layerCount; ++i) {
                auto layer = builder->newLayer("Layer" + std::to_string(i), LayerType::Features);
                auto root = layer->addSchema(simfil::Schema::Kind::Union);
                layer->setComposition(root, MapSchema::Composition::AnyOf);
                for (size_t j = 0; j < typeCount; ++j) {
                    auto name = "Feature" + std::to_string(i) + "_" + std::to_string(j);
                    auto feature =
                        layer->addSchema(LayerSchema::FeatureKind, LayerSchema::featureKey(name));
                    auto identity = FeatureTypeInfo::fromJson(
                        {{"name", name},
                         {"uniqueIdCompositions", {{{{"partId", "id"}, {"datatype", "U32"}}}}}});
                    layer->addFeatureType(identity, feature);
                    layer->addAlternative(root, feature);
                }
                layer->setRootSchema(root);
            }
            builder->finalize();
            schema = std::move(builder);
        }
        metadata.mapId_ = std::move(map);
        metadata.stringPoolId_ = metadata.mapId_ + "-strings";
        metadata.maxParallelJobs_ = 1;
        for (auto const& name : schema->layerNames()) {
            auto layer = std::make_shared<LayerInfo>();
            layer->layerId_ = name;
            layer->type_ = schema->layer(name)->layerType();
            metadata.layers_.emplace(name, std::move(layer));
        }
        metadata.setMapSchema(std::move(schema));
    }

    /** Return the immutable declaration without fetching data. */
    DataSourceInfo info() override { return metadata; }
    /** Inventory must never load a feature partition. */
    void fill(PartitionFeatureLayer::Ptr const&) override { FAIL("Inventory loaded feature data"); }
    /** Inventory must never load a source-data partition. */
    void fill(PartitionSourceDataLayer::Ptr const&) override
    {
        FAIL("Inventory loaded source data");
    }

    DataSourceInfo metadata;
};
}  // namespace

TEST_CASE(
    "Schema inventory categorizes roots without changing their native layer type",
    "[schema-inventory]")
{
    auto owner = std::make_shared<MapSchema>();
    auto features = owner->newLayer("Metadata-Features", LayerType::Features);
    auto feature = features->addSchema(LayerSchema::FeatureKind);
    features->addFeatureType(
        FeatureTypeInfo::fromJson(
            {{"name", "Road"},
             {"uniqueIdCompositions", {{{{"partId", "id"}, {"datatype", "U32"}}}}}}),
        feature);
    features->setRootSchema(feature);
    for (auto const* name : {"Raw-Header", "Metadata-Settings"}) {
        auto layer = owner->newLayer(name, LayerType::SourceData);
        auto root = layer->addSchema(simfil::Schema::Kind::Object);
        layer->addFieldSchema(root, "version", layer->addSchema(simfil::Schema::Kind::Int));
        layer->setOpen(root, std::string_view(name).starts_with("Metadata-"));
        layer->setRootSchema(root);
    }
    owner->finalize();
    Service service;
    service.add(std::make_shared<InventorySource>("Categorized", 0, 0, owner));
    auto inventory = service.schemaInventory(AuthHeaders{});
    REQUIRE(inventory.size() == 1);
    auto const& layers = inventory[0].at("layers");
    REQUIRE(layers.size() == 3);
    for (auto const& layer : layers) {
        auto const name = layer.at("layerId").get<std::string>();
        auto const definition = owner->layer(name);
        CHECK(layer.at("typeId") == definition->rootSchema());
        CHECK_FALSE(layer.contains("schemaId"));
        if (name == "Metadata-Features") {
            CHECK(layer.at("type") == "Features");
            CHECK(layer.at("category") == "Features");
            REQUIRE(layer.at("featureTypes").size() == 1);
            CHECK(layer.at("featureTypes")[0].at("typeId") == feature);
        }
        else {
            CHECK(layer.at("type") == "SourceData");
            CHECK(layer.at("category") == (name == "Raw-Header" ? "SourceData" : "Metadata"));
            CHECK(layer.at("open") == (name == "Metadata-Settings"));
            CHECK(layer.at("reachabilityComplete") == (name == "Raw-Header"));
        }
    }
}

TEST_CASE(
    "Schema inventory filters authorized owners before enumerating their roots",
    "[schema-inventory]")
{
    Service service;
    auto selected = std::make_shared<InventorySource>("Selected");
    auto excluded = std::make_shared<InventorySource>("Excluded", 32, 4);
    auto hidden = std::make_shared<InventorySource>("Hidden", 2, 2);
    hidden->requireAuthHeaderRegexMatchOption("x-inventory", std::regex("allowed"));
    for (auto const& source : {selected, excluded, hidden})
        service.add(source);
    std::set<std::string> seen;
    size_t checkpoints = 0;
    auto result = service.schemaInventory(
        AuthHeaders{},
        [&](MapSchema const& schema)
        {
            seen.emplace(schema.schemaId());
            return schema.schemaId() == selected->metadata.schemaId_;
        },
        [&] { ++checkpoints; });
    REQUIRE(result.size() == 1);
    CHECK(result[0]["mapSchemaId"] == selected->metadata.schemaId_);
    CHECK(result[0]["layers"].size() == 1);
    CHECK(seen.size() == 2);
    CHECK_FALSE(seen.contains(hidden->metadata.schemaId_));
    // Even checkpoint work stays below the excluded graph's layer count.
    CHECK(checkpoints < 32);
    CHECK(result.dump().find("Hidden") == std::string::npos);
    CHECK(result.dump().find("Excluded") == std::string::npos);
}

TEST_CASE("Schema inventory checkpoints cover layer and feature loops", "[schema-inventory]")
{
    auto count = [](size_t layers, size_t types)
    {
        Service service;
        service.add(std::make_shared<InventorySource>("Counted", layers, types));
        size_t calls = 0;
        auto inventory = service.schemaInventory(AuthHeaders{}, {}, [&] { ++calls; });
        REQUIRE(inventory.size() == 1);
        return calls;
    };
    auto baseline = count(1, 1);
    CHECK(count(1, 7) == baseline + 6);
    CHECK(count(4, 1) == baseline + 6);  // One layer and one feature checkpoint per extra root.
}

TEST_CASE(
    "Schema inventory selects authorized providers before expanding unrelated graphs",
    "[schema-inventory]")
{
    Service service;
    auto selected = std::make_shared<InventorySource>("Selected");
    auto excluded = std::make_shared<InventorySource>("Excluded", 32, 4);
    auto shared = std::make_shared<InventorySource>("Shared", 1, 1, selected->metadata.mapSchema_);
    auto hidden = std::make_shared<InventorySource>("Hidden", 2, 2);
    hidden->requireAuthHeaderRegexMatchOption("x-inventory", std::regex("allowed"));
    for (auto const& source : {selected, excluded, shared, hidden})
        service.add(source);
    std::string selectedSourceId;
    for (auto const& source : service.sourceCatalog(AuthHeaders{}, false).sources)
        if (source.info && source.info->mapId_ == "Selected")
            selectedSourceId = source.descriptor.sourceId;
    REQUIRE_FALSE(selectedSourceId.empty());

    auto selectBySourceId = GENERATE(false, true);
    size_t checkpoints = 0;
    size_t owners = 0;
    std::set<std::string> seenMaps;
    auto result = service.schemaInventory(
        AuthHeaders{},
        [&](MapSchema const&)
        {
            ++owners;
            return true;
        },
        [&]
        {
            if (++checkpoints > 32)
                throw std::length_error("inventory work budget");
        },
        [&](std::string_view sourceId, std::string_view mapId)
        {
            seenMaps.emplace(mapId);
            return selectBySourceId ? sourceId == selectedSourceId : mapId == "Selected";
        });
    REQUIRE(result.size() == 1);
    CHECK(result[0]["mapSchemaId"] == selected->metadata.schemaId_);
    CHECK(result[0]["layers"].size() == 1);
    CHECK(result[0]["ownerInstances"] == 1);
    REQUIRE(result[0]["consumers"].size() == 1);
    CHECK(result[0]["consumers"][0]["sourceId"] == selectedSourceId);
    CHECK(result[0]["consumers"][0]["mapId"] == "Selected");
    CHECK(owners == 1);
    CHECK(seenMaps == std::set<std::string>{"Selected", "Excluded", "Shared"});
    CHECK(checkpoints < 32);
}

TEST_CASE(
    "Schema inventory propagates cancellation without returning partial rows",
    "[schema-inventory]")
{
    Service service;
    service.add(std::make_shared<InventorySource>("Bounded", 5, 5));
    auto budget = GENERATE(size_t{0}, size_t{1}, size_t{5}, size_t{20});
    size_t calls = 0;
    REQUIRE_THROWS_AS(
        service.schemaInventory(
            AuthHeaders{},
            {},
            [&]
            {
                if (calls++ >= budget)
                    throw std::length_error("inventory work budget");
            }),
        std::length_error);
    CHECK(calls == budget + 1);
    REQUIRE_THROWS_AS(
        service.schemaInventory(
            AuthHeaders{},
            [](MapSchema const&) -> bool { throw std::invalid_argument("invalid selection"); }),
        std::invalid_argument);
    CHECK(service.schemaInventory(AuthHeaders{}).size() == 1);
}

TEST_CASE(
    "Schema inventory callbacks can reenter registration but cannot publish stale consumers",
    "[schema-inventory]")
{
    Service service;
    auto source = std::make_shared<InventorySource>("Replaced");
    service.add(source);
    auto replaceFromCheckpoint = GENERATE(false, true);
    size_t checkpoints = 0;
    try {
        (void)service.schemaInventory(
            AuthHeaders{},
            [&](MapSchema const&)
            {
                if (!replaceFromCheckpoint)
                    service.remove(source);
                return true;
            },
            [&]
            {
                // The first callback precedes snapshotting. The second is inside traversal.
                if (replaceFromCheckpoint && ++checkpoints == 2) {
                    service.remove(source);
                    service.add(source);  // Same datasource/graph, different pinned registration.
                }
            });
        FAIL("A changed registry returned an inventory from its former generation");
    }
    catch (std::system_error const& error) {
        CHECK(error.code() == std::errc::resource_unavailable_try_again);
    }
    CHECK(service.schemaInventory(AuthHeaders{}).size() == (replaceFromCheckpoint ? 1 : 0));
}

TEST_CASE(
    "Filtered schema inventory retains shared owners and all authorized consumers",
    "[schema-inventory]")
{
    Service service;
    auto first = std::make_shared<InventorySource>("First");
    auto second = std::make_shared<InventorySource>("Second", 1, 1, first->metadata.mapSchema_);
    service.add(first);
    service.add(second);
    auto result = service.schemaInventory(
        AuthHeaders{},
        [&](MapSchema const& schema) { return schema.schemaId() == first->metadata.schemaId_; });
    REQUIRE(result.size() == 1);
    CHECK(result[0]["ownerInstances"] == 1);
    CHECK(result[0]["consumers"].size() == 2);
    CHECK(
        result[0]["retainedBytes"] ==
        first->metadata.mapSchema_->statistics().at("memory").at("allocated-bytes"));
}
