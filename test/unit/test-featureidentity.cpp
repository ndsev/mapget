#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include "mapget/model/mapschema.h"

using namespace mapget;

namespace
{
/** Exercise mixed encodings, alternative keys, and optional prefix slots. */
FeatureTypeInfo identityDeclaration()
{
    return {
        "Road",
        {{{"tileId", "Spatial partition", IdPartDataType::I32},
          {"namespace", "Optional provider key", IdPartDataType::STR, true, true},
          {"roadId", "All unsigned bits", IdPartDataType::U64}},
         {{"globalId", "Provider reference", IdPartDataType::UUID128}}}};
}

/** Decode a fresh owner rather than relying on the process-wide artifact cache. */
MapSchema::ConstPtr roundtrip(MapSchema const& owner)
{
    std::ostringstream output(std::ios::binary);
    REQUIRE(owner.write(output));
    auto data = output.str();
    return MapSchema::read({reinterpret_cast<uint8_t const*>(data.data()), data.size()});
}
}  // namespace

TEST_CASE(
    "FeatureIdentity views borrow canonical storage and retain explicitly",
    "[mapschema][identity]")
{
    auto owner = std::make_shared<MapSchema>();
    owner->addFeatureType(identityDeclaration());
    auto identity = owner->getTypeInfo("Road");
    auto composition = identity.compositions().front();
    auto part = composition.parts()[1];
    CHECK(owner.use_count() == 1);
    CHECK(identity.name() == "Road");
    CHECK(part.name() == "namespace");
    CHECK(part.synthetic());
    CHECK(part.optional());
    CHECK(part.description() == "Optional provider key");
    auto count = owner.use_count();
    for (unsigned i = 0; i < 1000; ++i) {
        auto found = owner->getTypeInfo("Road");
        CHECK(found == identity);
        CHECK(found.compositions().front().parts()[1] == part);
    }
    CHECK(owner.use_count() == count);
    // Appending declarations cannot invalidate existing owner/array/element identities.
    for (unsigned i = 0; i < 80; ++i)
        owner->addFeatureType({"Type" + std::to_string(i), {{{"id", "", IdPartDataType::I64}}}});
    CHECK(owner->getTypeInfo("Road") == identity);
    CHECK(part.name() == "namespace");
    owner->finalize();
    CHECK(owner->getTypeInfo("Road") == identity);
    CHECK_FALSE(owner->getTypeInfo("missing", false));
    CHECK_THROWS(owner->getTypeInfo("missing"));
    CHECK(identity.toDeclaration().sameIdentity(identityDeclaration()));
    CHECK(identity.toJson() == identityDeclaration().toJson());
    auto retainedIdentity = identity.retain();
    auto retainedComposition = composition.retain();
    auto retainedPart = part.retain();
    CHECK(owner.use_count() == count + 3);
    CHECK(retainedIdentity.borrow() == identity);
    std::weak_ptr<MapSchema> weak = owner;
    owner.reset();
    CHECK_FALSE(weak.expired());
    CHECK(retainedIdentity.name() == "Road");
    CHECK(retainedComposition.parts().size() == 3);
    CHECK(retainedPart.name() == "namespace");
    retainedIdentity = {};
    retainedComposition = {};
    CHECK_FALSE(weak.expired());
    retainedPart = {};
    CHECK(weak.expired());
}

