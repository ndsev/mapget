#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../../libs/model/src/shapepoint-series.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/mapschema.h"
#include "mapget/model/simfilexpressioncache.h"
#include "mapget/model/simfilutil.h"
#include "shape-point-lane-fixture.h"

#include <algorithm>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>

namespace
{
using namespace mapget;
using Node = simfil::ModelNode::Ptr;
using Json = nlohmann::json;
using Kind = PointSeries::Position::Kind;

// Keep the producer raw: no decoded field and no per-vertex marker table is
// ever written to the feature. Both consumers have to interpret the schema.
struct Profile
{
    MapSchema::Ptr map = std::make_shared<MapSchema>();
    LayerSchema::Ptr schema = map->newLayer("Paths", LayerType::Features);
    PartitionFeatureLayer::Ptr tile;
    model_ptr<Feature> host;
    model_ptr<Attribute> attr;
    model_ptr<simfil::Array> rows;
    model_ptr<simfil::Object> columns;
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>("point-test");

    static PointSeries declaration(Kind kind = Kind::Fraction)
    {
        PointSeries s;
        s.featureType_ = "Path";
        s.attributeLayer_ = "profiles";
        s.attribute_ = "PROFILE";
        s.records_ = {"rows"};
        s.position_.kind_ = kind;
        if (kind != Kind::Dense)
            s.position_.field_ = {"pos"};
        if (kind == Kind::Fraction)
            s.position_.bits_ = {"bits"};
        s.sampling_ = kind == Kind::Fraction ?
            PointSeries::Sampling::Linear :
            PointSeries::Sampling::Exact;
        s.measure_ = PointSeries::Measure::Wgs84Planar2D;
        s.fields_ = {{"reading", {"raw"}, "km/h", {}}};
        return s;
    }

    explicit Profile(
        PointSeries s = declaration(),
        std::vector<Point> line = {{0, 0}, {1, 0}, {2, 0}},
        void (*rawMetadata)(simfil::SchemaScalarType) = nullptr)
    {
        (void)schema->newFeature({"Path", {{{"key", {}, IdPartDataType::U32}}}});
        auto attrType = map->newAttribute("Measurement");
        auto rowType = map->newObjectType();
        auto integer = map->schemaType(map->primitive(simfil::SchemaType::Kind::Int));
        auto list = [&](simfil::SchemaType t)
        {
            return map->schemaType(map->arrayOf(t.id()));
        };
        auto raw = integer;
        if (rawMetadata) {
            raw = map->schemaType(map->addSchema(simfil::SchemaType::Kind::Int));
            rawMetadata(raw.asScalar());
        }
        bool const parallel = s.parallel_;
        for (std::string_view key : {"pos", "bits", "raw", "end", "branch"}) {
            auto type = key == "raw" ? raw : integer;
            rowType.addField(key, parallel ? list(type) : type);
        }
        auto carrier = parallel ? rowType : list(rowType);
        attrType.addField("rows", carrier);
        if (!s.position_.vertexMeasures_.empty())
            attrType.addField(
                "measures",
                list(map->schemaType(map->primitive(simfil::SchemaType::Kind::Float))));
        if (!s.branches_.empty()) {
            auto branch = map->newObjectType();
            branch.addField("target", integer);
            branch.addField("local", carrier);
            attrType.addField("branches", list(branch));
        }
        attrType.addField("condition", integer);
        if (rawMetadata)
            attrType.addField("rawCode", raw);
        schema->bindAttribute(attrType, "Path", "profiles", "PROFILE");
        schema->addPointSeries(std::move(s));
        map->finalize();
        auto layer = std::make_shared<LayerInfo>();
        layer->layerId_ = "Paths";
        DataSourceInfo info;
        info.layers_.emplace("Paths", layer);
        info.setMapSchema(map);
        tile = std::make_shared<PartitionFeatureLayer>(
            TileId::fromTileXY(1, 0, 1),
            "point-test",
            "Map",
            info.getLayer("Paths"),
            strings);
        tile->setGeometryAnchor({0, 0});
        host = tile->newFeature("Path", {{"key", int64_t(1)}});
        host->addLine(line);
        auto group = host->attributeLayers()->newLayer("profiles");
        group->setId(37);
        attr = group->newAttribute("PROFILE");
        if (parallel) {
            columns = tile->newObject();
            REQUIRE(attr->addField("rows", columns));
        }
        else {
            rows = tile->newArray();
            REQUIRE(attr->addField("rows", rows));
        }
        REQUIRE(attr->addField("condition", tile->newValue(int64_t(19))));
    }
    model_ptr<simfil::Object>
    row(int pos, int bits, std::optional<int64_t> raw, int branch = 0, int end = 0) const
    {
        auto row = tile->newObject();
        REQUIRE(row->addField("pos", tile->newValue(int64_t(pos))));
        REQUIRE(row->addField("bits", tile->newValue(int64_t(bits))));
        REQUIRE(row->addField("end", tile->newValue(int64_t(end))));
        REQUIRE(row->addField("branch", tile->newValue(int64_t(branch))));
        if (raw)
            REQUIRE(row->addField("raw", tile->newValue(*raw)));
        return row;
    }
    void record(int pos, int bits, std::optional<int64_t> raw, int branch = 0, int end = 0)
    {
        rows->append(row(pos, bits, raw, branch, end));
    }
    template <class T>
    model_ptr<simfil::Array> numbers(std::vector<T> const& values) const
    {
        auto result = tile->newArray();
        for (auto v : values)
            result->append(v);
        return result;
    }
    Node field(Node const& n, std::string_view key) const
    {
        return n ? n->get(strings->get(key)) : Node{};
    }
    Node occurrences(uint32_t vertex, model_ptr<Feature> const& feature = {}) const
    {
        auto root = (feature ? feature : host)->shapePoints(0).at(vertex)->queryContext();
        return field(field(field(root, "$attributes"), "profiles"), "PROFILE");
    }
    Node occurrence(uint32_t vertex) const
    {
        auto uses = occurrences(vertex);
        REQUIRE(uses);
        REQUIRE(uses->size() == 1);
        return uses->at(0);
    }
    Json reading(uint32_t vertex) const { return field(occurrence(vertex), "reading")->toJson(); }
    Json samples(uint32_t vertex) const
    {
        return field(field(occurrence(vertex), "$samples"), "reading")->toJson();
    }
};

TEST_CASE(
    "Original vertex contexts retain their host, origin and dictionary after the scan",
    "[shapepoint]")
{
    auto tile = mapget::test::shapePointLane();
    auto strings = tile->strings();
    Node first, second, byName, byOrdinal;
    {
        auto host = tile->at(0);
        REQUIRE_FALSE(host->shapePoints(9).geometry());
        REQUIRE_FALSE(host->shapePoints(0).at(2));
        auto points = host->shapePoints(1);
        first = points.at(0)->queryContext();
        second = points.at(1)->queryContext();
        auto attrs = second->get(strings->get("$attributes"));
        byOrdinal = attrs->at(0);
        byName =
            attrs->get(strings->get("LaneAdasLayerWithGeo"))->get(strings->get("CURVATURE"))->at(0);
        REQUIRE(
            host->shapePoints(0).at(1)->queryContext()->get(strings->get("$attributes"))->size() ==
            0);
    }
    tile.reset();
    REQUIRE(first->get(strings->get("$pointIndex"))->toJson() == 0);
    REQUIRE(second->get(strings->get("$geometryOrdinal"))->toJson() == 1);
    REQUIRE(byName->toJson() == byOrdinal->toJson());
    REQUIRE(
        byName->get(strings->get("curvature"))->toJson().get<double>() == Catch::Approx(-.15296));
    auto tree = second->toJson();
    REQUIRE_FALSE(tree.contains("$feature"));
    REQUIRE_FALSE(tree["$attributes"][0].contains("records"));
    REQUIRE(second->get(strings->get("$feature")));
    auto parallel = std::async(std::launch::async, [second] { return second->toJson(); });
    REQUIRE(parallel.get() == tree);
}

TEST_CASE(
    "A covered vertex interpolates its local profile; record paths explain both weights",
    "[shapepoint]")
{
    Profile f;
    f.attr->validity()->newRange(Validity::RelativeLengthOffset, .25, .75);
    f.attr->validity()->newRange(Validity::BufferOffset, 1, 1);
    f.record(0, 1, 80);
    f.record(1, 1, 60);
    REQUIRE(f.occurrences(0)->size() == 0);
    REQUIRE(f.occurrences(2)->size() == 0);
    auto covered = f.occurrences(1);
    REQUIRE(covered->size() == 1);
    auto use = covered->at(0);
    REQUIRE(f.field(use, "reading")->toJson().get<double>() == Catch::Approx(70.));
    REQUIRE(f.field(use, "$validities")->toJson() == Json::array({0, 1}));
    REQUIRE(f.field(use, "$layerId")->toJson() == 37);
    auto candidates = f.field(f.field(use, "$samples"), "reading")->toJson();
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates[0]["unit"] == "km/h");
    REQUIRE(candidates[0]["contributors"][0]["weight"].get<double>() == Catch::Approx(.5));
    REQUIRE(candidates[0]["contributors"][1]["weight"].get<double>() == Catch::Approx(.5));
    REQUIRE(candidates[0]["contributors"][0]["raw"] == 80);
    REQUIRE(candidates[0]["contributors"][1]["path"] == Json::array({"rows", 1, "raw"}));
}

