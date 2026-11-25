#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mapget/model/featurelayer.h"
#include "mapget/model/mapschema.h"

#include <sstream>

namespace
{
using namespace mapget;
using Json = nlohmann::json;
constexpr std::string_view SPEED_TAG = "drivepath-speed-point";
constexpr std::string_view ADAS_TAG = "adas-measurement-point";
using Node = simfil::ModelNode::Ptr;

// The drivepath_1 fixture's validity prefix is an interior assignment; the
// two speed records are at 0 and 1. Both declarations use the same tagged public view.
struct Profile
{
    MapSchema::Ptr map = std::make_shared<MapSchema>();
    LayerSchema::Ptr schema = map->newLayer("Paths", LayerType::Features);
    std::shared_ptr<StringPool> strings = std::make_shared<StringPool>("marker-tests");
    std::shared_ptr<LayerInfo> info = std::make_shared<LayerInfo>();
    PartitionFeatureLayer::Ptr tile;
    model_ptr<Feature> host;
    model_ptr<AttributeLayer> layer;
    model_ptr<Attribute> attr;
    model_ptr<Array> records;
    model_ptr<Geometry> line;

    explicit Profile(
        std::string_view markerTag,
        bool branched = false,
        PointSeries::Position::Kind kind = PointSeries::Position::Kind::Fraction)
    {
        (void)schema->newFeature({"Path", {{{"key", {}, IdPartDataType::U32}}}});
        auto integer = map->schemaType(map->primitive(simfil::SchemaType::Kind::Int));
        auto row = map->newObjectType();
        for (auto key : {"position", "bits", "speed"})
            row.addField(key, integer);
        auto rows = map->schemaType(map->arrayOf(row.id()));
        auto payload = map->newObjectType();
        payload.addField("records", rows);
        auto attribute = map->newAttribute("Profile");
        attribute.addField("attributeValue", payload);
        PointSeries series;
        series.featureType_ = "Path";
        series.attributeLayer_ = "profiles";
        series.attribute_ = "SPEED";
        if (markerTag == ADAS_TAG)
            series.geometryName_ = "ADAS";
        series.records_ = {"attributeValue", "records"};
        series.position_.kind_ = kind;
        series.position_.field_ = {"position"};
        if (kind == PointSeries::Position::Kind::Fraction)
            series.position_.bits_ = {"bits"};
        series.measure_ = PointSeries::Measure::Wgs84Planar2D;
        series.fields_ = {{"normalSpeed", {"speed"}, "km/h", {}}};
        series.markerTag_ = markerTag;
        if (branched) {
            auto branch = map->newObjectType();
            branch.addField("target", integer);
            branch.addField("rows", rows);
            attribute.addField("branches", map->schemaType(map->arrayOf(branch.id())));
            series.branches_ = {"branches"};
            series.branchTarget_ = {"target"};
            series.branchRecords_ = {"rows"};
            series.hostTarget_ = {"key"};
        }
        schema->bindAttribute(attribute, "Path", "profiles", "SPEED");
        schema->addPointSeries(std::move(series));
        map->finalize();
        info->layerId_ = "Paths";
        DataSourceInfo catalog;
        catalog.layers_.emplace("Paths", info);
        catalog.setMapSchema(map);
        info = catalog.getLayer("Paths");
        tile = std::make_shared<PartitionFeatureLayer>(
            TileId::fromValue(545555028),
            "marker-tests",
            "Map",
            info,
            strings);
        tile->setGeometryAnchor({10, 70});
        host = tile->newFeature("Path", {{"key", int64_t{1}}});
        line = host->geom()->newGeometry(GeomType::Line);
        if (markerTag == ADAS_TAG)
            line->setName("ADAS");
        for (Point p : {Point{10, 70, 0}, Point{11, 70, 10}, Point{11, 71, 20}})
            line->append(p);
        layer = host->attributeLayers()->newLayer("profiles");
        layer->setId(37);
        attr = layer->newAttribute("SPEED");
        auto value = tile->newObject();
        records = tile->newArray();
        REQUIRE(value->addField("records", records));
        REQUIRE(attr->addField("attributeValue", value));
    }

    void add(int position, int bits, int speed, model_ptr<Array> rows = {})
    {
        auto row = tile->newObject();
        REQUIRE(row->addField("position", int64_t(position)));
        REQUIRE(row->addField("bits", int64_t(bits)));
        REQUIRE(row->addField("speed", int64_t(speed)));
        (rows ? rows : records)->append(row);
    }

