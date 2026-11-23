#include <catch2/catch_test_macros.hpp>

#include "mapget/model/mapschema.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <picosha2.h>
#include <array>
#include <barrier>
#include <fstream>
#include <future>
#include <sstream>

using namespace mapget;
namespace
{
using Type = simfil::SchemaType;
using Kind = Type::Kind;
using Role = Type::FieldRole;

struct Profile
{
    MapSchema::Ptr map = std::make_shared<MapSchema>();
    LayerSchema::Ptr layer = map->newLayer("Roads", LayerType::Features);
    Type integer = map->schemaType(map->primitive(Kind::Int));
    FeatureSchemaType host = layer->newFeature({"Road", {{{"roadKey", {}, IdPartDataType::U32}}}});
    AttributeSchemaType attr = map->newAttribute("source-profile");
    simfil::SchemaObjectType record = map->newObjectType();
    simfil::SchemaObjectType payload = map->newObjectType();

    Profile()
    {
        auto speed = map->schemaType(map->addSchema(Kind::Int)).asScalar();
        speed.setNumericDomain({.unit = "km/h", .minimum = int64_t{0}, .maximum = int64_t{255}});
        record.addField("rawSpeed", speed);
        record.addField("at", integer);
        record.addField("key", integer);
        auto rows = map->schemaType(map->arrayOf(record.id()));
        payload.addField("rows", rows);
        auto branch = map->newObjectType();
        branch.addField("local", rows);
        branch.addField("target", integer);
        payload.addField("branches", map->schemaType(map->arrayOf(branch.id())));
        payload.addField("width", integer);
        attr.addField("attributeValue", payload);
        attr.addField("condition", map->schemaType(map->primitive(Kind::Bool)));
        layer->bindAttribute(attr, "Road", "rules", "SPEED");
        layer->bindAttribute(attr, "Road", "other", "UNSAMPLED");
    }

    static PointSeries series()
    {
        PointSeries s;
        s.featureType_ = "Road";
        s.attributeLayer_ = "rules";
        s.attribute_ = "SPEED";
        s.records_ = {"attributeValue", "rows"};
        s.position_.kind_ = PointSeries::Position::Kind::VertexIndex;
        s.position_.field_ = {"at"};
        s.fields_ = {{"speed", {"rawSpeed"}, "mph", "decode"}};
        return s;
    }