TEST_CASE(
    "Point coverage and anchors respect the declared 2D measure on a bent line",
    "[shapepoint]")
{
    Profile f(Profile::declaration(), {{0, 60}, {2, 60}, {2, 62}});
    // The bend is at 50% in this producer's XY convention; on a geodesic it is at ~33%.
    f.attr->validity()->newRange(Validity::RelativeLengthOffset, .49, .51);
    f.record(0, 1, 80);
    f.record(1, 1, 60);
    REQUIRE(
        f.field(f.occurrences(1)->at(0), "reading")->toJson().get<double>() == Catch::Approx(70));
    SECTION("Off-vertex applicability cannot be widened to the nearest vertex")
    {
        Profile outside;
        outside.attr->validity()->newRange(Validity::RelativeLengthOffset, .10, .30);
        outside.record(0, 1, 80);
        outside.record(1, 1, 60);
        for (uint32_t i = 0; i < 3; ++i)
            REQUIRE(outside.occurrences(i)->size() == 0);
    }
    SECTION("Interpolation needs both ordinary physical values")
    {
        Profile missing;
        missing.attr->validity()->newComplete();
        missing.record(0, 1, 80);
        missing.record(1, 1, {});
        REQUIRE(missing.field(missing.occurrences(1)->at(0), "reading")->toJson().is_null());
    }
}

TEST_CASE("Repeated measurements require a choice, but never lose raw record order", "[shapepoint]")
{
    Profile f(Profile::declaration(Kind::VertexIndex));
    f.attr->validity()->newComplete();
    f.record(1, 0, 10);
    f.record(1, 0, 20);
    auto view = f.occurrences(1)->at(0);
    {
        PointQueryError::Capture capture;
        auto ambiguous = f.field(view, "reading");
        REQUIRE(ambiguous);
        CHECK(ambiguous->toJson().is_null());
        REQUIRE_THROWS_AS(capture.rethrowIfFailed(), PointQueryError);
    }
    Json candidates;
    {
        PointQueryError::Capture fresh;
        candidates = f.field(f.field(view, "$samples"), "reading")->toJson();
        REQUIRE_NOTHROW(fresh.rethrowIfFailed());
    }
    REQUIRE_THROWS_AS(f.field(view, "reading"), PointQueryError);
    REQUIRE(candidates.size() == 2);
    REQUIRE(candidates[0]["contributors"][0]["recordIndex"] == 0);
    REQUIRE(candidates[1]["value"] == 20);
    REQUIRE(f.field(f.occurrences(0)->at(0), "reading")->toJson().is_null());
    auto duplicate = f.host->attributeLayers()->newLayer("profiles");
    duplicate->addAttribute(f.attr);
    REQUIRE(f.occurrences(1)->size() == 2);
}