    Node publicAttribute(model_ptr<Feature> const& feature) const
    {
        return Node(feature)
            ->get(StringPool::PropertiesStr)
            ->get(StringPool::LayerStr)
            ->get(strings->get("profiles"))
            ->get(strings->get("SPEED"));
    }

    std::string bytes() const
    {
        std::ostringstream stream;
        REQUIRE(tile->write(stream).has_value());
        return stream.str();
    }
};

TEST_CASE(
    "DrivePath public markers index raw anchors, never assignment coverage",
    "[attribute-markers]")
{
    Profile f(SPEED_TAG);
    f.attr->validity()->newRange(Validity::RelativeLengthOffset, 20. / 127., 40. / 127.);
    f.add(0, 1, 80);
    f.add(1, 1, 60);
    QualifiedSourceDataReference source{
        .address_ = SourceDataAddress::fromBitPosition(24, 32),
        .layerId_ = f.strings->emplace("SourceProfile").value(),
        .qualifier_ = f.strings->emplace("ProfileValue").value()};
    auto refs = f.tile->newSourceDataReferenceCollection({&source, 1});
    f.attr->setSourceDataReferences(refs);
    auto before = f.bytes();
    auto view = f.attr->validityView(f.host, "profiles");
    REQUIRE(view.size() == 3);
    REQUIRE(f.attr->schemaType());
    CHECK(f.publicAttribute(f.host)->schemaType() == f.attr->schemaType());
    REQUIRE_FALSE(view.at(3));
    REQUIRE_FALSE(view.markerSamples(0));
    REQUIRE_FALSE(view.markerSample(2, "absent"));
    REQUIRE(view.at(0)->offsetRange());
    Json expected = Json::array(
        {{{"start", 20. / 127.}, {"end", 40. / 127.}, {"offsetType", "RelativeLengthOffset"}},
         {{"tag", SPEED_TAG}, {"point", 0.}, {"offsetType", "RelativeLengthOffset"}},
         {{"tag", SPEED_TAG}, {"point", 1.}, {"offsetType", "RelativeLengthOffset"}}});
    CHECK(view.node()->toJson()["validity"] == expected);
    CHECK(f.publicAttribute(f.host)->toJson() == view.node()->toJson());
    CHECK(f.host->toJson()["properties"]["layer"]["profiles"]["SPEED"] == view.node()->toJson());
    REQUIRE(f.attr->validityOrNull()->size() == 1);
    REQUIRE(view.at(1)->direction() == Validity::Empty);
    for (uint32_t i : {1u, 2u}) {
        auto sample = view.markerSample(i, "normalSpeed");
        REQUIRE(sample->get(StringPool::SourceDataStr));
        CHECK(sample->get(StringPool::SourceDataStr)->toJson() == refs->toJson());
        auto candidate = sample->toJson();
        CHECK_FALSE(candidate.contains("_sourceData"));
        int raw = i == 1 ? 80 : 60;
        CHECK(candidate["value"] == raw);
        CHECK(candidate["unit"] == "km/h");
        CHECK(
            candidate["contributors"] ==
            Json::array(
                {{{"recordIndex", i - 1},
                  {"weight", 1.},
                  {"raw", raw},
                  {"path", Json::array({"attributeValue", "records", i - 1, "speed"})}}}));
        auto context = view.queryContext(9, i);
        CHECK(
            context->get(StringPool::OverlaySamplesStr)->toJson()["normalSpeed"] ==
            Json::array({candidate}));
        CHECK(context->get(StringPool::OverlayValidityCountStr)->toJson() == 3);
        CHECK_FALSE(context->toJson().contains("$samples"));
        Node sliced = view.at(i);
        CHECK(sliced->toJson() == expected[i]);
        auto typed = f.tile->resolve<Validity>(*sliced);
        REQUIRE(typed->offsetPoint());
        auto geometry = typed->computeGeometry(f.host->geomOrNull());
        REQUIRE(geometry.points_.size() == 1);
        CHECK(geometry.points_[0] == f.line->pointAt(i == 1 ? 0 : 2));
        REQUIRE_THROWS(typed->setDirection(Validity::Negative));
    }
    for (uint32_t vertex : {0u, 1u, 2u}) {
        auto context = f.host->shapePoints(0).at(vertex)->queryContext();
        CHECK(context->get(StringPool::OverlayAttributesStr)->size() == 0);
    }
    CHECK(before == f.bytes());  // No persisted validity, station or marker array was added.
    auto clone = std::make_shared<
        PartitionFeatureLayer>(f.tile->tileId(), "marker-tests", "Map", f.info, f.strings);
    PartitionFeatureLayer::CloneCache cache;
    clone->clone(cache, f.tile, *f.host, "Path", {{"key", int64_t{1}}});
    CHECK(f.publicAttribute(clone->at(0))->toJson() == view.node()->toJson());
    auto reloaded = std::make_shared<PartitionFeatureLayer>(
        std::vector<uint8_t>(before.begin(), before.end()),
        [&](auto&&, auto&&) { return f.info; },
        [&](auto&&) { return f.strings; });
    CHECK(f.publicAttribute(reloaded->at(0))->toJson() == view.node()->toJson());
}

TEST_CASE(
    "Tagged percentage markers preserve duplicate stations without making an empty assignment "
    "apply",
    "[attribute-markers]")
{
    Profile f(ADAS_TAG);
    // Two identically named geometries: only the occurrence discriminator selects the second.
    auto selected = f.host->geom()->newGeometry(GeomType::Line);
    selected->setName("ADAS");
    selected->append({15, 70, 0});
    selected->append({16, 70, 10});
    selected->append({16, 71, 20});
    f.attr->setPointSeriesGeometry(1);
    (void)f.attr->validity();  // Explicit empty, not a whole-feature assignment.
    auto empty = f.attr->validityView(f.host, "profiles");
    CHECK(empty.node()->toJson()["validity"] == Json::array());

    for (auto [pos, speed] : {std::pair{0, 80}, {31, 71}, {31, 70}, {127, 60}})
        f.add(pos, 7, speed);
    auto view = f.attr->validityView(f.host, "profiles");
    REQUIRE(view.size() == 5);
    CHECK(view.at(0)->direction() == Validity::None);
    CHECK(view.at(0)->computeGeometry(f.host->geomOrNull()).points_.empty());
    CHECK_FALSE(view.at(5));
    CHECK_FALSE(view.markerSamples(0));
    auto fallback = view.queryContext(0, 0);
    auto json = f.publicAttribute(f.host)->toJson();
    REQUIRE_FALSE(json.contains("pointValidities"));
    CHECK_FALSE(json.contains("_mapgetPointSeriesGeometry"));
    REQUIRE(json["validity"].size() == 5);
    auto expected = Json{
        {"tag", ADAS_TAG},
        {"geometryName", "ADAS"},
        {"offsetType", "RelativeLengthOffset"},
        {"point", 31. / 127.}};
    CHECK(json["validity"][2] == expected);
    CHECK(json["validity"][3] == expected);
    Node sliced = fallback->get(StringPool::ValidityStr)->at(2);
    auto marker = f.tile->resolve<Validity>(*sliced);
    CHECK(marker->geometryName() == "ADAS");
    CHECK(marker->tag() == ADAS_TAG);
    CHECK(marker->direction() == Validity::Empty);
    CHECK(marker->geometryDescriptionType() == Validity::OffsetPointValidity);
    auto position = marker->computeGeometry(f.host->geomOrNull());
    REQUIRE(position.points_.size() == 1);
    // In planar WGS84 the two one-degree segments have equal length, unlike metres at 70N.
    CHECK(position.points_[0].x == Catch::Approx(15. + 2. * 31. / 127.));
    CHECK(position.points_[0].z == Catch::Approx(20. * 31. / 127.));
    CHECK(view.markerSample(2, "normalSpeed")->toJson()["value"] == 71);
    CHECK(view.markerSamples(3)->toJson()["normalSpeed"][0]["contributors"][0]["recordIndex"] == 2);
    CHECK(f.host->toJson()["properties"]["layer"]["profiles"]["SPEED"] == json);
}

TEST_CASE(
    "An inserted logical marker keeps its explicit off-segment coordinates",
    "[attribute-markers]")
{
    Profile f(SPEED_TAG, false, PointSeries::Position::Kind::LogicalIndex);
    auto sequence = f.tile->newAttrPointSequence(f.host, f.line);
    auto inserted = sequence->appendAttrPoint(1, {10.4, 70.7, 94});
    REQUIRE(inserted);
    f.attr->validity()->newAttrPointIndexRange(sequence, 0, sequence->positionCount() - 1);
    f.add(1, 0, 55);
    f.add(2, 0, 60);
    auto view = f.attr->validityView(f.host, "profiles");
    REQUIRE(view.size() == 3);
    Node sliced = view.at(1);
    auto typed = f.tile->resolve<Validity>(*sliced);
    auto result = typed->computeGeometry(f.host->geomOrNull());
    REQUIRE(result.points_.size() == 1);
    CHECK(result.points_[0] == sequence->pointAt(1));
    auto original = view.at(2)->computeGeometry(f.host->geomOrNull());
    REQUIRE(original.points_.size() == 1);
    CHECK(original.points_[0] == f.line->pointAt(1));
    CHECK(f.line->numPoints() == 3);
    CHECK(sliced->toJson().size() == 3);
}

TEST_CASE(
    "Public attribute traversal preserves attachment order and reads only requested anchors",
    "[attribute-markers]")
{
    Profile f(SPEED_TAG);
    f.add(1, 1, 60);
    f.add(0, 1, 80);
    SECTION("Multimaps retain ordinary layer IDs and unsorted source records")
    {
        REQUIRE(f.host->attributes()->addField("quality", int64_t{2}));
        REQUIRE(f.host->attributes()->addField("quality", int64_t{3}));
        f.layer->addAttribute(f.attr);
        auto other = f.host->attributeLayers()->newLayer("profiles");
        other->setId(42);
        other->newAttribute("CONDITION");
        auto unrelated = f.host->attributeLayers()->newLayer("other");
        unrelated->addAttribute(f.attr);  // Same physical data, no marker declaration here.
        auto plain = f.attr->validityView(f.host, "other");
        CHECK(plain.node()->borrowedModel() == f.attr->borrowedModel());
        CHECK(plain.node()->addr() == f.attr->addr());
        CHECK(plain.size() == 0);

        auto properties = f.host->toJson()["properties"];
        CHECK(properties["quality"] == Json::array({2, 3}));
        auto layers = properties.at("layer");
        REQUIRE(layers["_multimap"] == true);
        REQUIRE(layers["profiles"].size() == 2);
        auto const& first = layers["profiles"][0];
        CHECK(first["_multimap"] == true);
        CHECK(first["id"] == 37);
        REQUIRE(first["SPEED"].size() == 2);
        CHECK(first["SPEED"][0] == first["SPEED"][1]);
        CHECK(first["SPEED"][0]["validity"][0]["point"] == 1.);
        CHECK(first["SPEED"][0]["validity"][1]["point"] == 0.);
        CHECK(layers["profiles"][1]["id"] == 42);
        CHECK_FALSE(layers["other"]["SPEED"].contains("validity"));
    }
    SECTION("An invalid station does not poison the remaining raw records or stored traversal")
    {
        f.add(1, 0, 77);
        auto view = f.attr->validityView(f.host, "profiles");
        REQUIRE(view.size() == 3);
        CHECK(view.at(0)->offsetPoint()->x == 1.);
        CHECK(view.markerSample(1, "normalSpeed")->toJson()["value"] == 80);
        CHECK(
            f.publicAttribute(f.host)
                ->get(f.strings->get("attributeValue"))
                ->toJson()["records"]
                .size() == 3);
        CHECK_THROWS_AS(view.at(2), PointQueryError);
        CHECK_FALSE(
            f.host->attributeLayersOrNull()->toJson()["profiles"]["SPEED"].contains("validity"));
    }
    SECTION("An undeclared pointValidities is still an ordinary raw field")
    {
        auto rows = f.tile->newArray();
        rows->append(f.tile->newValue("raw payload"));
        REQUIRE(f.attr->addField("pointValidities", rows));
        auto view = f.attr->validityView(f.host, "profiles");
        CHECK(view.node()->toJson()["pointValidities"] == Json::array({"raw payload"}));
    }
    SECTION("A stored geometry selects its own markers among otherwise eligible lines")
    {
        auto selected = f.host->geom()->newGeometry(GeomType::Line);
        selected->append({17, 70, 0});
        selected->append({18, 70, 1});
        f.attr->validity()->newGeometry(selected);
        auto view = f.attr->validityView(f.host, "profiles");
        REQUIRE(view.size() == 3);
        auto endpoint = view.at(1)->computeGeometry(f.host->geomOrNull());
        REQUIRE(endpoint.points_.size() == 1);
        CHECK(endpoint.points_[0] == selected->pointAt(1));
    }
    SECTION("A raw feature query reports an ambiguous marker target without unwinding SIMFIL")
    {
        f.host->addLine({Point{17, 70}, Point{18, 70}});
        CHECK_THROWS_AS(f.publicAttribute(f.host), PointQueryError);
        // Cover both named lookup and wildcard iteration over the attached layer.
        for (auto query : {"properties.layer.profiles.SPEED.attributeValue.records",
                           "properties.layer.profiles.*.attributeValue.records"}) {
            auto result = f.tile->evaluate(query, *f.host, false, false);
            REQUIRE_FALSE(result);
            CHECK(result.error().type == simfil::Error::RuntimeError);
            CHECK(result.error().message.find("selects multiple geometries") != std::string::npos);
        }
        auto featureRead = f.host->evaluate("properties.layer.profiles.SPEED.attributeValue");
        REQUIRE_FALSE(featureRead);
        CHECK(featureRead.error().type == simfil::Error::RuntimeError);
        auto unrelated = f.tile->evaluate("properties.layer.profiles.id", *f.host, false, false);
        REQUIRE(unrelated);
        REQUIRE(unrelated->values.size() == 1);
        CHECK(unrelated->values.front().toString() == "37");
    }
}

TEST_CASE("Shared marker profiles retain the host and exact branch path", "[attribute-markers]")
{
    Profile f(SPEED_TAG, true);
    f.add(0, 1, 80);
    auto branchRows = f.tile->newArray();
    f.add(1, 1, 40, branchRows);
    auto branch = f.tile->newObject();
    REQUIRE(branch->addField("target", int64_t{2}));
    REQUIRE(branch->addField("rows", branchRows));
    auto branches = f.tile->newArray();
    branches->append(branch);
    REQUIRE(f.attr->addField("branches", branches));
    auto other = f.tile->newFeature("Path", {{"key", int64_t{2}}});
    other->addLine({Point{20, 70}, Point{21, 70}});
    other->attributeLayers()->addLayer("profiles", f.layer);
    auto first = f.attr->validityView(f.host, "profiles");
    CHECK(first.node()->toJson()["validity"]["point"] == 0.);
    CHECK(first.markerSample(0, "normalSpeed")->toJson()["value"] == 80);
    Node retained;
    {
        auto second = f.attr->validityView(other, "profiles");
        CHECK(second.size() == 1);
        CHECK(f.publicAttribute(other)->toJson()["validity"]["point"] == 1.);
        retained = second.queryContext(0, 0)->get(StringPool::OverlaySamplesStr);
    }
    auto result = retained->toJson()["normalSpeed"][0];
    CHECK(result["value"] == 40);
    CHECK(result["contributors"][0]["path"] == Json::array({"branches", 0, "rows", 0, "speed"}));
}
}  // namespace

