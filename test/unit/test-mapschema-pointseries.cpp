#include <catch2/catch_test_macros.hpp>

#include "mapget/model/mapschema.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <picosha2.h>
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

Type occurrence(Type root, std::string_view layer = "rules", std::string_view name = "SPEED")
{
    return root.field("$attributes").type().field(layer).type().field(name).type().itemType();
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
        CHECK(layer->shapePointQuerySchema("NotProduced") == simfil::NoTypeId);
        auto root = map->schemaType(layer->shapePointQuerySchema("Road"));
        REQUIRE(root);
        CHECK(root.field("$geometryOrdinal").type().kind() == Kind::Int);
        CHECK(root.field("$pointIndex").required() == true);
        CHECK(root.field("$feature").role() == Role::Context);
        CHECK(root.field("$feature").type().id() == layer->featureSchema("Road"));
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
        auto attribute =
            root.field("$feature")
                .type()
                .field("properties")
                .type()
                .field("layer")
                .type()
                .field("rules")
                .type()
                .field("SPEED")
                .type();
        auto overlay = map->schemaType(layer->attributeQuerySchema("Road", attribute.id()));
        REQUIRE(overlay);
        CHECK(overlay.field("$samples").role() == Role::Context);
        CHECK(overlay.field("$samples").required() == false);
        CHECK(overlay.field("$samples").type().field("speed").type().itemType() == candidate);
        CHECK(overlay.field("validity").role() == Role::Data);
        CHECK(overlay.field("validity").type().id() == map->findSchema("mapget:Validities"));
        CHECK_FALSE(attribute.field("$samples"));
        CHECK(candidate.field("status").type().kind() == Kind::String);
        auto contributor = candidate.field("contributors").type().itemType();
        CHECK(contributor.field("recordIndex").type().kind() == Kind::Int);
        CHECK(contributor.field("raw").type().kind() == Kind::Int);
        CHECK(contributor.field("path").type().elements().size() == 2);
        CHECK(root.fieldPaths(map->dictionary().get("speed")).size() == 1);
        CHECK(root.fieldPaths(map->dictionary().get("contributors")).empty());
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
    auto occ = occurrence(schema->schemaType(layer->shapePointQuerySchema("Road")));
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
    "Point declarations are canonical content; composition rebuilds and translates point types",
    "[mapschema][pointseries]")
{
    Profile a, b;
    auto descriptor = a.series();
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
    auto root = merged->schemaType(layer->shapePointQuerySchema("Road"));
    auto occ = occurrence(root);
    for (auto const& input : inputs) {
        auto source = input->schemaType(input->layer("Roads")->shapePointQuerySchema("Road"));
        CHECK(merged->remapSchema(input->schemaId(), source.id()) == root.id());
        CHECK(merged->remapSchema(input->schemaId(), occurrence(source).id()) == occ.id());
    }
    REQUIRE(occ.field("speed"));
    REQUIRE(occ.field("another"));
    auto loaded = MapSchema::read(bytes(*merged));
    CHECK(bytes(*merged) == bytes(*loaded));
    CHECK(loaded->layer("Roads")->shapePointQuerySchema("Road") == root.id());
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
        REQUIRE(types.size() == 2);
        auto root = map->schemaType(layer->shapePointQuerySchema("Road"));
        for (auto [group, attr, field] :
             {std::tuple{"rules", "SPEED", "speed"}, std::tuple{"other", "UNSAMPLED", "limit"}})
        {
            auto actual =
                occurrence(root, group, attr).field("$samples").type().field(field).type();
            CHECK(std::ranges::any_of(
                types,
                [&](auto id) { return map->schemaType(id).field(field).type() == actual; }));
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
    auto root = restored->layer("Roads")->shapePointQuerySchema("Road");
    REQUIRE(occurrence(restored->schemaType(root)).field("speed"));
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
        auto root = layer->mapSchema()->schemaType(layer->shapePointQuerySchema("Road"));
        CHECK(occurrence(root).field("mCoordinates").role() == Role::Context);
    }
}

TEST_CASE(
    "Checksum-valid point artifacts still validate their raw accessors and envelope version",
    "[mapschema][pointseries]")
{
    Profile f;
    f.layer->addPointSeries(f.series());
    f.layer->addPointCodec("decode", f.codec());
    f.map->finalize();
    auto data = bytes(*f.map);
    auto warm = MapSchema::read(data);
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> reader(Adapter(data.begin(), data.end()));
    uint32_t magic = 0, version = 0;
    std::string id, payload;
    reader.value4b(magic);
    reader.value4b(version);
    reader.text1b(id, 128);
    reader.text1b(payload, data.size());
    REQUIRE(version == 8);
    SECTION("old version with the same cached identity")
    {
        version = 7;
    }
    SECTION("declared accessor changed independently of typed graph")
    {
        auto field = payload.find("rawSpeed");
        REQUIRE(field != std::string::npos);
        payload.replace(field, 8, "badSpeed");
        id = "mapget-schema:sha256:" + picosha2::hash256_hex_string(payload);
    }
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
}