TEST_CASE(
    "Missing and explicitly empty assignments differ; pointIndex does no attribute work",
    "[shapepoint]")
{
    auto s = Profile::declaration(Kind::VertexIndex);
    s.omittedValidityCovers_ = true;
    Profile f(s);
    f.record(1000, 0, 1);
    auto malformed = f.occurrences(0)->at(0);
    REQUIRE_THROWS_AS(f.field(malformed, "reading"), PointQueryError);
    bool cancelled = true;
    auto retained = f.host->shapePoints(0, [&] { return cancelled; }).at(1)->queryContext();
    REQUIRE(f.field(retained, "$pointIndex")->toJson() == 1);
    REQUIRE_THROWS_AS(f.field(retained, "$attributes")->size(), PointQueryError);
    cancelled = false;
    REQUIRE(f.field(retained, "$attributes")->size() == 1);
    (void)f.attr->validity();
    REQUIRE(f.occurrences(0)->size() == 0);
}

TEST_CASE("Scan work is bounded by the distinct raw arrays and geometry measures", "[shapepoint]")
{
    Profile f;
    f.attr->validity()->newComplete();
    for (uint32_t i = 0; i <= 1023; ++i)
        f.record(i, 10, int(i));
    f.host->attributeLayers()->newLayer("profiles")->addAttribute(f.attr);

    bool abort = false;
    uint32_t probes = 0;
    auto points = f.host->shapePoints(0, [&] { return abort && ++probes == 4; });
    for (uint32_t i = 0; i < points.size(); ++i) {
        auto p = points.at(i)->queryContext();
        REQUIRE(f.field(p, "$pointIndex")->toJson() == i);
    }
    auto cold = points.work();
    REQUIRE(cold.inventories == 0);
    REQUIRE(cold.recordIndexes == 0);
    REQUIRE(cold.distanceTables == 0);

    auto attrs = f.field(points.at(1)->queryContext(), "$attributes");
    REQUIRE(attrs->size() == 2);
    abort = true;
    REQUIRE_THROWS_AS(f.field(attrs->at(0), "reading"), PointQueryError);
    REQUIRE(points.work().recordIndexes == 0);
    abort = false;
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        auto uses = f.field(points.at(vertex)->queryContext(), "$attributes");
        for (int i = 0; i < 2; ++i)
            REQUIRE(
                f.field(uses->at(i), "reading")->toJson().get<double>() ==
                Catch::Approx(vertex * 511.5));
    }
    auto done = points.work();
    REQUIRE(done.inventories == 1);
    REQUIRE(done.recordIndexes == 1);
    REQUIRE(done.recordsIndexed == 1024);
    REQUIRE(done.distanceTables == 1);
}

TEST_CASE(
    "A slice and shared geometry keep local indices and distinct host assignments",
    "[shapepoint]")
{
    Profile f(Profile::declaration(Kind::VertexIndex));
    f.attr->validity()->newComplete();
    f.record(1, 0, 39);
    auto original = f.host->shapePoints(0).geometry();
    auto other = f.tile->newFeature("Path", {{"key", int64_t(2)}});
    other->addGeometry(original);
    REQUIRE(f.field(other->shapePoints(0).at(1)->queryContext(), "$attributes")->size() == 0);
    auto slice = f.tile->newGeometryView(GeomType::Line, 1, 2, original);
    auto third = f.tile->newFeature("Path", {{"key", int64_t(3)}});
    third->addGeometry(slice);
    third->attributeLayers()->newLayer("profiles")->addAttribute(f.attr);
    auto sliced = third->shapePoints(0);
    REQUIRE(sliced.size() == 2);
    REQUIRE(sliced.at(0)->position() == original->pointAt(1));
    auto first = f.field(sliced.at(0)->queryContext(), "$attributes")->at(0);
    auto second = f.field(sliced.at(1)->queryContext(), "$attributes")->at(0);
    REQUIRE(f.field(first, "reading")->toJson().is_null());
    REQUIRE(f.field(second, "reading")->toJson() == 39);

    f.host->addGeometry(original);
    REQUIRE_THROWS_AS(f.occurrences(1)->size(), PointQueryError);
    f.attr->setPointSeriesGeometry(1);
    REQUIRE(f.occurrences(1)->size() == 0);
    REQUIRE(f.field(f.host->shapePoints(1).at(1)->queryContext(), "$attributes")->size() == 1);
}

TEST_CASE(
    "Dense, interval and branched profiles keep their declared assignment rules",
    "[shapepoint]")
{
    SECTION("Dense reads are direct, and malformed lengths are local diagnostics")
    {
        Profile f(Profile::declaration(Kind::Dense));
        f.attr->validity()->newComplete();
        f.record(0, 0, 7);
        f.record(1, 0, 5);
        REQUIRE_THROWS_AS(f.field(f.occurrences(1)->at(0), "reading"), PointQueryError);
        f.record(2, 0, 3);
        auto range = f.host->shapePoints(0);
        auto use = f.field(range.at(1)->queryContext(), "$attributes")->at(0);
        REQUIRE(f.field(use, "reading")->toJson() == 5);
        REQUIRE(range.work().recordIndexes == 0);
    }
    SECTION("Interval endpoints are not silently rounded or made inclusive")
    {
        auto s = Profile::declaration(Kind::Interval);
        s.position_.end_ = {"end"};
        s.position_.includeStart_ = false;
        Profile f(s);
        f.attr->validity()->newComplete();
        f.record(0, 0, 65, 0, 1);
        REQUIRE(f.field(f.occurrences(0)->at(0), "reading")->toJson().is_null());
        REQUIRE(f.field(f.occurrences(1)->at(0), "reading")->toJson() == 65);
        REQUIRE(f.field(f.occurrences(2)->at(0), "reading")->toJson().is_null());
    }
    SECTION("Interpolated candidates cannot cross a continuation boundary")
    {
        auto s = Profile::declaration();
        s.alternative_ = {"branch"};
        Profile f(s);
        f.attr->validity()->newComplete();
        f.record(0, 1, 0, 12);
        f.record(1, 1, 100, 24);
        REQUIRE(f.field(f.occurrences(1)->at(0), "reading")->toJson().is_null());
        f.record(1, 1, 80, 12);
        auto group = f.occurrences(1)->at(0);
        auto candidates = f.field(f.field(group, "$samples"), "reading")->toJson();
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates[0]["value"] == 40.);
        REQUIRE(candidates[0]["alternative"] == 12);
        REQUIRE(candidates[0]["contributors"][1]["recordIndex"] == 2);
    }
    SECTION("A fraction between stored vertices never creates a new vertex")
    {
        auto s = Profile::declaration();
        s.sampling_ = PointSeries::Sampling::Exact;
        Profile f(s);
        f.attr->validity()->newComplete();
        f.record(1, 2, 0);
        REQUIRE(f.host->shapePoints(0).size() == 3);
        for (uint32_t i = 0; i < 3; ++i)
            REQUIRE(f.field(f.occurrences(i)->at(0), "reading")->toJson().is_null());
    }
    SECTION("A zero length path has no measurable interior station")
    {
        Profile f(Profile::declaration(), {{1, 1}, {1, 1}, {1, 1}});
        f.attr->validity()->newComplete();
        f.record(0, 1, 80);
        f.record(1, 1, 60);
        REQUIRE(f.field(f.occurrences(1)->at(0), "reading")->toJson().is_null());
    }
}

