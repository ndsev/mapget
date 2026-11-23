#include "attr.h"
#include "featurelayer.h"
#include "shapepoint-series.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>

namespace mapget
{
using shape::Axis;
using shape::Context;
using shape::SeriesReader;

namespace
{
model_ptr<Validity> legacyPoint(
    PartitionFeatureLayer const& layer,
    simfil::ModelNode::Ptr const& points,
    uint32_t index)
{
    if (!points || points->type() != simfil::ValueType::Array)
        return {};
    auto node = points->at(index);
    if (!node ||
        (node->addr().column() != PartitionFeatureLayer::ColumnId::Validities &&
         node->addr().column() != PartitionFeatureLayer::ColumnId::SimpleValidity))
        return {};  // An unrelated producer field may also be named pointValidities.
    return layer.resolve<Validity>(*node);
}
}  // namespace

struct AttributeValidityView::State
{
    using Node = simfil::ModelNode::Ptr;
    using Markers = PointSeries::Markers;

    struct Run
    {
        std::shared_ptr<Axis> axis;
        std::shared_ptr<SeriesReader> reader;
        uint32_t first;
        uint32_t count;

        Node sample(
            uint32_t index,
            PointSeries::Field const& field,
            model_ptr<SourceDataReferenceCollection> const& refs) const
        {
            auto candidate = reader->markerSample(index - first, field);
            if (!refs)
                return candidate;
            // This is the enclosing occurrence's source region, not a fabricated
            // byte range for the individual record. The public candidate JSON is unchanged.
            auto overlay = model_ptr<simfil::OverlayNode>::make(simfil::Value::field(*candidate));
            overlay->set(StringPool::SourceDataStr, simfil::Value::field(*refs));
            return overlay;
        }
    };

    struct Layout
    {
        std::vector<Run> runs;
        uint32_t size = 0;
        bool declared = false;

        Run const* find(uint32_t index) const
        {
            for (auto const& run : runs)
                if (index >= run.first && index - run.first < run.count)
                    return &run;
            return nullptr;
        }
    };

    State(
        model_ptr<Attribute> attr,
        model_ptr<Feature> host,
        LayerSchema::Ptr schema,
        std::span<PointSeries const> declarations)
        : attr(std::move(attr)),
          host(std::move(host)),
          stored(this->attr->validityOrNull()),
          schema(std::move(schema)),
          declarations(declarations)
    {
        separateDeclared = std::ranges::any_of(
            declarations,
            [](auto const& d) { return d.markers_ == Markers::PointValidities; });
        auto strings = this->attr->model().strings();
        pointKey = strings->get("pointValidities");
        if (pointKey)
            storedPoints = this->attr->get(pointKey);
    }

    model_ptr<Attribute> attr;
    model_ptr<Feature> host;
    model_ptr<MultiValidity> stored;
    LayerSchema::Ptr schema;  // Pins the readers' declarations, even after the view is copied.
    simfil::StringId pointKey;
    Node storedPoints;
    std::span<PointSeries const> declarations;
    bool separateDeclared;
    mutable std::array<std::once_flag, 2> prepared;
    mutable std::array<Layout, 2> layouts;

    Layout const& layout(bool separate) const
    {
        std::call_once(
            prepared[separate],
            [&]
            {
                Layout result;
                if (!separate && stored)
                    result.size = stored->size();
                auto destination = separate ? Markers::PointValidities : Markers::AppendToValidity;
                std::map<uint32_t, std::shared_ptr<Axis>> axes;
                for (auto const& decl : declarations) {
                    if (decl.markers_ != destination)
                        continue;
                    result.declared = true;
                    auto geometries = host->geomOrNull();
                    auto selected = attr->pointSeriesGeometry();
                    auto count = geometries ? geometries->mergedSize() : 0;
                    for (uint32_t g = 0; g < count; ++g) {
                        if (selected && g != *selected)
                            continue;
                        auto& axis = axes[g];
                        if (!axis)
                            axis = std::make_shared<Axis>(host, g, nullptr);
                        if (!axis->geometry_ || !axis->selects(decl, *attr))
                            continue;
                        auto reader = std::make_shared<SeriesReader>(axis, attr, decl, schema);
                        auto n = reader->markerCount();
                        if (n > std::numeric_limits<uint32_t>::max() - result.size)
                            throw PointQueryError("Too many point markers on one attribute.");
                        result.runs.push_back({axis, std::move(reader), result.size, n});
                        result.size += n;
                        break;
                    }
                }
                if (separate && !result.declared && storedPoints &&
                    storedPoints->type() == simfil::ValueType::Array)
                    result.size = storedPoints->size();
                layouts[separate] = std::move(result);
            });
        return layouts[separate];
    }