TEST_CASE(
    "Canonical identity matching preserves optional slots and unsigned IDs",
    "[mapschema][identity]")
{
    auto owner = std::make_shared<MapSchema>();
    auto declaration = identityDeclaration();
    owner->addFeatureType(declaration);
    auto identity = owner->getTypeInfo("Road");
    auto primary = identity.compositions().front();
    KeyValueViewPairs parts{
        {"tileId", int64_t{-2006751151}},
        {"roadId", std::string_view{"18446744073709551615"}}};
    CHECK(primary.matches(0, parts, parts.size(), true));
    CHECK(primary.matchEndIndex(0, parts, 1) == 1);
    CHECK(primary.matchEndIndex(1, {{"roadId", int64_t{-1}}}, 1) == 3);
    CHECK_FALSE(primary.matchEndIndex(4, parts, 0));
    CHECK_FALSE(primary.matches(0, {{"roadId", int64_t{1}}}, 1, true));
    CHECK_FALSE(primary.matches(0, parts, 3, true));
    CHECK(identity.matchingCompositionIndex(parts) == 0);
    KeyValueViewPairs alternative{{"globalId", std::string_view{"0123456789abcdef"}}};
    CHECK(identity.matchingCompositionIndex(alternative) == 1);
    CHECK_FALSE(
        identity.matchingCompositionIndex(alternative, FeatureIdentity::MatchMode::PrimaryOnly));
    auto idPart = primary.parts().back();
    std::variant<int64_t, std::string> value = std::string{"18446744073709551615"};
    REQUIRE(idPart.validateValue(value));
    CHECK(std::get<int64_t>(value) == std::bit_cast<int64_t>(UINT64_MAX));
    value = std::string{"18446744073709551616"};
    CHECK_FALSE(idPart.validateValue(value));
    CHECK(std::get<std::string>(value) == "18446744073709551616");
    owner->finalize();
    auto decoded = roundtrip(*owner);
    auto restored = decoded->getTypeInfo("Road");
    CHECK_FALSE(restored == identity);
    CHECK(restored.sameIdentity(identity));
    CHECK(restored.toJson() == identity.toJson());
    CHECK(restored.compositions().front().parts().back().datatype() == IdPartDataType::U64);
}

TEST_CASE(
    "Empty identity compositions remain valid arrays through transport",
    "[mapschema][identity]")
{
    auto owner = std::make_shared<MapSchema>();
    FeatureTypeInfo empty{"Singleton", {{}}};
    owner->addFeatureType(empty);
    auto identity = owner->getTypeInfo("Singleton");
    REQUIRE(identity.compositions().size() == 1);
    auto composition = identity.compositions().front();
    CHECK(composition);
    CHECK(composition.parts().empty());
    CHECK(composition.matches(0, {}, 0, true));
    CHECK(identity.matchingCompositionIndex({}) == 0);
    CHECK_FALSE(FeatureIdentity::Composition{});
    CHECK_FALSE(FeatureIdentity::Part{});
    CHECK(identity.toJson().at("uniqueIdCompositions").at(0).is_array());
    CHECK(identity.toJson() == empty.toJson());
    auto imported = FeatureTypeInfo::fromJson(identity.toJson());
    CHECK(imported.sameIdentity(empty));
    owner->finalize();
    auto restored = roundtrip(*owner)->getTypeInfo("Singleton").retain();
    CHECK(restored.compositions().front().parts().empty());
    CHECK(restored.toJson() == empty.toJson());
}

TEST_CASE(
    "Failed identity registration rolls back dictionary and scalar tails",
    "[mapschema][identity]")
{
    auto owner = std::make_shared<MapSchema>();
    auto dictionary = owner->strings();
    // Leave room for the feature name and its first part, but not its second part.
    while (dictionary->highest() < UINT16_MAX - 2)
        REQUIRE(dictionary->emplace("padding-" + std::to_string(dictionary->highest())));
    auto beforeStrings = dictionary->size();
    auto beforeValues = owner->memoryUsageStats().total().logicalBytes;
    FeatureTypeInfo declaration{
        "LateFeature",
        {{{"first", "Allocated prose", IdPartDataType::I64}, {"second", "", IdPartDataType::STR}}}};
    REQUIRE_THROWS(owner->addFeatureType(declaration));
    CHECK(owner->featureIdentities().empty());
    CHECK_FALSE(owner->getTypeInfo("LateFeature", false));
    CHECK(dictionary->size() == beforeStrings);
    CHECK(dictionary->get("LateFeature") == simfil::StringPool::Empty);
    CHECK(dictionary->get("first") == simfil::StringPool::Empty);
    CHECK(owner->memoryUsageStats().total().logicalBytes == beforeValues);
    // Repeating the failure must not leave stale text-index entries or retired arrays.
    REQUIRE_THROWS(owner->addFeatureType(declaration));
    CHECK(dictionary->size() == beforeStrings);
    CHECK(owner->featureIdentities().empty());
}