TEST_CASE("Point contexts carry the schema used by named and wildcard queries", "[shapepoint]")
{
    Profile f;
    f.attr->validity()->newComplete();
    auto root = f.host->shapePoints(0).at(1)->queryContext();
    auto type = f.schema->shapePointQueryType("Path");
    REQUIRE(type);
    CHECK(type.pool() != f.map.get());
    REQUIRE(root->schemaType() == type);
    CHECK(f.field(root, "$feature")->schemaType() == f.host->schemaType());
    auto attributes = f.field(root, "$attributes");
    REQUIRE(attributes);
    auto layer = f.field(attributes, "profiles");
    REQUIRE(layer);
    auto named = f.field(layer, "PROFILE");
    REQUIRE(named);
    REQUIRE(attributes->schemaType() == type.field("$attributes").type());
    REQUIRE(layer->schemaType() == attributes->schemaType().field("profiles").type());
    REQUIRE(named->schemaType() == layer->schemaType().field("PROFILE").type());
    REQUIRE(attributes->size() == 1);
    REQUIRE(named->size() == 1);
    auto occurrenceType = named->schemaType().itemType();
    REQUIRE(occurrenceType);
    CHECK(occurrenceType == attributes->schemaType().itemType());
    CHECK(named->at(0)->schemaType() == occurrenceType);
    CHECK(attributes->at(0)->schemaType() == occurrenceType);
}

TEST_CASE("Shape-point projection publishes source domains, not query-local IDs",
          "[shapepoint][projection]")
{
    Profile f(Profile::declaration(), {{0, 0}, {1, 0}}, [](simfil::SchemaScalarType raw)
    { raw.setNumericDomain({.encoding = "uint8", .unit = "km/h", .maximum = uint64_t{255}}); });
    f.attr->validity()->newComplete();
    f.record(0, 1, 21);
    f.record(1, 1, 31);
    auto inlineValue = f.tile->newSmallValue(int16_t{7});
    REQUIRE_FALSE(inlineValue->schemaType());
    REQUIRE(f.attr->addField("rawCode", inlineValue));
    auto sourceType = f.attr->schemaType().field("rawCode").type();
    REQUIRE(sourceType.numericDomain());
    FeatureLayerFilterRequest request;
    auto& channel = request.channels_.emplace_back();
    channel.channelId_ = "domains";
    channel.scope_ = FeatureLayerFilterScope::ShapePoint;
    channel.entryFilter_ = "$pointIndex == 0";
    channel.entryFields_ = {"$attributes.profiles.PROFILE.*",
                            "$attributes.profiles.PROFILE.*.rawCode",
                            "trace($attributes.profiles.PROFILE.*.$samples.reading.*, -1, 'candidate')"};
    auto result = request.filter(*f.tile);
    REQUIRE(result);
    auto subset = *result;
    INFO(subset->toJson()["issues"].dump());
    REQUIRE(subset->issues().empty());
    std::stringstream wire;
    REQUIRE(subset->write(wire));
    auto data = wire.str();
    // A reader knows the source MapSchema and dictionary only, never the host's query pool.
    auto decoded = std::make_shared<PartitionSubsetLayer>(
        std::vector<uint8_t>(data.begin(), data.end()),
        [&](auto const&, auto const&) { return f.tile->layerInfo(); },
        [&](auto const&) { return f.strings; });
    auto entry = decoded->at(0)->shapePointEntryAt(0);
    REQUIRE(entry);
    auto slots = entry->values();
    REQUIRE(slots->size() == 3);
    auto occurrence = slots->at(0)->at(0);
    REQUIRE(occurrence);
    CHECK(occurrence->schemaType().pool() == f.map.get());
    auto measurement = f.field(occurrence, "reading");
    REQUIRE(measurement);
    CHECK(measurement->type() == simfil::ValueType::Float);
    REQUIRE(measurement->schemaType().numericDomain());
    CHECK(measurement->schemaType().numericDomain()->unit == "km/h");
    CHECK(f.field(occurrence, "rawCode")->schemaType() == sourceType);
    CHECK(slots->at(1)->at(0)->schemaType() == sourceType);
    auto checkCandidate = [&](Node const& candidate) {
        REQUIRE(candidate);
        CHECK(candidate->schemaType().pool() == f.map.get());
        auto contributors = f.field(candidate, "contributors");
        REQUIRE(contributors);
        REQUIRE(contributors->size() == 1);
        auto raw = f.field(contributors->at(0), "raw");
        REQUIRE(raw);
        CHECK(raw->toJson() == 21);
        CHECK(raw->schemaType() == sourceType);
    };
    checkCandidate(slots->at(2)->at(0));
    REQUIRE(decoded->traceCount() == 1);
    checkCandidate(decoded->traceAt(0)->values()->at(0));
}