    model_ptr<Validity> at(uint32_t index, bool separate) const
    {
        auto const& list = layout(separate);
        if (index >= list.size)
            return {};
        auto run = list.find(index);
        if (!run) {
            if (separate)
                return legacyPoint(attr->model(), storedPoints, index);
            auto node = stored->at(index);
            return node ? attr->model().resolve<Validity>(*node) : model_ptr<Validity>{};
        }
        auto marker = run->reader->marker(index - run->first);
        if (!marker)
            return {};
        auto [type, offset] = *marker;
        auto const& axis = *run->axis;
        auto const& decl = run->reader->declaration();
        auto explicitPosition = run->reader->logicalMarkerPosition(index - run->first);
        auto measure = type == Validity::MetricLengthOffset ?
            PointSeries::Measure::GeographicMetres :
            decl.measure_;
        // Compute the fractional source vertex with the retained distance table.
        // Subsequent computeGeometry() on a sliced or retained node is O(1).
        double vertex = std::numeric_limits<double>::quiet_NaN();
        if (!explicitPosition && axis.geometry_->numPoints()) {
            if (type == Validity::BufferOffset)
                vertex = offset;
            else if (offset == 0.)
                vertex = 0.;
            else if (type == Validity::RelativeLengthOffset && offset == 1.)
                vertex = axis.last();
            else {
                double length = axis.distanceAt(axis.last(), measure);
                double distance = type == Validity::RelativeLengthOffset ?
                    offset * length :
                    std::clamp(offset, 0., length);
                if (auto v = axis.vertexAt(distance, measure))
                    vertex = *v;
            }
        }
        return Validity::view(
            axis.geometry_,
            axis.ordinal_,
            type,
            offset,
            vertex,
            separate ? Validity::Both : Validity::Empty,
            decl.geometryName_.has_value(),
            measure == PointSeries::Measure::Wgs84Planar2D,
            explicitPosition);
    }
};

AttributeValidityView
Attribute::validityView(model_ptr<Feature> const& feature, std::string_view layerName) const
{
    if (!feature || feature->borrowedModel() != borrowedModel())
        throw std::invalid_argument(
            "Attribute validity view requires a feature from the same layer.");
    AttributeValidityView view;
    view.node_ = simfil::ModelNode::Ptr(*this);
    view.host_ = feature;
    view.nameKey_ = data_->name_;
    view.layerKey_ = model().strings()->get(layerName);
    if (!view.layerKey_)
        view.unknownLayer_ = layerName;
    view.stored_ = validityOrNull();
    if (auto schema = model().layerSchema()) {
        auto decls = schema->pointSeries(feature->typeId(), layerName, name());
        if (!decls.empty() &&
            (std::ranges::any_of(
                 decls,
                 [](auto const& d) { return d.markers_ != PointSeries::Markers::None; }) ||
             pointSeriesBranch() || pointSeriesGeometry()))
        {
            view.state_ = std::make_shared<AttributeValidityView::State>(
                model().resolve<Attribute>(*this),
                feature,
                schema,
                decls);
            view.node_ = view.publicNode();
        }
    }
    return view;
}

PartitionFeatureLayer const& AttributeValidityView::model() const
{
    return static_cast<PartitionFeatureLayer const&>(*host_->borrowedModel());
}

simfil::ModelNode::Ptr AttributeValidityView::storedPoints() const
{
    auto key = model().strings()->get("pointValidities");
    return key ? node_->get(key) : simfil::ModelNode::Ptr{};
}

uint32_t AttributeValidityView::size() const
{
    return state_ ? state_->layout(false).size : (stored_ ? stored_->size() : 0);
}

uint32_t AttributeValidityView::pointCount() const
{
    if (state_)
        return state_->layout(true).size;
    auto points = storedPoints();
    return points && points->type() == simfil::ValueType::Array ? points->size() : 0;
}

bool AttributeValidityView::hasVirtualPointValidities() const
{
    return state_ && state_->separateDeclared;
}

model_ptr<Validity> AttributeValidityView::at(uint32_t i) const
{
    if (state_)
        return state_->at(i, false);
    auto v = stored_ ? stored_->at(i) : simfil::ModelNode::Ptr{};
    return v ? model().resolve<Validity>(*v) : model_ptr<Validity>{};
}

model_ptr<Validity> AttributeValidityView::pointAt(uint32_t i) const
{
    if (state_)
        return state_->at(i, true);
    return legacyPoint(model(), storedPoints(), i);
}

simfil::ModelNode::Ptr
AttributeValidityView::markerSample(uint32_t index, std::string_view field, bool separate) const
{
    if (!state_)
        return {};
    auto run = state_->layout(separate).find(index);
    if (!run)
        return {};
    for (auto const& f : run->reader->declaration().fields_)
        if (f.name_ == field)
            return run->sample(index, f, state_->attr->sourceDataReferences());
    return {};
}

simfil::ModelNode::Ptr AttributeValidityView::markerSamples(uint32_t index, bool separate) const
{
    if (!state_)
        return {};
    auto run = state_->layout(separate).find(index);
    if (!run)
        return {};
    auto strings = state_->attr->model().strings();
    Context::Fields fields;
    for (auto const& field : run->reader->declaration().fields_) {
        fields.emplace_back(
            strings->get(field.name_),
            [strings, state = state_, run, &field, index]
            {
                return Context::array(
                    strings,
                    {run->sample(index, field, state->attr->sourceDataReferences())});
            });
    }
    return Context::object(strings, std::move(fields));
}

simfil::ModelNode::Ptr AttributeValidityView::publicNode() const
{
    auto attr = state_->attr;
    auto strings = attr->model().strings();
    auto array = [state = state_, strings](bool separate)
    {
        return Context::array(
            strings,
            [state, separate] { return state->layout(separate).size; },
            [state, separate](uint32_t i) { return state->at(i, separate); });
    };
    auto const hasPoints = hasVirtualPointValidities();
    Context::Fields fields;
    if (state_->stored || size()) {
        fields.emplace_back(
            StringPool::ValidityStr,
            [state = state_, array]() -> State::Node {
                return state->layout(false).size == 1 ?
                    State::Node(state->at(0, false)) :
                    array(false);
            });
    }
    bool emittedPoints = false;
    for (uint32_t i = 0; i < attr->size(); ++i) {
        auto key = attr->keyAt(i);
        if (i == 0 && state_->stored && key == StringPool::ValidityStr)
            continue;
        if (auto name = strings->resolve(key);
            name == "_mapgetPointSeriesBranch" || name == "_mapgetPointSeriesGeometry")
            continue;
        if (hasPoints && key == state_->pointKey) {
            fields.emplace_back(key, [array] { return array(true); });
            emittedPoints = true;
        }
        else {
            fields.emplace_back(key, [attr, i] { return attr->at(i); });
        }
    }
    if (hasPoints && !emittedPoints) {
        if (!state_->pointKey)
            throw PointQueryError("Missing pointValidities name in the tile dictionary.");
        fields.emplace_back(state_->pointKey, [array] { return array(true); });
    }
    return Context::object(strings, std::move(fields), {}, attr->schemaType());
}

model_ptr<simfil::OverlayNode>
AttributeValidityView::queryContext(uint32_t attributeIndex, uint32_t validityIndex) const
{
    using simfil::Value;
    auto count = size();
    if (validityIndex >= std::max(1u, count))
        throw std::out_of_range("Invalid public attribute validity index.");
    auto result = model_ptr<simfil::OverlayNode>::make(Value::field(*node_));
    auto strings = model().strings();
    result->set(StringPool::OverlayNameStr, Value::make(std::string(*strings->resolve(nameKey_))));
    result->set(StringPool::OverlayFeatureStr, Value::field(*host_));
    result->set(
        StringPool::OverlayLayerStr,
        Value::make(layerKey_ ? std::string(*strings->resolve(layerKey_)) : unknownLayer_));
    result->set(StringPool::OverlayAttributeIndexStr, Value::make(int64_t(attributeIndex)));
    result->set(StringPool::OverlayValidityIndexStr, Value::make(int64_t(validityIndex)));
    result->set(StringPool::OverlayValidityCountStr, Value::make(int64_t(std::max(1u, count))));
    result->set(StringPool::OverlayHasValidityStr, Value::make(count != 0));
    if (auto samples = markerSamples(validityIndex))
        result->set(StringPool::OverlaySamplesStr, Value::field(*samples));
    return result;
}
}  // namespace mapget