    static PointSeries::Codec codec()
    {
        PointSeries::Codec c;
        c.scale_ = 0.5;
        c.offset_ = 1;
        c.codes_ = {{255, {}, "range", 100., 120.}, {0, 0., {}, {}, {}}};
        return c;
    }
};

std::vector<uint8_t> bytes(MapSchema const& schema)
{
    std::ostringstream output(std::ios::binary);
    REQUIRE(schema.write(output));
    auto s = output.str();
    return {s.begin(), s.end()};
}

Type occurrenceList(Type root, std::string_view layer = "rules", std::string_view name = "SPEED")
{
    return root.field("$attributes").type().field(layer).type().field(name).type();
}

Type occurrence(Type root, std::string_view layer = "rules", std::string_view name = "SPEED")
{
    return occurrenceList(root, layer, name).itemType();
}

Type attribute(Type feature, std::string_view layer = "rules", std::string_view name = "SPEED")
{
    return feature.field("properties")
        .type()
        .field("layer")
        .type()
        .field(layer)
        .type()
        .field(name)
        .type();
}

/** Check the candidate contract, not the local numeric handles of either query graph. */
void checkSpeedCandidate(Type candidate)
{
    REQUIRE(candidate);
    auto value = candidate.field("value").type();
    REQUIRE(value.numericDomain());
    CHECK(value.kind() == Kind::Float);
    CHECK(value.numericDomain()->unit == "mph");
    CHECK(candidate.field("status").type().kind() == Kind::String);
    auto source = candidate.field("contributors").type().itemType();
    CHECK(source.field("recordIndex").type().kind() == Kind::Int);
    CHECK(source.field("weight").type().kind() == Kind::Float);
    CHECK(source.field("path").type().elements().size() == 2);
    auto raw = source.field("raw").type();
    REQUIRE(raw.numericDomain());
    CHECK(raw.kind() == Kind::Int);
    CHECK(raw.numericDomain()->unit == "km/h");
    REQUIRE(raw.numericDomain()->maximum);
    CHECK(std::visit([](auto n) { return n == 255; }, *raw.numericDomain()->maximum));
}

std::vector<uint8_t> golden(char const* file)
{
    std::ifstream input(std::string(MAPGET_TEST_DATA_DIR) + "/" + file, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

uint32_t wireVersion(std::vector<uint8_t> const& data)
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> reader(Adapter(data.begin(), data.end()));
    uint32_t magic = 0, version = 0;
    reader.value4b(magic);
    reader.value4b(version);
    REQUIRE(magic == 0x4843534d);  // MSCH
    return version;
}
}  // namespace

TEST_CASE(
    "Point roots expose the same typed occurrence via named index and wildcard",
    "[mapschema][pointseries]")
{
    Profile f;
    auto s = f.series();
    s.measure_ = PointSeries::Measure::Wgs84Planar2D;
    s.assignmentMeasure_ = PointSeries::Measure::GeographicMetres;
    s.sampling_ = PointSeries::Sampling::Linear;
    s.geometryName_ = "profile";
    s.omittedValidityCovers_ = true;
    s.markers_ = PointSeries::Markers::AppendToValidity;
    s.branches_ = {"attributeValue", "branches"};
    s.branchTarget_ = {"target"};
    s.hostTarget_ = {"roadKey"};
    s.branchRecords_ = {"local"};
    s.position_.kind_ = PointSeries::Position::Kind::Fraction;
    s.position_.bitsFromAttribute_ = {"attributeValue", "width"};
    s.alternative_ = {"key"};
    s.fields_.push_back({"kilometresPerHour", {"rawSpeed"}, {}, {}});
    f.layer->addPointSeries(s);
    f.layer->addPointCodec("decode", f.codec());
    f.map->finalize();
    auto binary = bytes(*f.map);
    auto decoded = MapSchema::read(binary, f.map->schemaId());
    auto json = f.layer->toJsonSchema();
    auto imported = LayerSchema::fromJsonSchema(json);
    REQUIRE(bytes(*decoded) == binary);
    for (auto layer : {f.layer, decoded->layer("Roads"), imported}) {
        auto map = layer->mapSchema();
        REQUIRE(layer->pointSeries().size() == 1);
        auto const& series = layer->pointSeries().front();
        CHECK(series.assignmentMeasure_ == s.assignmentMeasure_);
        CHECK(series.omittedValidityCovers_);
        CHECK(series.markers_ == s.markers_);
        CHECK(series.geometryName_ == s.geometryName_);
        CHECK(series.hostTarget_ == s.hostTarget_);
        CHECK(series.position_.bitsFromAttribute_ == s.position_.bitsFromAttribute_);
        auto* codec = layer->pointCodec("decode");
        REQUIRE(codec);
        CHECK(codec->codes_.front().raw_ == 0);
        CHECK(codec->codes_.back().lower_ == 100.);
        CHECK(codec->codes_.back().status_ == "range");
        CHECK_FALSE(layer->pointCodec("other"));
        CHECK_FALSE(layer->shapePointQueryType("NotProduced"));
        auto root = layer->shapePointQueryType("Road");
        REQUIRE(root);
        CHECK(root.field("$geometryOrdinal").type().kind() == Kind::Int);
        CHECK(root.field("$pointIndex").required() == true);
        CHECK(root.field("$feature").role() == Role::Context);
        auto host = root.field("$feature").type();
        auto canonicalHost = map->schemaType(layer->featureSchema("Road"));
        REQUIRE(host);
        CHECK(map->pointResultType(host) == canonicalHost);
        REQUIRE(host.field("roadKey"));
        CHECK(host.field("roadKey").type().kind() == canonicalHost.field("roadKey").type().kind());
        CHECK(root.field("$geometry").role() == Role::Context);
        CHECK(root.field("$attributes").role() == Role::Data);
        auto attrs = root.field("$attributes").type();
        CHECK(attrs.kind() == Kind::Array);
        CHECK(attrs.field("rules").role() == Role::Context);
        CHECK(attrs.field("rules").type().field("SPEED").role() == Role::Context);
        auto occ = occurrence(root);
        REQUIRE(occ);
        auto elements = attrs.elements();
        CHECK(std::ranges::find(elements, occ.id()) != elements.end());
        CHECK(occ.field("attributeValue").role() == Role::Context);
        CHECK(occ.field("condition").role() == Role::Data);
        CHECK(occ.field("$attributeIndex").type().kind() == Kind::Int);
        CHECK(occ.field("$validities").type().itemType().kind() == Kind::Int);
        CHECK(occ.field("$samples").role() == Role::Context);
        CHECK(occ.field("$layerId").required() == false);
        CHECK(occurrence(root, "other", "UNSAMPLED").field("attributeValue").role() == Role::Data);
        auto values = occ.field("speed").type();
        REQUIRE(values.numericDomain());
        CHECK(values.numericDomain()->unit == "mph");
        CHECK(values.kind() == Kind::Float);
        auto rawUnit = occ.field("kilometresPerHour").type().numericDomain();
        REQUIRE(rawUnit);
        CHECK(rawUnit->unit == "km/h");
        auto candidate = occ.field("$samples").type().field("speed").type().itemType();
        REQUIRE(candidate);
        CHECK(candidate.field("alternative").type().kind() == Kind::Int);
        CHECK(candidate.field("value").type() == values);
        auto overlay =
            map->schemaType(layer->attributeQuerySchema("Road", attribute(canonicalHost).id()));
        REQUIRE(overlay);
        CHECK(overlay.field("$samples").role() == Role::Context);
        CHECK(overlay.field("$samples").required() == false);
        checkSpeedCandidate(overlay.field("$samples").type().field("speed").type().itemType());
        checkSpeedCandidate(candidate);
        CHECK(overlay.field("validity").role() == Role::Data);
        CHECK(overlay.field("validity").type().id() == map->findSchema("mapget:Validities"));
        CHECK_FALSE(attribute(host).field("$samples"));
        CHECK_FALSE(attribute(canonicalHost).field("$samples"));
        auto const& viewNames = root.pool()->dictionary();
        CHECK(root.fieldPaths(viewNames.get("speed")).size() == 1);
        CHECK(root.fieldPaths(viewNames.get("contributors")).empty());
        CHECK(map->strings()->get("profile") != 0);
        CHECK(map->checkForErrors().empty());
    }
    CHECK_THROWS(f.layer->addPointSeries(s));
    CHECK_THROWS(f.layer->addPointCodec("decode", f.codec()));
}

TEST_CASE(
    "Point schema validates actual owner paths, all selected arms and numeric domains",
    "[mapschema][pointseries]")
{
    Profile f;
    auto s = f.series();
    auto c = f.codec();
    SECTION("wrong use site, even when the same raw attribute is reachable")
    {
        s.attributeLayer_ = "other";
    }
    SECTION("unknown feature")
    {
        s.featureType_ = "Lane";
    }
    SECTION("scalar in place of a record array")
    {
        s.records_ = {"attributeValue", "width"};
    }
    SECTION("path must not implicitly descend through an array")
    {
        s.records_.push_back("rawSpeed");
    }
    SECTION("a matching choice arm with a malformed tail cannot be skipped")
    {
        auto ok = f.map->newObjectType(), bad = f.map->newObjectType();
        ok.addField("profile", f.payload);
        bad.addField("profile", f.map->newObjectType());
        auto choice = f.map->newCompositionType(Type::Composition::OneOf);
        choice.addAlternative(ok);
        choice.addAlternative(bad);
        f.attr.addField("selected", choice);
        s.records_ = {"selected", "profile", "rows"};
    }
    SECTION("a lookup alias is not a literal raw field")
    {
        f.record.addAlias("notStored", "rawSpeed");
        s.fields_[0].path_ = {"notStored"};
    }
    SECTION("unresolved value member")
    {
        s.fields_[0].path_ = {"typo"};
    }
    SECTION("wrong record type on a second arm")
    {
        auto alt = f.map->newObjectType();
        alt.addField("at", f.map->schemaType(f.map->primitive(Kind::String)));
        alt.addField("rawSpeed", f.integer);
        auto arr = f.map->newArrayType();
        arr.addElement(f.record);
        arr.addElement(alt);
        f.payload.addField("mixed", arr);
        s.records_.back() = "mixed";
    }
    SECTION("no integer representation for an alternative key")
    {
        s.alternative_ = {"direction"};
        f.record.addField("direction", f.map->schemaType(f.map->primitive(Kind::Float)));
    }
    SECTION("width outside the supported shift domain")
    {
        s.position_.kind_ = PointSeries::Position::Kind::Fraction;
        s.position_.bits_ = {"invalidWidth"};
        auto bad = f.map->schemaType(f.map->addSchema(Kind::Int)).asScalar();
        bad.setNumericDomain({.minimum = uint64_t{64}, .maximum = uint64_t{128}});
        f.record.addField("invalidWidth", bad);
    }
    SECTION("numerator can never fit the declared width")
    {
        s.position_.kind_ = PointSeries::Position::Kind::Fraction;
        s.position_.bits_ = {"smallWidth"};
        s.position_.field_ = {"largeNumerator"};
        auto width = f.map->schemaType(f.map->addSchema(Kind::Int)).asScalar();
        width.setNumericDomain({.maximum = uint64_t{3}});
        auto n = f.map->schemaType(f.map->addSchema(Kind::Int)).asScalar();
        n.setNumericDomain({.minimum = uint64_t{8}});
        f.record.addField("smallWidth", width);
        f.record.addField("largeNumerator", n);
    }
    SECTION("width missing")
    {
        s.position_.kind_ = PointSeries::Position::Kind::Fraction;
    }
    SECTION("reversed codec range")
    {
        c.codes_.front().upper_ = 0.;
    }
    SECTION("nonfinite affine scale")
    {
        c.scale_ = std::numeric_limits<double>::infinity();
    }
    SECTION("duplicate exceptional code")
    {
        c.codes_.push_back(c.codes_.front());
    }
    SECTION("non-integral input to a code table")
    {
        f.record.addField("real", f.map->schemaType(f.map->primitive(Kind::Float)));
        s.fields_[0].path_ = {"real"};
    }
    SECTION("raw code out of physical input range")
    {
        c.codes_.front().raw_ = 256;
    }
    SECTION("adjacent integer bounds above the float precision limit are distinct")
    {
        auto scalar = f.map->schemaType(f.map->addSchema(Kind::Int)).asScalar();
        scalar.setNumericDomain({.maximum = uint64_t{INT64_MAX - 1}});
        f.record.addField("wide", scalar);
        s.fields_[0].path_ = {"wide"};
        c.codes_.front().raw_ = INT64_MAX;
    }
    SECTION("unknown codec")
    {
        s.fields_[0].codec_ = "noDecoder";
    }
    SECTION("non-numeric output for interpolation")
    {
        f.record.addField("color", f.map->schemaType(f.map->primitive(Kind::String)));
        s.sampling_ = PointSeries::Sampling::Linear;
        s.fields_[0] = {"name", {"color"}, {}, {}};
    }
    SECTION("unknown end bound")
    {
        s.position_.kind_ = PointSeries::Position::Kind::Interval;
        s.position_.end_ = {"missing"};
    }
    SECTION("parallel columns may not be scalars")
    {
        s.records_ = {"attributeValue"};
        s.parallel_ = true;
        s.position_.field_ = {"width"};
    }
    SECTION("duplicate contextual output")
    {
        s.fields_.push_back(s.fields_[0]);
    }
    SECTION("an unresolved referenced branch must not fall back to the primary")
    {
        s.branches_ = {"attributeValue", "branches"};
        s.branchTarget_ = {"target"};
        s.branchRecords_ = {"missing"};
        s.hostTarget_ = {"roadKey"};
    }
    SECTION("unbounded accessor depth")
    {
        s.fields_[0].path_.resize(65, "x");
    }
    auto publish = [&]
    {
        f.layer->addPointCodec("decode", c);
        f.layer->addPointSeries(s);
        f.map->finalize();
    };
    CHECK_THROWS(publish());
    CHECK_FALSE(f.map->finalized());
}

TEST_CASE(
    "Sparse parallel and dense scalar accessors retain explicit array boundaries",
    "[mapschema][pointseries]")
{
    Profile f;
    auto s = f.series();
    s.parallel_ = true;
    s.records_ = {"attributeValue"};
    s.position_.field_ = {"positions"};
    s.fields_ = {{"measurement", {"readings"}, {}, {}}};
    auto column = f.map->schemaType(f.map->arrayOf(f.integer.id()));
    f.payload.addField("positions", column);
    f.payload.addField("readings", column);
    auto dense = s;
    dense.parallel_ = false;
    dense.records_.push_back("readings");
    dense.fields_ = {{"dense", {}, {}, {}}};
    dense.position_ = {};
    f.layer->addPointSeries(s);
    CHECK_THROWS(f.layer->pointSeries("Road", "rules", "SPEED"));
    f.layer->addPointSeries(dense);
    f.map->finalize();
    auto schema = MapSchema::read(bytes(*f.map));
    auto layer = schema->layer("Roads");
    auto occ = occurrence(layer->shapePointQueryType("Road"));
    REQUIRE(occ);
    auto selected = layer->pointSeries("Road", "rules", "SPEED");
    REQUIRE(selected.size() == 2);
    CHECK(selected.data() == layer->pointSeries().data());
    CHECK(layer->pointSeries("Lane", "rules", "SPEED").empty());
    CHECK(layer->pointSeries("Road", "other", "SPEED").empty());
    CHECK(occ.field("measurement").type().kind() == Kind::Int);
    CHECK(occ.field("dense").type().kind() == Kind::Int);
}

TEST_CASE(
    "Composition preserves canonical marker remaps and rebuilds site-qualified point views",
    "[mapschema][pointseries]")
{
    Profile a, b;
    auto descriptor = a.series();
    descriptor.markers_ = PointSeries::Markers::PointValidities;
    auto codec = a.codec();
    a.layer->addPointCodec("decode", codec);
    a.layer->addPointSeries(descriptor);
    a.map->finalize();
    SECTION("canonical bytes independent of codec registration and table order")
    {
        b.layer->addPointSeries(descriptor);
        std::ranges::reverse(codec.codes_);
        b.layer->addPointCodec("decode", codec);
        b.map->finalize();
        CHECK(bytes(*a.map) == bytes(*b.map));
        return;
    }
    bool conflict = false;
    SECTION("additional named output")
    {
        descriptor.fields_[0].name_ = "another";
    }
    SECTION("compatibility checks include transform semantics")
    {
        codec.scale_ = 99.;
        conflict = true;
    }
    SECTION("same output cannot change sampling silently")
    {
        descriptor.sampling_ = PointSeries::Sampling::Linear;
        conflict = true;
    }
    b.layer->addPointCodec("decode", codec);
    b.layer->addPointSeries(descriptor);
    b.map->finalize();
    REQUIRE(a.map->schemaId() != b.map->schemaId());
    std::array<MapSchema::ConstPtr, 2> inputs{a.map, b.map};
    if (conflict) {
        CHECK_THROWS(MapSchema::compose({}, inputs));
        return;
    }
    auto merged = MapSchema::compose({}, inputs);
    auto layer = merged->layer("Roads");
    auto physical = attribute(merged->schemaType(layer->featureSchema("Road")));
    auto query = merged->schemaType(layer->attributeQuerySchema("Road", physical.id()));
    auto occ = occurrence(layer->shapePointQueryType("Road"));
    for (auto const& input : inputs) {
        auto origin = input->layer("Roads");
        auto name = origin->pointSeries().front().fields_.front().name_;
        auto native = attribute(input->schemaType(origin->featureSchema("Road")));
        auto overlay = input->schemaType(origin->attributeQuerySchema("Road", native.id()));
        auto candidate = overlay.field("$samples").type().field(name).type().itemType();
        auto effective = query.field("$samples").type().field(name).type().itemType();
        REQUIRE(candidate);
        REQUIRE(effective);
        CHECK(merged->remapSchema(input->schemaId(), native.id()) == physical.id());
        CHECK(merged->remapSchema(input->schemaId(), overlay.id()) == query.id());
        CHECK(merged->remapSchema(input->schemaId(), candidate.id()) == effective.id());
        checkSpeedCandidate(occ.field("$samples").type().field(name).type().itemType());
        auto sourceUse = occurrence(origin->shapePointQueryType("Road"));
        CHECK(sourceUse.field(name));
        CHECK_FALSE(sourceUse.field(name == "speed" ? "another" : "speed"));
    }
    REQUIRE(occ.field("speed"));
    REQUIRE(occ.field("another"));
    auto loaded = MapSchema::read(bytes(*merged));
    CHECK(bytes(*merged) == bytes(*loaded));
    auto restored = occurrence(loaded->layer("Roads")->shapePointQueryType("Road"));
    CHECK(restored.field("speed"));
    CHECK(restored.field("another"));
}

TEST_CASE(
    "One extension can specialize just one of two uses of the same physical attribute",
    "[mapschema][pointseries]")
{
    Profile base, extension;
    base.map->finalize();  // Both use sites start as ordinary covering occurrences.
    extension.layer->addPointSeries(extension.series());
    extension.layer->addPointCodec("decode", extension.codec());
    extension.map->finalize();
    auto original = base.layer->shapePointQueryType("Road");
    auto specialized = extension.layer->shapePointQueryType("Road");
    REQUIRE(specialized);
    REQUIRE(original);
    CHECK(specialized.pool()->identity() != original.pool()->identity());
    // A trace can persist this declared candidate even without virtual validity markers.
    auto candidate =
        occurrence(specialized).field("$samples").type().field("speed").type().itemType();
    checkSpeedCandidate(extension.map->pointResultType(candidate));
    CHECK(extension.map->pointResultType(occurrence(specialized)).kind() == Kind::Any);
    REQUIRE(occurrence(original));
    CHECK(occurrence(original) == occurrence(original, "other", "UNSAMPLED"));
    CHECK(occurrenceList(original) == occurrenceList(original, "other", "UNSAMPLED"));
    CHECK(occurrence(original).field("attributeValue").role() == Role::Data);
    CHECK_FALSE(occurrence(original).field("speed"));
    auto raw = attribute(original.field("$feature").type());
    CHECK(base.map->pointResultType(raw) == base.attr);
    CHECK_THROWS_AS(extension.map->pointResultType(raw), std::invalid_argument);

    std::array<MapSchema::ConstPtr, 2> inputs{base.map, extension.map};
    for (int order = 0; order < 2; ++order) {
        CAPTURE(order);
        auto merged = MapSchema::compose({}, inputs);
        auto layer = merged->layer("Roads");
        auto host = merged->schemaType(layer->featureSchema("Road"));
        REQUIRE(attribute(host) == attribute(host, "other", "UNSAMPLED"));
        auto root = layer->shapePointQueryType("Road");
        auto sampled = occurrence(root), ordinary = occurrence(root, "other", "UNSAMPLED");
        REQUIRE(sampled);
        REQUIRE(ordinary);
        CHECK(sampled != ordinary);
        CHECK(occurrenceList(root) != occurrenceList(root, "other", "UNSAMPLED"));
        CHECK(sampled.field("attributeValue").role() == Role::Context);
        CHECK(ordinary.field("attributeValue").role() == Role::Data);
        CHECK(sampled.field("speed"));
        CHECK_FALSE(ordinary.field("speed"));
        CHECK_FALSE(ordinary.field("$samples").type().field("speed"));
        auto wildcard = root.field("$attributes").type().elements();
        CHECK(std::ranges::find(wildcard, sampled.id()) != wildcard.end());
        CHECK(std::ranges::find(wildcard, ordinary.id()) != wildcard.end());
        CHECK_FALSE(occurrence(original).field("speed"));
        std::swap(inputs[0], inputs[1]);
    }
}

TEST_CASE(
    "Querying hundreds of ordinary uses does not grow the published graph",
    "[mapschema][pointseries]")
{
    auto make = [](unsigned count)
    {
        Profile f;
        for (unsigned i = 0; i < count; ++i)
            f.layer->bindAttribute(f.attr, "Road", "rules", "ordinary" + std::to_string(i));
        for (auto unit : {"m", "ft"}) {
            auto height = f.map->schemaType(f.map->addSchema(Kind::Float)).asScalar();
            height.setNumericDomain({.unit = unit});
            auto raw = f.map->newAttribute(std::string("height-") + unit);
            raw.addField("height", height);
            f.layer->bindAttribute(raw, "Road", "rules", unit);
        }
        f.map->finalize();
        return f.map;
    };
    auto small = make(1), wide = make(256);
    // The producer has more names/edges but no extra payload types.
    CHECK(wide->typeCount() == small->typeCount());
    auto before = bytes(*wide);
    REQUIRE(wireVersion(before) == 9);
    auto decoded = MapSchema::read(before, wide->schemaId());
    for (auto map : {MapSchema::ConstPtr(wide), decoded}) {
        auto counts = map->statistics().at("counts");
        auto typeCount = map->typeCount();
        auto root = map->layer("Roads")->shapePointQueryType("Road");
        REQUIRE(root);
        for (auto name : {"ordinary0", "ordinary127", "ordinary255"}) {
            CAPTURE(name);
            auto use = occurrence(root, "rules", name);
            REQUIRE(use);
            CHECK(use.field("attributeValue").role() == Role::Data);
            CHECK(use.field("condition").type().kind() == Kind::Bool);
            CHECK(use.field("$validities").type().itemType().kind() == Kind::Int);
        }
        auto metres = occurrenceList(root, "rules", "m"),
             feet = occurrenceList(root, "rules", "ft");
        REQUIRE(metres.itemType());
        REQUIRE(feet.itemType());
        CHECK(metres != feet);
        CHECK(metres.itemType() != feet.itemType());
        for (auto unit : {"m", "ft"}) {
            auto height = occurrence(root, "rules", unit).field("height").type();
            REQUIRE(height.numericDomain());
            CHECK(height.numericDomain()->unit == unit);
        }
        CHECK(map->typeCount() == typeCount);
        // Live query-cache telemetry is allowed to grow; the published graph is not.
        auto after = map->statistics().at("counts");
        for (auto key :
             {"types",
              "physicalTypes",
              "fields",
              "fieldEdges",
              "elementEdges",
              "alternativeEdges",
              "internedNames",
              "queryContexts",
              "attributeQueryContexts"})
        {
            CAPTURE(key);
            CHECK(after.at(key) == counts.at(key));
        }
        CHECK(bytes(*map) == before);
    }
}

TEST_CASE(
    "Concurrent first use retains a point view beyond host cache churn and producer lifetime",
    "[mapschema][pointseries]")
{
    Type retained;
    {
        Profile f;
        auto cm = f.map->schemaType(f.map->addSchema(Kind::Int)).asScalar();
        cm.setTypeName("fixture.RawDistance");
        cm.setNumericDomain({.encoding = "int16", .unit = "m", .scale = .01, .offset = 1.});
        cm.addNamedValue(
            {.name = "MISSING",
             .value = int64_t{-32768},
             .role = Type::NamedValue::Role::Special,
             .meaning = Type::NamedValue::Meaning::Unknown});
        f.attr.addField("distance", cm);
        f.layer->addPointSeries(f.series());
        f.layer->addPointCodec("decode", f.codec());
        for (unsigned i = 0; i < 12; ++i) {
            auto name = "Other" + std::to_string(i);
            (void)f.layer->newFeature({name, {{{"key", {}, IdPartDataType::U32}}}});
            f.layer->bindAttribute(f.attr, name, "rules", "SPEED");
        }
        f.map->finalize();
        std::array<std::future<Type>, 6> queries;
        std::barrier start(static_cast<std::ptrdiff_t>(queries.size()));
        for (auto& next : queries)
            next = std::async(
                std::launch::async,
                [layer = f.layer, &start]
                {
                    start.arrive_and_wait();
                    return layer->shapePointQueryType("Road");
                });
        for (auto& next : queries) {
            auto view = next.get();
            REQUIRE(view);
            if (!retained)
                retained = view;
            CHECK(view == retained);
        }
        CHECK(f.map->pointResultType(occurrence(retained).field("distance").type()) == cm);
        for (unsigned i = 0; i < 12; ++i) {
            auto other = f.layer->shapePointQueryType("Other" + std::to_string(i));
            REQUIRE(other);
            CHECK(other.pool()->identity() != retained.pool()->identity());
            REQUIRE(occurrence(other).field("condition"));
            CHECK_FALSE(occurrence(other).field("speed"));
        }
    }
    checkSpeedCandidate(
        occurrence(retained).field("$samples").type().field("speed").type().itemType());
    for (auto use : {attribute(retained.field("$feature").type()), occurrence(retained)}) {
        auto distance = use.field("distance").type();
        CHECK(distance.typeName() == "fixture.RawDistance");
        auto domain = distance.numericDomain();
        REQUIRE(domain);
        CHECK(domain->encoding == "int16");
        CHECK(domain->unit == "m");
        CHECK(domain->scale == .01);
        CHECK(domain->offset == 1.);
        auto missing = distance.namedValue("MISSING");
        REQUIRE(missing);
        CHECK(missing->role == Type::NamedValue::Role::Special);
        CHECK(missing->meaning == Type::NamedValue::Meaning::Unknown);
    }
}

TEST_CASE(
    "Two process-wide warm point views respect active readers and source lifetime",
    "[mapschema][pointseries]")
{
    auto source = [](std::string field) -> MapSchema::ConstPtr
    {
        Profile f;
        auto series = f.series();
        series.fields_.front().name_ = std::move(field);
        f.layer->addPointCodec("decode", f.codec());
        f.layer->addPointSeries(std::move(series));
        f.map->finalize();
        return f.map;
    };
    auto query = [](auto const& map)
    {
        return map->layer("Roads")->shapePointQueryType("Road");
    };
    auto warm = [&](auto const& map)
    {
        auto root = query(map);
        REQUIRE(root);
        return root.pool()->weak_from_this();  // Do not keep a reader alive.
    };
    auto live = [](MapSchema::ConstPtr const& map)
    {
        return map->statistics().at("counts").at("livePointQueryViews").get<size_t>();
    };
    auto a = source("firstSpeed"), b = source("secondSpeed"), c = source("thirdSpeed");
    auto reader = query(a);
    REQUIRE(reader);
    auto original = reader.pool()->weak_from_this();
    auto second = warm(b);
    auto third = warm(c);
    REQUIRE_FALSE(second.expired());
    REQUIRE_FALSE(third.expired());
    CHECK(live(a) == 1);
    CHECK(live(b) == 1);
    CHECK(live(c) == 1);
    // A is cold now. Read its retained handle without refreshing its cache slot.
    REQUIRE_FALSE(original.expired());
    CHECK(occurrence(reader).field("firstSpeed"));
    reader = {};
    REQUIRE(original.expired());
    CHECK(live(a) == 0);

    auto again = query(a);
    REQUIRE(again);
    checkSpeedCandidate(
        occurrence(again).field("$samples").type().field("firstSpeed").type().itemType());
    CHECK_FALSE(occurrence(again).field("secondSpeed"));
    CHECK(live(b) + live(c) == 1);
    auto rewarmed = again.pool()->weak_from_this();
    again = {};
    REQUIRE_FALSE(rewarmed.expired());
    CHECK(live(a) == 1);
    auto producer = std::weak_ptr(a);
    a.reset();
    CHECK(producer.expired());
    CHECK(rewarmed.expired());
}

TEST_CASE(
    "Composing a legacy contributor preserves its wire and type translation contract",
    "[mapschema][pointseries]")
{
    Profile fresh;
    auto series = fresh.series();
    series.markers_ = PointSeries::Markers::AppendToValidity;
    fresh.layer->addPointSeries(series);
    fresh.layer->addPointCodec("decode", fresh.codec());
    fresh.map->finalize();
    REQUIRE(wireVersion(bytes(*fresh.map)) == 9);
    MapSchema::ConstPtr old;
    uint32_t effectiveVersion = 9;
    SECTION("V7 has no point view and does not force expanded output")
    {
        old = MapSchema::read(golden("mapschema-v7.bin"));
        CHECK_FALSE(old->layer("Values")->shapePointQueryType("Reference"));
    }
    SECTION("V8 keeps every expanded handle translatable")
    {
        // schema.bin from the original seven-file generate-shape-point-fixture Lane publication.
        // Do not regenerate it with the compact schema writer.
        auto data = golden("mapschema-v8.bin");
        REQUIRE(wireVersion(data) == 8);
        REQUIRE(
            picosha2::hash256_hex_string(data) ==
            "e5617df572cb402be5240cbc7bf4d3934bec59d8982eb120a99ec87e35e22c28");
        old = MapSchema::read(
            data,
            "mapget-schema:sha256:"
            "40c1fa82d182d8f54107a68652788cb9a21ac09694f780269d8a01a0b364311e");
        auto lane = old->layer("Lane")->shapePointQueryType("Lane");
        REQUIRE(occurrence(lane, "LaneAdasLayerWithGeo", "CURVATURE").field("curvature"));
        CHECK(bytes(*old) == data);
        effectiveVersion = 8;
    }
    std::array<MapSchema::ConstPtr, 2> inputs{old, fresh.map};
    auto merged = MapSchema::compose({}, inputs);
    auto data = bytes(*merged);
    CHECK(wireVersion(data) == effectiveVersion);
    for (auto const& source : inputs) {
        for (size_t i = 1; i <= source->typeCount(); ++i) {
            auto id = merged->remapSchema(source->schemaId(), simfil::TypeId(i));
            REQUIRE(merged->schemaType(id));
        }
    }
    auto restored = MapSchema::read(data, merged->schemaId());
    CHECK(bytes(*restored) == data);
    REQUIRE(occurrence(restored->layer("Roads")->shapePointQueryType("Road")).field("speed"));
    if (effectiveVersion == 8) {
        auto lane = restored->layer("Lane")->shapePointQueryType("Lane");
        REQUIRE(occurrence(lane, "LaneAdasLayerWithGeo", "CURVATURE").field("curvature"));
    }
}

TEST_CASE(
    "Marker query overlays keep possible named uses separate from stored validity and payloads",
    "[mapschema][pointseries]")
{
    Profile f;
    auto first = f.series();
    first.markers_ = PointSeries::Markers::PointValidities;
    auto other = first;
    other.attributeLayer_ = "other";
    other.attribute_ = "UNSAMPLED";
    other.fields_.front().name_ = "limit";
    other.markers_ = PointSeries::Markers::AppendToValidity;
    f.layer->addPointSeries(first);
    f.layer->addPointSeries(other);
    f.layer->addPointCodec("decode", f.codec());
    f.map->finalize();
    auto restored = MapSchema::read(bytes(*f.map));
    for (auto map : {MapSchema::ConstPtr(f.map), restored}) {
        auto layer = map->layer("Roads");
        auto overlay = map->schemaType(layer->attributeQuerySchema("Road", f.attr.id()));
        auto samples = overlay.field("$samples");
        REQUIRE(samples);
        CHECK(samples.role() == Role::Context);
        REQUIRE(layer->pointSeries("Road", "other", "UNSAMPLED").size() == 1);
        REQUIRE(layer->pointSeries("Road", "rules", "SPEED").size() == 1);
        CHECK(
            layer->pointSeries("Road", "rules", "SPEED").data() ==
            layer->pointSeries("Road", "other", "UNSAMPLED").data() + 1);
        auto types = samples.types();
        REQUIRE_FALSE(types.empty());
        auto root = layer->shapePointQueryType("Road");
        for (auto [group, attr, field] :
             {std::tuple{"rules", "SPEED", "speed"}, std::tuple{"other", "UNSAMPLED", "limit"}})
        {
            auto pointSamples = occurrence(root, group, attr).field("$samples").type();
            checkSpeedCandidate(pointSamples.field(field).type().itemType());
            CHECK_FALSE(pointSamples.field(field == std::string_view("speed") ? "limit" : "speed"));
            auto found = std::ranges::find_if(
                types,
                [&](auto id) { return bool(map->schemaType(id).field(field)); });
            REQUIRE(found != types.end());
            checkSpeedCandidate(map->schemaType(*found).field(field).type().itemType());
        }
        auto validities = overlay.field("pointValidities");
        REQUIRE(validities);
        CHECK(validities.role() == Role::Data);
        CHECK(validities.type().itemType().id() == map->findSchema("mapget:Validity"));
        CHECK_FALSE(validities.type().itemType().field("$samples"));
        CHECK_FALSE(map->schemaType(f.attr.id()).field("pointValidities"));
    }
}

TEST_CASE(
    "Named raw choices select record arms without flattening unrelated arrays",
    "[mapschema][pointseries]")
{
    Profile f;
    auto single = f.map->newObjectType(), other = f.map->newObjectType();
    single.addField("anchors", f.map->schemaType(f.map->arrayOf(f.record.id())));
    other.addField("constant", f.integer);
    auto choice = f.map->newCompositionType(Type::Composition::OneOf);
    choice.addAlternative(other);
    choice.addAlternative(single);
    f.payload.addField("profile", choice);
    auto s = f.series();
    s.records_ = {"attributeValue", "profile", "anchors"};
    f.layer->addPointSeries(s);
    f.layer->addPointCodec("decode", f.codec());
    f.map->finalize();
    auto restored = MapSchema::read(bytes(*f.map));
    auto root = restored->layer("Roads")->shapePointQueryType("Road");
    REQUIRE(occurrence(root).field("speed"));
}

TEST_CASE(
    "Vertex-measure stations and interval bounds use the declared signed numeric axis",
    "[mapschema][pointseries]")
{
    Profile f;
    auto series = f.series();
    auto value = f.map->schemaType(f.map->addSchema(Kind::Float)).asScalar();
    value.setNumericDomain({.unit = "m", .minimum = -20., .maximum = -1.});
    series.position_.kind_ = PointSeries::Position::Kind::VertexMeasure;
    series.position_.vertexMeasures_ = {"mCoordinates"};
    series.position_.field_ = {"station"};
    f.attr.addField("mCoordinates", f.map->schemaType(f.map->arrayOf(value.id())));
    f.record.addField("station", value);
    bool valid = false;
    SECTION("exact station")
    {
        valid = true;
    }
    SECTION("interval with a negative end")
    {
        series.position_.kind_ = PointSeries::Position::Kind::Interval;
        series.position_.intervalAxis_ = PointSeries::Position::Kind::VertexMeasure;
        series.position_.end_ = {"station"};
        valid = true;
    }
    SECTION("missing measure array")
    {
        series.position_.vertexMeasures_.clear();
    }
    SECTION("axis references a scalar")
    {
        series.position_.vertexMeasures_ = {"attributeValue", "width"};
    }
    SECTION("axis is a record-relative path")
    {
        series.position_.vertexMeasures_ = {"station"};
    }
    SECTION("axis has nonnumeric elements")
    {
        auto text = f.map->primitive(Kind::String);
        f.attr.addField("strings", f.map->schemaType(f.map->arrayOf(text)));
        series.position_.vertexMeasures_ = {"strings"};
    }
    SECTION("different declared station unit")
    {
        auto other = f.map->schemaType(f.map->addSchema(Kind::Float)).asScalar();
        other.setNumericDomain({.unit = "cm"});
        f.record.addField("centimetres", other);
        series.position_.field_ = {"centimetres"};
    }
    SECTION("no metric conversion on M")
    {
        series.position_.distanceScale_ = .01;
    }
    SECTION("width not applicable")
    {
        series.position_.bits_ = {"at"};
    }
    SECTION("measure array on a different axis")
    {
        series.position_.kind_ = PointSeries::Position::Kind::Distance;
    }
    auto publish = [&]
    {
        f.layer->addPointSeries(series);
        f.layer->addPointCodec("decode", f.codec());
        f.map->finalize();
    };
    if (!valid) {
        CHECK_THROWS(publish());
        return;
    }
    publish();
    auto binary = MapSchema::read(bytes(*f.map));
    auto json = LayerSchema::fromJsonSchema(f.layer->toJsonSchema());
    for (auto layer : {f.layer, binary->layer("Roads"), json}) {
        auto rows = layer->pointSeries("Road", "rules", "SPEED");
        REQUIRE(rows.size() == 1);
        CHECK(rows.front().position_.vertexMeasures_ == series.position_.vertexMeasures_);
        CHECK(rows.front().position_.kind_ == series.position_.kind_);
        CHECK(rows.front().position_.intervalAxis_ == series.position_.intervalAxis_);
        auto root = layer->shapePointQueryType("Road");
        CHECK(occurrence(root).field("mCoordinates").role() == Role::Context);
    }
}

TEST_CASE(
    "Checksum-valid point artifacts still validate the expanded or compact representation",
    "[mapschema][pointseries]")
{
    Profile f;
    f.layer->addPointSeries(f.series());
    f.layer->addPointCodec("decode", f.codec());
    f.map->finalize();
    auto reject = [](std::vector<uint8_t> const& data, auto mutate)
    {
        auto warm = MapSchema::read(data);
        using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
        bitsery::Deserializer<Adapter> reader(Adapter(data.begin(), data.end()));
        uint32_t magic = 0, version = 0;
        std::string id, payload;
        reader.value4b(magic);
        reader.value4b(version);
        reader.text1b(id, 128);
        reader.text1b(payload, data.size());
        REQUIRE(reader.adapter().isCompletedSuccessfully());
        mutate(version, payload);
        id = "mapget-schema:sha256:" + picosha2::hash256_hex_string(payload);
        std::ostringstream output(std::ios::binary);
        bitsery::Serializer<bitsery::OutputStreamAdapter> writer(output);
        writer.value4b(magic);
        writer.value4b(version);
        writer.text1b(id, 128);
        writer.text1b(payload, UINT32_MAX);
        writer.adapter().flush();
        auto mutated = output.str();
        CHECK_THROWS(MapSchema::read(
            std::span(reinterpret_cast<uint8_t const*>(mutated.data()), mutated.size())));
    };
    SECTION("old version with the same cached identity")
    {
        reject(
            bytes(*f.map),
            [](auto& version, auto&)
            {
                REQUIRE(version == 9);
                version = 8;
            });
    }
    SECTION("V9 accessor changed independently of the producer graph")
    {
        reject(
            bytes(*f.map),
            [](auto&, auto& payload)
            {
                auto field = payload.find("rawSpeed");
                REQUIRE(field != std::string::npos);
                payload.replace(field, 8, "badSpeed");
            });
    }
    SECTION("V8 expanded use-site key does not match its declaration")
    {
        reject(
            golden("mapschema-v8.bin"),
            [](auto& version, auto& payload)
            {
                REQUIRE(version == 8);
                auto key = payload.find("/sample/curvature/source");
                REQUIRE(key != std::string::npos);
                payload[key + 1] = 'X';
            });
    }
}