TEST_CASE("Equal point root IDs from different hosts compile distinct projections",
          "[shapepoint][projection]")
{
    auto map = std::make_shared<MapSchema>();
    auto layer = map->newLayer("Hosts", LayerType::Features);
    auto integer = map->schemaType(map->primitive(simfil::SchemaType::Kind::Int));
    for (auto [name, member] : {std::pair{"Alpha", "onlyAlpha"}, {"Bravo", "onlyBravo"}}) {
        (void)layer->newFeature({name, {{{"key", {}, IdPartDataType::U32}}}});
        auto attr = map->newAttribute(name);
        attr.addField(member, integer);
        layer->bindAttribute(attr, name, "profiles", "PROFILE");
    }
    map->finalize();
    auto alpha = layer->shapePointQueryType("Alpha");
    auto bravo = layer->shapePointQueryType("Bravo");
    REQUIRE(alpha);
    REQUIRE(bravo);
    REQUIRE(alpha.pool() != bravo.pool());
    REQUIRE(alpha.id() == bravo.id());
    DataSourceInfo info;
    info.layers_["Hosts"] = std::make_shared<LayerInfo>();
    info.layers_["Hosts"]->layerId_ = "Hosts";
    info.setMapSchema(map);
    auto tile = std::make_shared<PartitionFeatureLayer>(
        TileId::fromTileXY(1, 0, 1), "hosts", "Map", info.getLayer("Hosts"),
        std::make_shared<StringPool>("hosts"));
    for (auto [name, member] : {std::pair{"Alpha", "onlyAlpha"}, {"Bravo", "onlyBravo"}}) {
        auto host = tile->newFeature(name, {{"key", int64_t{1}}});
        host->addLine({Point{1, 2}, Point{2, 2}});
        auto attr = host->attributeLayers()->newLayer("profiles")->newAttribute("PROFILE");
        attr->validity()->newComplete();
        REQUIRE(attr->addField(member, int64_t{42}));
    }
    FeatureLayerFilterRequest request;
    auto& channel = request.channels_.emplace_back();
    channel.channelId_ = "members";
    channel.scope_ = FeatureLayerFilterScope::ShapePoint;
    channel.entryFilter_ = "$pointIndex == 0";
    // A standalone name needs recursive schema shorthand on its own host only.
    channel.entryFields_ = {"onlyAlpha", "onlyBravo"};
    auto result = request.filter(*tile);
    REQUIRE(result);
    REQUIRE((*result)->issues().empty());
    auto output = (*result)->at(0);
    REQUIRE(output->shapePointEntryCount() == 2);
    CHECK(output->shapePointEntryAt(0)->values()->toJson() == Json::array({{42}, {nullptr}}));
    CHECK(output->shapePointEntryAt(1)->values()->toJson() == Json::array({{nullptr}, {42}}));

    // Across independent readers the verified source + host identity, not an
    // allocation address or a caller-supplied schema name, authorizes reuse.
    SimfilExpressionCache cache;
    auto env = makeEnvironment(tile->strings());
    auto program = [&](simfil::SchemaType root, LayerSchema::Ptr owner) {
        simfil::CompileOptions options{
            .any = false, .rewriteMode = simfil::RewriteMode::Schema, .rootSchema = root};
        return cache.getOrCompile("onlyAlpha", options, owner, {}, [&] {
            return simfil::compile(*env, "onlyAlpha", options);
        });
    };
    auto first = program(alpha, layer);
    auto second = program(bravo, layer);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*first != *second);
    std::stringstream wire;
    REQUIRE(map->write(wire));
    auto bytes = wire.str();
    auto other = MapSchema::read(std::vector<uint8_t>(bytes.begin(), bytes.end()))->layer("Hosts");
    auto sameHost = other->shapePointQueryType("Alpha");
    REQUIRE(sameHost.pool() != alpha.pool());
    auto hit = program(sameHost, other);
    REQUIRE(hit);
    CHECK(*hit == *first);
    auto spoof = std::make_shared<simfil::SchemaPool>();
    while (spoof->typeCount() < alpha.id())
        (void)spoof->newObjectType();
    spoof->finalize(std::string(alpha.pool()->identity()));
    auto untrusted = spoof->schemaType(alpha.id());
    REQUIRE(program(untrusted, layer));
    REQUIRE(program(untrusted, layer));
    auto stats = cache.statistics();
    CHECK(stats.entries == 2);
    CHECK(stats.hits == 1);
    CHECK(stats.compiles == 4);
}

TEST_CASE("Cached point enum programs share live readers without pinning evicted pools",
          "[shapepoint][expression-cache]")
{
    auto map = std::make_shared<MapSchema>();
    auto layer = map->newLayer("Hosts", LayerType::Features);
    for (auto host : {"Alpha", "Bravo", "Charlie"})
        (void)layer->newFeature({host, {{{"key", {}, IdPartDataType::U32}}}});
    auto state = map->newEnumType();
    state.addSymbol("TRACK_IDLE");
    auto extra = map->newObjectType();
    extra.setOpen(true);
    auto attr = map->newAttribute("Status");
    attr.addField("state", state);
    attr.addField("extra", extra);
    layer->bindAttribute(attr, "Alpha", "profiles", "STATUS");
    map->finalize();

    SimfilExpressionCache cache(1);
    auto env = makeEnvironment(std::make_shared<StringPool>("point-cache"));
    auto get = [&](simfil::SchemaType root, std::string_view query) {
        simfil::CompileOptions opts{
            .any = false, .rewriteMode = simfil::RewriteMode::Schema, .rootSchema = root};
        return cache.getOrCompile(query, opts, layer, {}, [&] {
            return simfil::compile(*env, query, opts);
        });
    };
    auto evictWarmAlpha = [&] {
        REQUIRE(layer->shapePointQueryType("Bravo"));
        REQUIRE(layer->shapePointQueryType("Charlie"));
    };
    auto root = layer->shapePointQueryType("Alpha");
    REQUIRE(root);
    auto pool = root.pool()->weak_from_this();
    bool complete = true;
    REQUIRE_FALSE(root.enumSymbolPaths(root.pool()->dictionary().get("TRACK_IDLE"), &complete).empty());
    REQUIRE_FALSE(complete);  // This shorthand requires a schema-bound runtime predicate.
    auto first = get(root, "TRACK_IDLE");
    auto second = get(root, "TRACK_IDLE");
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(*first == *second);
    CHECK(cache.statistics().hits == 1);
    root = {};
    evictWarmAlpha();
    first->reset();
    REQUIRE_FALSE(pool.expired());  // Only the remaining reader's AST keeps this pool alive.
    second->reset();
    REQUIRE(pool.expired());        // The cache and the source are both still live.

    auto rebuilt = layer->shapePointQueryType("Alpha");
    REQUIRE(get(rebuilt, "TRACK_IDLE"));
    CHECK(cache.statistics().compiles == 2);
    SECTION("A stale private slot leaves room for a strong canonical program") {
        auto canonical = map->schemaType(layer->featureSchema("Alpha"));
        auto program = get(canonical, "true");
        REQUIRE(program);
        std::weak_ptr<simfil::AST const> witness = *program;
        program->reset();
        REQUIRE_FALSE(witness.expired());
        auto again = get(canonical, "true");
        REQUIRE(again);
        CHECK(*again == witness.lock());
        CHECK(cache.statistics().compiles == 3);
    }
    SECTION("A cached point compilation error survives pool eviction") {
        auto failure = get(rebuilt, "(");
        REQUIRE_FALSE(failure);
        auto witness = rebuilt.pool()->weak_from_this();
        rebuilt = {};
        evictWarmAlpha();
        REQUIRE(witness.expired());
        auto again = get(layer->shapePointQueryType("Alpha"), "(");
        REQUIRE_FALSE(again);
        CHECK(again.error() == failure.error());
        CHECK(cache.statistics().failedCompiles == 1);
        CHECK(cache.statistics().compiles == 3);
    }
    CHECK(cache.statistics().entries == 1);
}

