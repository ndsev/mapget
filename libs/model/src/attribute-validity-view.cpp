#include "attr.h"
#include "featurelayer.h"
#include "shapepoint-series.h"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>

namespace mapget
{
using shape::Axis;
using shape::Context;
using shape::SeriesReader;

struct AttributeValidityView::State
{
    using Node = simfil::ModelNode::Ptr;

    struct Run
    {
        std::shared_ptr<Axis> axis;
        std::shared_ptr<SeriesReader> reader;
        uint32_t first;
        uint32_t count;

        Node sample(
            uint32_t record,
            PointSeries::Field const& field,
            model_ptr<SourceDataReferenceCollection> const& refs) const
        {
            auto candidate = reader->markerSample(record, field);
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
        std::vector<Run> runs;        // Appended, synthesized from raw record positions.
        std::vector<Run> storedRuns;  // Original validity slots supply the record positions.
        uint32_t size = 0;
        bool explicitNone = false;

        Run const* find(uint32_t index) const
        {
            for (auto const& run : runs)
                if (index >= run.first && index - run.first < run.count)
                    return &run;
            return nullptr;
        }

        /** Translate the public validity ordinal into a raw record ordinal, not a coordinate. */
        std::pair<Run const*, uint32_t> source(uint32_t index) const
        {
            if (auto run = find(index))
                return {run, index - run->first};
            for (auto const& run : storedRuns) {
                auto ordinals = run.reader->storedMarkerOrdinals();
                auto it = std::ranges::lower_bound(ordinals, index);
                if (it != ordinals.end() && *it == index)
                    return {&run, static_cast<uint32_t>(it - ordinals.begin())};
            }
            return {};
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
    }

    model_ptr<Attribute> attr;
    model_ptr<Feature> host;
    model_ptr<MultiValidity> stored;
    LayerSchema::Ptr schema;  // Pins the readers' declarations, even after the view is copied.
    std::span<PointSeries const> declarations;
    mutable std::once_flag prepared;
    mutable Layout markers;

    Layout const& layout() const
    {
        std::call_once(
            prepared,
            [&]
            {
                Layout result;
                if (stored)
                    result.size = stored->size();
                std::map<uint32_t, std::shared_ptr<Axis>> axes;
                for (auto const& decl : declarations) {
                    if (decl.markerTag_.empty())
                        continue;
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
                        if (decl.position_.kind_ == PointSeries::Position::Kind::TaggedValidity) {
                            // These anchors already occupy canonical slots. Read records only
                            // when a caller asks for samples, never append another marker.
                            result.storedRuns.push_back({axis, std::move(reader), 0, 0});
                            break;
                        }
                        auto n = reader->markerCount();
                        // [] explicitly covers nothing, whereas no assignment may mean full
                        // coverage. A NONE slot keeps this distinction visible if markers would
                        // hide the empty list.
                        if (n && stored && !result.size) {
                            result.explicitNone = true;
                            ++result.size;
                        }
                        if (n > std::numeric_limits<uint32_t>::max() - result.size)
                            throw PointQueryError("Too many point markers on one attribute.");
                        result.runs.push_back({axis, std::move(reader), result.size, n});
                        result.size += n;
                        break;
                    }
                }
                markers = std::move(result);
            });
        return markers;
    }

    model_ptr<Validity> at(uint32_t index) const
    {
        auto const& list = layout();
        if (index >= list.size)
            return {};
        auto run = list.find(index);
        if (!run) {
            if (list.explicitNone && index == 0)
                return attr->model().resolve<Validity>(simfil::ModelNodeAddress{
                    PartitionFeatureLayer::ColumnId::SimpleValidity,
                    Validity::None});
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
        auto tag = attr->model().strings()->get(decl.markerTag_);
        if (!tag)
            throw PointQueryError("Missing measurement tag in the tile dictionary.");
        return Validity::view(
            axis.geometry_,
            axis.ordinal_,
            type,
            offset,
            vertex,
            tag,
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
            (std::ranges::any_of(decls, [](auto const& d) { return !d.markerTag_.empty(); }) ||
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

uint32_t AttributeValidityView::size() const
{
    return state_ ? state_->layout().size : (stored_ ? stored_->size() : 0);
}

model_ptr<Validity> AttributeValidityView::at(uint32_t i) const
{
    if (state_)
        return state_->at(i);
    auto v = stored_ ? stored_->at(i) : simfil::ModelNode::Ptr{};
    return v ? model().resolve<Validity>(*v) : model_ptr<Validity>{};
}

simfil::ModelNode::Ptr
AttributeValidityView::markerSample(uint32_t index, std::string_view field) const
{
    if (!state_)
        return {};
    auto [run, record] = state_->layout().source(index);
    if (!run)
        return {};
    for (auto const& f : run->reader->declaration().fields_)
        if (f.name_ == field)
            return run->sample(record, f, state_->attr->sourceDataReferences());
    return {};
}

simfil::ModelNode::Ptr AttributeValidityView::markerSamples(uint32_t index) const
{
    if (!state_)
        return {};
    auto [run, record] = state_->layout().source(index);
    if (!run)
        return {};
    auto strings = state_->attr->model().strings();
    Context::Fields fields;
    for (auto const& field : run->reader->declaration().fields_) {
        fields.emplace_back(
            strings->get(field.name_),
            [strings, state = state_, run, &field, record]
            {
                return Context::array(
                    strings,
                    {run->sample(record, field, state->attr->sourceDataReferences())});
            });
    }
    return Context::object(strings, std::move(fields));
}

simfil::ModelNode::Ptr AttributeValidityView::publicNode() const
{
    auto attr = state_->attr;
    auto strings = attr->model().strings();
    Context::Fields fields;
    if (state_->stored || size()) {
        fields.emplace_back(
            StringPool::ValidityStr,
            [state = state_, strings]() -> State::Node
            {
                auto count = state->layout().size;
                if (count == 1)
                    return state->at(0);
                return Context::array(
                    strings,
                    [count] { return count; },
                    [state](uint32_t i) { return state->at(i); });
            });
    }
    for (uint32_t i = 0; i < attr->size(); ++i) {
        auto key = attr->keyAt(i);
        if (i == 0 && state_->stored && key == StringPool::ValidityStr)
            continue;
        if (auto name = strings->resolve(key);
            name == "_mapgetPointSeriesBranch" || name == "_mapgetPointSeriesGeometry")
            continue;
        fields.emplace_back(key, [attr, i] { return attr->at(i); });
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
