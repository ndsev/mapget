#include "mapget/model/mapschema.h"
#include "simfil/model/schema-node.h"

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <sstream>

using namespace mapget;

TEST_CASE(
    "Native schema descriptors expose mapget metadata from their canonical owner",
    "[mapschema][schema-descriptor]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Attributes", LayerType::SourceData);
    auto attribute = owner->newAttribute("accuracy", "fixture.Accuracy");
    auto root = attribute.id();
    owner->setZserioType(root, "fixture.PackedAccuracy");
    owner->setAttributeType(root, "fixture.AttributeCode");
    owner->setAttributeMetadata(
        root,
        {"Road", "rules", "FIRST", root, "Attributes"},
        "fixture.AttributeCode");
    owner->setAttributeMetadata(
        root,
        {"Lane", "otherRules", "SECOND", root, "Attributes"},
        "fixture.AttributeCode");
    auto scalar = owner->addSchema(
        simfil::SchemaType::Kind::Int,
        "accuracy-value",
        "Measurement",
        "#/properties/value");
    attribute.addField("value", owner->schemaType(scalar));
    layer->setRootSchema(root);
    owner->finalize();
    std::ostringstream output(std::ios::binary);
    REQUIRE(owner->write(output));
    auto bytes = output.str();
    auto decoded = MapSchema::read({reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()});

    for (auto const& graph : {MapSchema::ConstPtr(owner), decoded}) {
        auto node = graph->schemaType(root).modelNode();
        REQUIRE(node);
        CHECK(node->owningModel().get() == graph.get());
        auto read = [&](simfil::ModelNode const& value, std::string_view name)
        {
            auto child = value.get(graph->strings()->get(name));
            REQUIRE(child);
            CHECK(child->owningModel().get() == graph.get());
            return child->toJson();
        };
        CHECK(read(*node, "schemaKey") == "accuracy");
        CHECK(read(*node, "metaType") == "Attribute");
        CHECK(read(*node, "zserioType") == "fixture.PackedAccuracy");
        CHECK(read(*node, "attributeType") == "fixture.AttributeCode");
        CHECK_FALSE(node->get(graph->strings()->get("attributeTypeCode")));
        auto owners = node->get(graph->strings()->get("attributeOwners"));
        REQUIRE(owners);
        REQUIRE(owners->size() == 2);
        auto firstOwner = owners->at(0);
        REQUIRE(firstOwner);
        CHECK(read(*firstOwner, "featureType") == "Road");
        CHECK(read(*firstOwner, "layer") == "rules");
        CHECK(read(*firstOwner, "name") == "FIRST");
        CHECK(read(*firstOwner, "featureLayer") == "Attributes");
        auto resolvedOwner = graph->resolve(firstOwner->addr());
        CHECK(read(*resolvedOwner, "name") == "FIRST");
        CHECK(read(*owners->at(1), "name") == "SECOND");
        size_t visited = 0;
        CHECK_FALSE(owners->iterate(simfil::ModelNode::IterLambda(
            [&](simfil::ModelNode const&)
            {
                ++visited;
                return false;
            })));
        CHECK(visited == 1);
        auto fields = node->get(simfil::StringPool::SchemaFields);
        REQUIRE(fields);
        auto field = fields->get(graph->strings()->get("value"));
        REQUIRE(field);
        CHECK(read(*field, "schemaKey") == "accuracy-value");
        CHECK(read(*field, "jsonPointer") == "#/properties/value");
        CHECK(read(*field, "metaType") == "Measurement");

        std::set<simfil::StringId> names;
        for (uint32_t i = 0; i < node->size(); ++i) {
            auto key = node->keyAt(i);
            CHECK(names.insert(key).second);
            CHECK(node->at(i)->toJson() == node->get(key)->toJson());
        }
    }
}

TEST_CASE(
    "Native schema descriptor children independently retain their mapget owner",
    "[mapschema][schema-descriptor]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Values", LayerType::SourceData);
    auto root = owner->addSchema(simfil::SchemaType::Kind::Int, "measurement", "Measurement");
    layer->setRootSchema(root);
    owner->finalize();
    auto metadata = owner->typeNode(root)->get(owner->strings()->get("metaType"));
    REQUIRE(metadata);
    CHECK_FALSE(metadata->schemaType());
    std::weak_ptr<MapSchema> weak = owner;
    layer = {};
    owner.reset();
    CHECK_FALSE(weak.expired());
    CHECK(metadata->toJson() == "Measurement");
    metadata = {};
    CHECK(weak.expired());
}

TEST_CASE(
    "Attribute-owner descriptors retain stable rows independently of their type node",
    "[mapschema][schema-descriptor]")
{
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->newLayer("Attributes", LayerType::SourceData);
    auto attribute = owner->newAttribute("accuracy");
    auto root = attribute.id();
    owner->setAttributeMetadata(root, {"Road", "rules", "ACCURACY", root, "Attributes"}, {});
    layer->setRootSchema(root);
    owner->finalize();
    auto node = owner->typeNode(root);
    auto uses = node->get(owner->dictionary().get("attributeOwners"));
    REQUIRE(uses);
    auto use = uses->at(0);
    REQUIRE(use);
    CHECK_FALSE(uses->schemaType());
    CHECK_FALSE(use->schemaType());
    CHECK_FALSE(uses->at(-1));
    CHECK_FALSE(uses->at(1));
    auto nameKey = owner->dictionary().get("name");
    auto invalid = simfil::ModelNodeAddress{use->addr().column(), 0xffffff};
    auto result = owner->resolve(
        *owner->resolve(invalid),
        simfil::Model::Lambda([](simfil::ModelNode const&)
                              { FAIL("Invalid owner row resolved"); }));
    CHECK_FALSE(result);

    std::weak_ptr<MapSchema> weak = owner;
    layer = {};
    node = {};
    owner.reset();
    CHECK_FALSE(weak.expired());
    CHECK(use->get(nameKey)->toJson() == "ACCURACY");
    uses = {};
    CHECK_FALSE(weak.expired());
    CHECK(use->get(nameKey)->toJson() == "ACCURACY");
    use = {};
    CHECK(weak.expired());
}