TEST_CASE("A native V7 feature scans original vertices without a typed point root",
          "[shapepoint][legacy]")
{
    // Authored once using the V7 writer: a features layer, not a reference-only identity.
    std::ifstream file(MAPGET_TEST_DATA_DIR "/mapschema-v7-feature.bin", std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    REQUIRE(bytes.at(4) == 7);
    auto map = MapSchema::read(bytes);
    auto layer = map->layer("Paths");
    REQUIRE(layer->producesFeatureType("Path"));
    REQUIRE_FALSE(layer->shapePointQueryType("Path"));
    DataSourceInfo info;
    info.layers_["Paths"] = std::make_shared<LayerInfo>();
    info.layers_["Paths"]->layerId_ = "Paths";
    info.setMapSchema(map);
    auto tile = std::make_shared<PartitionFeatureLayer>(
        TileId::fromTileXY(1, 0, 1), "v7", "V7", info.getLayer("Paths"),
        std::make_shared<StringPool>("v7"));
    auto host = tile->newFeature("Path", {{"id", int64_t{1}}});
    host->addLine({Point{0, 0}, Point{1, 2}});
    auto root = host->shapePoints(0).at(1)->queryContext();
    CHECK_FALSE(root->schemaType());
    FeatureLayerFilterRequest request;
    auto& channel = request.channels_.emplace_back();
    channel.channelId_ = "untyped";
    channel.scope_ = FeatureLayerFilterScope::ShapePoint;
    channel.entryFilter_ = "$pointIndex == 1";
    channel.entryFields_ = {"$pointIndex", "$geometry.type"};
    auto result = request.filter(*tile);
    REQUIRE(result);
    REQUIRE((*result)->issues().empty());
    REQUIRE((*result)->at(0)->shapePointEntryCount() == 1);
    auto row = (*result)->at(0)->shapePointEntryAt(0);
    CHECK(row->position() == host->shapePoints(0).at(1)->position());
    CHECK(row->values()->toJson() == Json::array({{1}, {"LineString"}}));
}

TEST_CASE("Signed vertex measures select stations independently of XY length", "[shapepoint]")
{
    auto s = Profile::declaration(Kind::VertexMeasure);
    s.position_.vertexMeasures_ = {"measures"};
    s.sampling_ = PointSeries::Sampling::Linear;
    // All three vertices coincide in XY. Only the producer's M axis distinguishes them.
    Profile f(s, {{1, 1}, {1, 1}, {1, 1}});
    f.attr->validity()->newComplete();
    f.record(-8, 0, 8);
    f.record(4, 0, 56);
    SECTION("Increasing and decreasing stations")
    {
        bool decreasing = GENERATE(false, true);
        CAPTURE(decreasing);
        auto measures = decreasing ?
            f.numbers<double>({4, -5, -8}) :
            f.numbers<double>({-8, -5, 4});
        REQUIRE(f.attr->addField("measures", measures));
        CHECK(f.reading(0) == (decreasing ? 56 : 8));
        CHECK(f.reading(2) == (decreasing ? 8 : 56));
        CHECK(f.reading(1) == 20.);
        auto candidates = f.samples(1);
        REQUIRE(candidates.size() == 1);
        auto const& contributors = candidates.at(0).at("contributors");
        REQUIRE(contributors.size() == 2);
        CHECK(contributors[0]["weight"] == .75);
        CHECK(contributors[1]["weight"] == .25);
    }
    SECTION("A missing, short, repeated, nonmonotone or nonfinite axis is not a fallback")
    {
        REQUIRE_THROWS_AS(f.reading(1), PointQueryError);  // No measure array.
        auto malformed = GENERATE(values<std::vector<double>>(
            {{},
             {-8, 4},
             {-8, -8, 4},
             {-8, 4, -5},
             {-8, std::numeric_limits<double>::infinity(), 4}}));
        CAPTURE(malformed);
        REQUIRE(f.attr->addField("measures", f.numbers(malformed)));
        REQUIRE_THROWS_AS(f.reading(1), PointQueryError);
    }
}

TEST_CASE(
    "Parallel columns preserve record alignment and exact duplicate candidates",
    "[shapepoint]")
{
    auto s = Profile::declaration(Kind::VertexIndex);
    s.parallel_ = true;
    Profile f(s);
    f.attr->validity()->newComplete();
    REQUIRE(f.columns->addField("pos", f.numbers<int64_t>({2, 1, 1})));
    SECTION("Pair values by original row, not by sorted vertex ordinal")
    {
        REQUIRE(f.columns->addField("raw", f.numbers<int64_t>({30, 10, 20})));
        CHECK(f.samples(0).empty());
        CHECK(f.reading(2) == 30);
        REQUIRE_THROWS_AS(f.reading(1), PointQueryError);
        auto candidates = f.samples(1);
        REQUIRE(candidates.size() == 2);
        for (int i = 0; i < 2; ++i) {
            int raw = 10 * (i + 1);
            CHECK(candidates[i]["value"] == raw);
            Json contributor{
                {"recordIndex", i + 1},
                {"weight", 1.},
                {"raw", raw},
                {"path", {"rows", "raw", i + 1}}};
            CHECK(candidates[i]["contributors"] == Json::array({contributor}));
        }
    }
    SECTION("No silent truncation or missing columns")
    {
        REQUIRE_THROWS_AS(f.samples(2), PointQueryError);
        REQUIRE(f.columns->addField("raw", f.numbers<int64_t>({30, 10})));
        REQUIRE_THROWS_AS(f.samples(2), PointQueryError);
    }
}

TEST_CASE(
    "Referenced profiles select a host-local record list, never the first branch",
    "[shapepoint]")
{
    auto s = Profile::declaration(Kind::VertexIndex);
    s.omittedValidityCovers_ = true;
    s.branches_ = {"branches"};
    s.branchTarget_ = {"target"};
    s.branchRecords_ = {"local"};
    s.hostTarget_ = {"key"};
    Profile f(s);  // The canonical host's key is 1.
    f.record(1, 0, 11);
    auto branches = f.tile->newArray();
    REQUIRE(f.attr->addField("branches", branches));
    auto addBranch = [&](int64_t key, std::optional<int> reading)
    {
        auto branch = f.tile->newObject();
        auto local = f.tile->newArray();
        if (reading)
            local->append(f.row(1, 0, *reading));
        REQUIRE(branch->addField("target", key));
        REQUIRE(branch->addField("local", local));
        branches->append(branch);
    };
    addBranch(2, 22);
    SECTION("No target match uses the primary; a mismatched pin is an error")
    {
        CHECK(f.reading(1) == 11);
        CHECK(f.samples(1).at(0)["contributors"][0]["path"] == Json::array({"rows", 0, "raw"}));
        auto other = f.tile->newFeature("Path", {{"key", int64_t{2}}});
        other->addGeometry(f.host->shapePoints(0).geometry());
        other->attributeLayers()->newLayer("profiles")->addAttribute(f.attr);
        auto referenced = f.occurrences(1, other);
        REQUIRE(referenced->size() == 1);
        CHECK(f.field(referenced->at(0), "reading")->toJson() == 22);
        auto explanation = f.field(f.field(referenced->at(0), "$samples"), "reading")->toJson();
        CHECK(explanation.at(0)["alternative"] == 2);
        f.attr->setPointSeriesBranch(0);
        REQUIRE_THROWS_AS(f.reading(1), PointQueryError);
    }
    SECTION("Multiple matches require a pin and retain its full source path")
    {
        addBranch(1, 33);
        CHECK(f.reading(1) == 33);
        addBranch(1, 44);
        REQUIRE_THROWS_AS(f.reading(1), PointQueryError);
        f.attr->setPointSeriesBranch(2);
        CHECK(f.reading(1) == 44);
        auto candidate = f.samples(1).at(0);
        CHECK(candidate["contributors"][0]["raw"] == 44);
        CHECK(
            candidate["contributors"][0]["path"] ==
            Json::array({"branches", 2, "local", 0, "raw"}));
    }
    SECTION("An explicitly empty selected branch cannot fall back to primary records")
    {
        addBranch(1, {});
        CHECK(f.samples(1).empty());
        CHECK(f.reading(1).is_null());
    }
    SECTION("A matching referenced branch cannot widen explicitly empty applicability")
    {
        addBranch(1, 33);
        (void)f.attr->validity();
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(f.occurrences(i)->size() == 0);
    }
}

TEST_CASE(
    "Logical attribute positions select only original vertices in the assignment",
    "[shapepoint]")
{
    Profile f(Profile::declaration(Kind::LogicalIndex));
    auto points = f.host->shapePoints(0);
    auto sequence = f.tile->newAttrPointSequence(f.host, points.geometry());
    // The inserted point even has the same XY as B; its logical identity is still distinct.
    sequence->appendAttrPoint(1, points.geometry()->pointAt(1));  // A, inserted, B, C.
    f.attr->validity()->newAttrPointIndexRange(sequence, 1, 3);
    f.record(1, 0, 99);
    f.record(2, 0, 42);
    REQUIRE(points.size() == 3);
    CHECK(f.occurrences(0)->size() == 0);
    CHECK(f.reading(1) == 42);
    CHECK(f.samples(1).at(0)["contributors"][0]["recordIndex"] == 1);
    CHECK(f.reading(2).is_null());
    auto other = f.tile->newFeature("Path", {{"key", int64_t{2}}});
    other->addGeometry(points.geometry());
    other->attributeLayers()->newLayer("profiles")->addAttribute(f.attr);
    CHECK(f.occurrences(1, other)->size() == 0);  // The logical sequence targets the first host.
    f.record(4, 0, 17);
    REQUIRE_THROWS_AS(f.reading(1), PointQueryError);
}

TEST_CASE("Indexed anchors interpolate by path measure, not ordinal difference", "[shapepoint]")
{
    for (Kind kind : {Kind::VertexIndex, Kind::LogicalIndex}) {
        CAPTURE(kind);
        auto s = Profile::declaration(kind);
        s.sampling_ = PointSeries::Sampling::Linear;
        Profile f(s, {{0, 0}, {1, 0}, {10, 0}});
        int end = 2;
        if (kind == Kind::LogicalIndex) {
            auto sequence = f.tile->newAttrPointSequence(f.host, f.host->shapePoints(0).geometry());
            sequence->appendAttrPoint(1, {.25, 0});
            end = 3;
            f.attr->validity()->newAttrPointIndexRange(sequence, 0, end);
        }
        else
            f.attr->validity()->newComplete();
        f.record(0, 0, 0);
        f.record(end, 0, 100);
        CHECK(f.reading(1).get<double>() == Catch::Approx(10.));
        auto contributors = f.samples(1).at(0).at("contributors");
        REQUIRE(contributors.size() == 2);
        CHECK(contributors[0]["weight"].get<double>() == Catch::Approx(.9));
        CHECK(contributors[1]["weight"].get<double>() == Catch::Approx(.1));
    }
}

TEST_CASE("Only ordinary typed raw values interpolate in physical units", "[shapepoint]")
{
    auto s = Profile::declaration(Kind::VertexIndex);
    s.fields_[0].unit_.clear();  // Inherit both the transform and the unit from raw.
    s.sampling_ = PointSeries::Sampling::Linear;
    Profile f(
        s,
        {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}},
        [](simfil::SchemaScalarType raw)
        {
            simfil::SchemaType::NumericRange const ordinary{int64_t{-4}, int64_t{12}};
            raw.setNumericDomain(
                {.unit = "m/s",
                 .minimum = int64_t{-8},
                 .maximum = int64_t{31},
                 .ordinaryRanges = std::span(&ordinary, 1),
                 .scale = .5,
                 .offset = -2.});
            raw.addNamedValue({.name = "STILL", .value = 0., .rawValue = int64_t{4}});
            raw.addNamedValue(
                {.name = "NO_READING",
                 .value = int64_t{-1},
                 .role = simfil::SchemaType::NamedValue::Role::Special,
                 .meaning = simfil::SchemaType::NamedValue::Meaning::Unknown,
                 .rawValue = int64_t{31}});
        });
    f.attr->validity()->newComplete();
    f.record(0, 0, 4);
    SECTION("Named ordinary codes and unlabelled codes both contribute")
    {
        f.record(4, 0, 12);
        CHECK(f.reading(0) == 0.);
        CHECK(f.reading(1) == 1.);
        CHECK(f.reading(4) == 4.);
        auto candidate = f.samples(1).at(0);
        CHECK(candidate["unit"] == "m/s");
        CHECK_FALSE(candidate.contains("status"));
        auto const& contributors = candidate.at("contributors");
        REQUIRE(contributors.size() == 2);
        CHECK(contributors[0]["raw"].is_number_integer());
        CHECK(contributors[0]["raw"] == 4);
        CHECK(contributors[1]["raw"] == 12);
        CHECK(contributors[0]["weight"] == .75);
        CHECK(contributors[1]["weight"] == .25);
    }
    SECTION("A special or out-of-domain code is inspectable but never an interpolation anchor")
    {
        int code = GENERATE(31, 20);
        CAPTURE(code);
        f.record(2, 0, code);
        f.record(4, 0, 12);
        CHECK(f.reading(2).is_null());
        auto candidate = f.samples(2).at(0);
        CHECK(candidate.at("value").is_null());
        CHECK(candidate["unit"] == "m/s");
        REQUIRE(candidate.at("status").is_string());
        if (code == 31)
            CHECK(candidate["status"] == "NO_READING");
        CHECK(candidate["contributors"][0]["raw"] == code);
        for (auto vertex : {1u, 3u}) {
            CHECK(f.samples(vertex).empty());
            CHECK(f.reading(vertex).is_null());
        }
    }
}