TEST_CASE(
    "Property references identify slots, not tags, positions or shared assignments",
    "[attribute-markers][validity-reference]")
{
    static_assert(sizeof(ValidityData) == 64);
    Profile f(SPEED_TAG);
    auto other = f.layer->newAttribute("OTHER");
    auto assignments = f.tile->newValidityCollection(1, true);
    assignments->newDirection(Validity::Positive);
    f.attr->setValidity(assignments);
    other->setValidity(assignments);

    std::array<model_ptr<Validity>, 2> points;
    for (auto& point : points) {
        point = f.tile->newValidity();
        point->setOffsetPoint(Validity::BufferOffset, 1.);
        point->setTag("house-position");
    }
    auto first = f.attr->appendTaggedValidities(points);
    REQUIRE(first == 1);
    REQUIRE(other->validityOrNull()->size() == 1);
    CHECK(
        other->validityOrNull()->at(0)->addr().column() ==
        PartitionFeatureLayer::ColumnId::SimpleValidity);

    auto ref = f.tile->newValidityReference(f.attr, first + 1);
    auto properties = f.tile->newArray(2, true);
    properties->append(f.tile->newValidityReference(f.attr, first));
    properties->append(ref);
    REQUIRE(f.attr->addField("properties", properties));
    REQUIRE(ref->index() == 2);
    REQUIRE(ref->validity()->addr() == points[1]->addr());
    CHECK(ref->validity()->addr() != points[0]->addr());
    CHECK(ref->get(StringPool::TagStr)->toJson() == "house-position");
    CHECK(ref->toJson() == Json{{"$mapgetValidity", 2}});
    CHECK_THROWS(f.tile->newValidityReference(other, first));

    SECTION("clone rooted at the reference retains the owner and the exact shared child")
    {
        auto dst = std::make_shared<PartitionFeatureLayer>(
            f.tile->tileId(),
            "clone",
            "Map",
            f.info,
            std::make_shared<StringPool>("clone"));
        PartitionFeatureLayer::CloneCache cache;
        auto clonedRef = dst->resolve<ValidityReference>(*dst->clone(cache, f.tile, ref));
        auto clonedAttr = dst->resolve<Attribute>(*dst->clone(cache, f.tile, f.attr));
        auto slot = clonedAttr->validityOrNull()->at(2);
        REQUIRE(slot);
        CHECK(clonedRef->validity()->addr() == slot->addr());
        auto props = clonedAttr->get(dst->strings()->get("properties"));
        CHECK(props->at(1)->addr() == clonedRef->addr());
        CHECK(clonedRef->validity()->tag() == "house-position");
    }
    SECTION("binary re-read and dictionary replacement preserve tags and index")
    {
        f.add(1, 1, 80);
        auto bytes = f.bytes();
        auto copy = std::make_shared<PartitionFeatureLayer>(
            std::vector<uint8_t>(bytes.begin(), bytes.end()),
            [&](auto&&, auto&&) { return f.info; },
            [&](auto&&) { return f.strings; });
        auto dict = std::make_shared<StringPool>("replacement");
        REQUIRE(dict->emplace("distinct-first-key"));
        REQUIRE(copy->setStrings(dict));
        auto copyRef = copy->resolve<ValidityReference>(ref->addr());
        REQUIRE(copyRef->index() == 2);
        CHECK(copyRef->validity()->tag() == "house-position");
        CHECK(copyRef->get(StringPool::MapgetValidityStr)->toJson() == 2);
        auto view = copy->resolve<Attribute>(f.attr->addr())
                        ->validityView(copy->resolve<Feature>(f.host->addr()), "profiles");
        REQUIRE(view.at(3)->tag() == SPEED_TAG);
        CHECK(view.markerSamples(3)->get(dict->get("normalSpeed"))->at(0)->toJson()["value"] == 80);
    }
}

