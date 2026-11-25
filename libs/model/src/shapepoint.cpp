#include "shapepoint.h"

#include "featurelayer.h"
#include "mapschema.h"
#include "shapepoint-series.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace mapget
{
thread_local PointQueryError::Capture* PointQueryError::Capture::active_ = nullptr;
PointQueryError::Capture::Capture() noexcept : previous_(active_)
{
    active_ = this;
}
PointQueryError::Capture::~Capture()
{
    active_ = previous_;
}
void PointQueryError::Capture::rethrowIfFailed() const
{
    if (error_)
        std::rethrow_exception(error_);
}
bool PointQueryError::Capture::recordCurrent() noexcept
{
    if (!active_)
        return false;
    if (!active_->error_)
        active_->error_ = std::current_exception();
    return true;
}

namespace
{
using shape::Axis;
using shape::Context;
using shape::SeriesReader;
using Node = simfil::ModelNode::Ptr;
using Id = simfil::StringId;
using Span = Axis::Span;

// A physical Attribute can be shared by several layers/features. This handle
// pins one attachment and its schema, not just the attribute storage address.
class Occurrence : public std::enable_shared_from_this<Occurrence>
{
public:
    struct Bound
    {
        std::shared_ptr<SeriesReader> reader;
        std::vector<std::pair<uint32_t, Span>> assignments;
        bool unrestricted = false;
    };
    std::shared_ptr<Axis> axis;
    model_ptr<Attribute> attr;
    std::string layer;
    uint32_t index;
    std::optional<uint64_t> layerId;
    LayerSchema::Ptr schema;
    simfil::SchemaType queryType;
    std::span<PointSeries const> contracts;
    std::once_flag prepared;
    std::vector<Bound> applicable;

    // Visit only participating contracts; a failed record index cannot hide
    // unrelated layers, a bare marker, or the host geometry.
    void prepare()
    {
        std::call_once(
            prepared,
            [&]
            {
                std::vector<Bound> next;
                auto bind = [&](PointSeries const* contract)
                {
                    auto validities = attr->validityOrNull();
                    if (validities && !validities->size())
                        return;
                    if (contract && !axis->selects(*contract, *attr))
                        return;
                    Bound bound;
                    auto measure = PointSeries::Measure::GeographicMetres;
                    if (contract) {
                        measure = contract->assignmentMeasure_.value_or(contract->measure_);
                        bound.reader =
                            std::make_shared<SeriesReader>(axis, attr, *contract, schema);
                    }
                    bool hasAssignment = false;
                    for (uint32_t i = 0; validities && i < validities->size(); ++i) {
                        if (!(i % 128))
                            axis->check();
                        auto v = attr->model().resolve<Validity>(*validities->at(i));
                        if (v->tag())
                            continue;  // A value annotation never extends assignment coverage.
                        hasAssignment = true;
                        if (auto span = axis->coverage(*v, measure, contract != nullptr))
                            bound.assignments.emplace_back(i, *span);
                    }
                    // The explicitly empty collection returned above. A nonempty list of
                    // annotations alone has no assignment of its own.
                    bound.unrestricted = !hasAssignment && contract &&
                        contract->omittedValidityCovers_;
                    if (bound.unrestricted || !bound.assignments.empty())
                        next.push_back(std::move(bound));
                };
                if (contracts.empty())
                    bind(nullptr);
                else
                    for (auto const& contract : contracts)
                        bind(&contract);
                applicable = std::move(next);
            });
    }

    class Vertex : public std::enable_shared_from_this<Vertex>
    {
    public:
        Vertex(std::shared_ptr<Occurrence> occurrence, uint32_t point)
            : occurrence_(std::move(occurrence)),
              point_(point),
              strings_(occurrence_->axis->host_->model().strings())
        {
        }
        bool match()
        {
            occurrence_->prepare();
            for (auto const& bound : occurrence_->applicable) {
                std::vector<Span> spans;
                if (bound.unrestricted)
                    spans.emplace_back(0, occurrence_->axis->last());
                for (auto const& [i, span] : bound.assignments) {
                    if ((point_ >= span.first || Axis::near(point_, span.first)) &&
                        (point_ <= span.second || Axis::near(point_, span.second)))
                    {
                        spans.push_back(span);
                        validities_.push_back(i);
                    }
                }
                if (spans.empty())
                    continue;
                // Distinct validity rows can denote exactly the same local origin.
                // Preserve both backlinks without making the origin ambiguous.
                if (spans.size() > 1) {
                    std::ranges::sort(spans);
                    spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
                }
                covered_ = true;
                if (!bound.reader)
                    continue;
                for (auto const& field : bound.reader->declaration().fields_) {
                    auto id = strings_->get(field.name_);
                    if (!id)
                        throw PointQueryError(
                            "Decoded point field missing from the input dictionary.");
                    projections_[id].bindings.push_back({bound.reader, &field, spans});
                }
            }
            std::ranges::sort(validities_);
            validities_
                .erase(std::unique(validities_.begin(), validities_.end()), validities_.end());
            return covered_;
        }
        Node node()
        {
            auto self = shared_from_this();
            Context::Fields fields;
            auto member = [&](std::string_view key, Context::Getter getter)
            {
                auto id = strings_->get(key);
                if (id)
                    fields.emplace_back(id, std::move(getter));
            };
            member(
                "$name",
                [self] { return Context::scalar(std::string(self->occurrence_->attr->name())); });
            member("$layer", [self] { return Context::scalar(self->occurrence_->layer); });
            member(
                "$attributeIndex",
                [self] { return Context::scalar(int64_t(self->occurrence_->index)); });
            if (occurrence_->layerId)
                member(
                    "$layerId",
                    [self] { return Context::scalar(int64_t(*self->occurrence_->layerId)); });
            member(
                "$validities",
                [self]
                {
                    std::vector<Node> elements;
                    for (auto i : self->validities_)
                        elements.push_back(Context::scalar(int64_t(i)));
                    return Context::array(self->strings_, std::move(elements));
                });
            for (auto const& [id, slot] : projections_)
                fields.emplace_back(id, [self, id] { return self->project(id, false); });
            occurrence_->attr->forEachField(
                [&](auto const& key, Node const& value)
                {
                    auto id = strings_->get(key);
                    if (!id || projections_.contains(id) || key.starts_with("_mapgetPointSeries") ||
                        key == "_sourceData")
                        return true;
                    if (!occurrence_->contracts.empty()) {
                        if (key == "attributeValue")
                            return true;
                        auto carrier = [&](PointSeries::Path const& path)
                        {
                            return !path.empty() && key == path.front();
                        };
                        for (auto const& decl : occurrence_->contracts) {
                            if (carrier(decl.records_) || carrier(decl.branches_) ||
                                carrier(decl.position_.vertexMeasures_))
                                return true;
                            if (decl.parallel_ && decl.records_.empty()) {
                                if (carrier(decl.position_.field_) ||
                                    carrier(decl.position_.end_) || carrier(decl.position_.bits_) ||
                                    carrier(decl.alternative_))
                                    return true;
                                for (auto const& field : decl.fields_)
                                    if (carrier(field.path_))
                                        return true;
                            }
                        }
                    }
                    fields.emplace_back(id, [value] { return value; });
                    return true;
                });
            return Context::object(
                strings_,
                std::move(fields),
                [self](Id id)
                {
                    if (id == self->strings_->get("$samples")) {
                        Context::Fields f;
                        for (auto const& [key, slot] : self->projections_)
                            f.emplace_back(key, [self, key] { return self->project(key, true); });
                        return Context::object(
                            self->strings_,
                            std::move(f),
                            {},
                            self->occurrence_->queryType.field("$samples").type());
                    }
                    return self->occurrence_->attr->get(id);
                },
                occurrence_->queryType);
        }

    private:
        struct Slot
        {
            struct Binding
            {
                std::shared_ptr<SeriesReader> reader;
                PointSeries::Field const* field;
                std::vector<Span> spans;
            };
            std::vector<Binding> bindings;
            std::once_flag read;
            std::vector<std::pair<Binding const*, SeriesReader::Sample>> results;
        };
        std::shared_ptr<Occurrence> occurrence_;
        uint32_t point_;
        Context::Dictionary strings_;
        std::map<Id, Slot> projections_;
        std::vector<uint32_t> validities_;
        bool covered_ = false;

        Node project(Id id, bool explain)
        {
            auto& slot = projections_.at(id);
            std::call_once(
                slot.read,
                [&]
                {
                    decltype(slot.results) next;
                    auto output = occurrence_->queryType.field(*strings_->resolve(id)).type();
                    for (auto const& b : slot.bindings)
                        for (auto& s : b.reader->samples(point_, *b.field, b.spans)) {
                            // An exact anchor in a linear series can return its raw integer.
                            // Its decoded measurement is still float; contributors keep the integer.
                            if (output.kind() == simfil::SchemaType::Kind::Float && s.value &&
                                s.value->type() == simfil::ValueType::Int)
                            {
                                s.value = Context::scalar(double(std::get<int64_t>(s.value->value())));
                            }
                            next.emplace_back(&b, std::move(s));
                        }
                    slot.results = std::move(next);
                });
            if (!explain) {
                if (slot.results.size() > 1)
                    throw PointQueryError(
                        "Multiple samples for " + std::string(strings_->resolve(id).value_or("")) +
                        "; select a candidate in $samples.");
                return slot.results.empty() ? Node{} : slot.results.front().second.value;
            }
            std::vector<Node> result;
            for (auto const& [binding, sample] : slot.results)
                result.push_back(binding->reader->explain(sample, *binding->field));
            return Context::array(strings_, std::move(result));
        }
    };
};
}  // namespace

struct ShapePointRange::State : public std::enable_shared_from_this<State>
{
    explicit State(model_ptr<Feature> host, uint32_t index, std::function<bool()> cancelled)
        : axis(std::make_shared<Axis>(std::move(host), index, std::move(cancelled))),
          strings(axis->host_->model().strings())
    {
    }
    std::shared_ptr<Axis> axis;
    Context::Dictionary strings;
    simfil::SchemaType queryType;
    std::once_flag queryTypeReady;
    simfil::SchemaType const& rootType()
    {
        // Coordinate-only picks and scans never need the host's query graph.
        std::call_once(queryTypeReady, [&] {
            if (auto layer = axis->host_->model().layerSchema())
                queryType = layer->shapePointQueryType(axis->host_->typeId());
        });
        return queryType;
    }
    std::once_flag initialized;
    using Occurrences = std::vector<std::shared_ptr<Occurrence>>;
    Occurrences all;
    std::map<std::pair<Id, Id>, Occurrences> named;
    void prepare()
    {
        std::call_once(
            initialized,
            [&]
            {
                auto layers = axis->host_->attributeLayersOrNull();
                if (!layers)
                    return;
                auto schema = axis->host_->model().layerSchema();
                Occurrences next;
                decltype(named) indexes;
                layers->forEachLayer(
                    [&](std::string_view layerName, auto const& layer)
                    {
                        auto layerId = strings->get(layerName);
                        return layer->forEachAttribute(
                            [&](auto const& attr)
                            {
                                if (!(next.size() % 128))
                                    axis->check();
                                auto attrId = strings->get(attr->name());
                                auto use = std::make_shared<Occurrence>();
                                use->axis = axis;
                                use->attr = attr;
                                use->layer = layerName;
                                use->layerId = layer->id();
                                use->index = static_cast<uint32_t>(next.size());
                                use->schema = schema;
                                use->queryType =
                                    queryType.field("$attributes")
                                        .type()
                                        .field(layerName)
                                        .type()
                                        .field(attr->name())
                                        .type()
                                        .itemType();
                                if (schema)
                                    use->contracts = schema->pointSeries(
                                        axis->host_->typeId(),
                                        layerName,
                                        attr->name());
                                next.push_back(use);
                                indexes[{layerId, attrId}].push_back(std::move(use));
                                return true;
                            });
                    });
                all = std::move(next);
                named = std::move(indexes);
                ++axis->inventories;
            });
    }

    Node attributes(uint32_t point, Id layer = 0, Id name = 0)
    {
        struct Matches
        {
            std::once_flag evaluated;
            std::vector<Node> nodes;
        };
        auto matches = std::make_shared<Matches>();
        auto self = shared_from_this();
        auto resolve = [self, matches, point, layer, name]() -> std::vector<Node> const&
        {
            std::call_once(
                matches->evaluated,
                [&]
                {
                    self->prepare();
                    std::vector<Node> nodes;
                    auto visit = [&](Occurrences const& uses)
                    {
                        for (auto const& use : uses) {
                            auto view = std::make_shared<Occurrence::Vertex>(use, point);
                            if (view->match())
                                nodes.push_back(view->node());
                        }
                    };
                    if (!layer)
                        visit(self->all);
                    else if (name) {
                        if (auto it = self->named.find({layer, name}); it != self->named.end())
                            visit(it->second);
                    }
                    matches->nodes = std::move(nodes);
                });
            return matches->nodes;
        };
        // A named layer is a lookup-only index into the canonical occurrence
        // sequence. It must not appear as a second branch under recursive '*'.
        auto arrayType = queryType.field("$attributes").type();
        Context::Lookup lookup;
        if (!layer)
            lookup = [self, point, arrayType](Id layerId) -> Node
            {
                self->prepare();
                auto first = self->named.lower_bound({layerId, 0});
                if (first == self->named.end() || first->first.first != layerId)
                    return {};
                return Context::object(
                    self->strings,
                    {},
                    [self, point, layerId](Id attr) -> Node
                    {
                        if (!self->named.contains({layerId, attr}))
                            return {};
                        return self->attributes(point, layerId, attr);
                    },
                    arrayType.field(*self->strings->resolve(layerId)).type());
            };
        else if (name)
            arrayType =
                arrayType.field(*strings->resolve(layer))
                    .type()
                    .field(*strings->resolve(name))
                    .type();
        return Context::array(
            strings,
            [resolve] { return static_cast<uint32_t>(resolve().size()); },
            [resolve](uint32_t i) { return resolve()[i]; },
            std::move(lookup),
            arrayType);
    }
};

ShapePointRange Feature::shapePoints(uint32_t ordinal, std::function<bool()> cancelled) const
{
    return {model_ptr<Feature>(*this), ordinal, std::move(cancelled)};
}
ShapePointRange::ShapePointRange(
    model_ptr<Feature> host,
    uint32_t ordinal,
    std::function<bool()> cancelled)
    : state_(std::make_shared<State>(std::move(host), ordinal, std::move(cancelled)))
{
}
ShapePointRange::Work ShapePointRange::work() const
{
    auto const& axis = *state_->axis;
    return {
        axis.inventories.load(),
        axis.recordIndexes.load(),
        axis.recordsIndexed.load(),
        axis.distanceTables.load()};
}
model_ptr<Geometry> ShapePointRange::geometry() const
{
    return state_->axis->geometry_;
}
uint32_t ShapePointRange::size() const
{
    auto const& g = state_->axis->geometry_;
    return g ? g->numPoints() : 0;
}
std::optional<ShapePointView> ShapePointRange::at(uint32_t point) const
{
    if (point >= size())
        return {};
    return ShapePointView(state_, point);
}
Point ShapePointView::position() const
{
    return state_->axis->geometry_->pointAt(index_);
}
Node ShapePointView::queryContext() const
{
    auto strings = state_->strings;
    return Context::object(
        strings,
        {{strings->get("$pointIndex"),
          [i = index_]
          {
              return Context::scalar(int64_t(i));
          }},
         {strings->get("$geometryOrdinal"),
          [ordinal = state_->axis->ordinal_]
          {
              return Context::scalar(int64_t(ordinal));
          }},
         {strings->get("$attributes"),
          [s = state_, i = index_]
          {
              return s->attributes(i);
          }}},
        [s = state_](Id id) -> Node
        {
            if (id == s->strings->get("$feature"))
                return s->axis->host_;
            if (id == s->strings->get("$geometry"))
                return s->axis->geometry_;
            return {};
        },
        state_->rootType());
}
}  // namespace mapget