TEST_CASE(
    "A wide raw integer adjacent to a sentinel keeps its value and provenance",
    "[shapepoint]")
{
    static constexpr int64_t missing = int64_t{1} << 53;
    Profile f(
        Profile::declaration(Kind::VertexIndex),
        {{0, 0}, {1, 0}, {2, 0}},
        [](simfil::SchemaScalarType raw)
        {
            simfil::SchemaType::NumericRange const ordinary{uint64_t{missing + 1}, UINT64_MAX};
            raw.setNumericDomain(
                {.encoding = "uint64",
                 .minimum = uint64_t{0},
                 .maximum = UINT64_MAX,
                 .ordinaryRanges = std::span(&ordinary, 1)});
            raw.addNamedValue(
                {.name = "MISSING",
                 .value = int64_t{-1},
                 .role = simfil::SchemaType::NamedValue::Role::Special,
                 .rawValue = uint64_t{missing}});
        });
    f.attr->validity()->newComplete();
    f.record(0, 0, missing);
    f.record(1, 0, missing + 1);  // This has the same double representation as missing.
    f.record(2, 0, missing - 1);
    CHECK(f.samples(0).at(0)["status"] == "MISSING");
    CHECK(f.reading(0).is_null());
    auto ordinary = f.reading(1);
    REQUIRE(ordinary.is_number_integer());
    CHECK(ordinary.get<int64_t>() == missing + 1);
    CHECK(f.samples(1).at(0)["contributors"][0]["raw"].get<int64_t>() == missing + 1);
    CHECK(f.reading(2).is_null());
}