TEST_CASE(
    "A tagged value on a vertex does not make its attribute apply there",
    "[attribute-markers][validity-reference][shapepoint]")
{
    Profile f(SPEED_TAG);
    f.attr->validity()->newRange(Validity::BufferOffset, 0., 1.);
    auto valueLocation = f.tile->newValidity();
    valueLocation->setOffsetPoint(Validity::BufferOffset, 2.);
    valueLocation->setTag("house-position");
    std::array annotations{valueLocation};
    auto ordinal = f.attr->appendTaggedValidities(annotations);
    f.add(1, 1, 80);  // An actual measurement at the excluded endpoint, too.
    auto publicView = f.attr->validityView(f.host, "profiles");
    REQUIRE(publicView.size() == 3);
    CHECK(publicView.at(ordinal)->tag() == "house-position");
    CHECK(publicView.at(2)->tag() == SPEED_TAG);
    CHECK(publicView.markerSample(2, "normalSpeed")->toJson()["value"] == 80);
    auto vertex = f.host->shapePoints(0).at(2);
    REQUIRE(vertex);
    CHECK(vertex->queryContext()->get(StringPool::OverlayAttributesStr)->size() == 0);
}

TEST_CASE(
    "Tagging a compact directional validity upgrades only its owning slot",
    "[attribute-markers][validity-reference]")
{
    Profile f(SPEED_TAG);
    auto first = f.attr->validity()->newDirection(Validity::Positive);
    auto second = f.attr->validity()->newDirection(Validity::Positive);
    auto ref = f.tile->newValidityReference(f.attr, 0);
    first->setTag("");
    REQUIRE(first->addr().column() == PartitionFeatureLayer::ColumnId::SimpleValidity);
    first->setTag("value-location");
    REQUIRE(first->addr() == ref->validity()->addr());
    Node sliced = first;
    CHECK(sliced->get(StringPool::TagStr)->toJson() == "value-location");
    CHECK(second->addr().column() == PartitionFeatureLayer::ColumnId::SimpleValidity);
    CHECK_FALSE(second->tag());
    CHECK(ref->toJson() == Json{{"$mapgetValidity", 0}});
}