TEST_CASE("Assignment-local fractions use the selected span in either direction", "[shapepoint]")
{
    for (bool reverse : {false, true}) {
        CAPTURE(reverse);
        auto s = Profile::declaration();
        s.position_.fromAssignmentStart_ = true;
        s.position_.reverse_ = reverse;
        Profile f(s, {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}});
        f.attr->validity()->newRange(Validity::BufferOffset, 1, 5);
        f.record(0, 1, 10);
        f.record(1, 1, 50);
        CHECK(f.occurrences(0)->size() == 0);
        CHECK(f.occurrences(6)->size() == 0);
        CHECK(f.reading(1) == (reverse ? 50 : 10));
        CHECK(f.reading(5) == (reverse ? 10 : 50));
        CHECK(f.reading(2).get<double>() == Catch::Approx(reverse ? 40. : 20.));
    }
}

TEST_CASE(
    "Fraction order survives rounding collisions and overflowing cross products",
    "[shapepoint]")
{
    using Fraction = shape::RecordIndexes::Fraction;
    auto order = [](Fraction a, Fraction b, int sign)
    {
        CAPTURE(a.numerator, a.denominator, b.numerator, b.denominator);
        CHECK(std::clamp(a.compare(b), -1, 1) == sign);
        CHECK(std::clamp(b.compare(a), -1, 1) == -sign);
    };
    constexpr auto wide = (uint64_t{1} << 63) - 1, narrower = (uint64_t{1} << 62) - 1;
    order({wide / 7, wide}, {1, 7}, 0);
    order({wide - 1, wide}, {wide - 2, wide}, 1);
    order({wide - 1, wide}, {narrower - 1, narrower}, 1);
    order({0, wide}, {0, narrower}, 0);
    // Adjacent Fibonacci ratios: the two mathematical cross products differ by exactly one.
    constexpr uint64_t f90 = 2880067194370816120, f91 = 4660046610375530309, f92 = f90 + f91;
    order({f90, f91}, {f91, f92}, -1);
    // Both neighbours round to the same double at these original-vertex stations.
    CHECK((Fraction{wide - 1, wide}.compare(1.) < 0));
    CHECK((Fraction{wide, wide}.compare(1.) == 0));
    CHECK((Fraction{wide / 2, wide}.compare(.5) < 0));
    CHECK((Fraction{wide / 2 + 1, wide}.compare(.5) > 0));
}
}  // namespace
