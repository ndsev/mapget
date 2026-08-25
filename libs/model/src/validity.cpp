#include "validity.h"
#include "mapget/log.h"
#include "stringpool.h"
#include "featurelayer.h"

#include <algorithm>
#include <limits>

namespace mapget
{

namespace
{
/** Return the full validity record backing an AttrPoint index view. */
model_ptr<Validity> attrPointValidity(
    TileFeatureLayer const& model,
    simfil::ModelNodeAddress const& viewAddress)
{
    return model.resolve<Validity>(simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::Validities,
        viewAddress.index()});
}

/** Build the compact sequence-reference node shared by index and range views. */
model_ptr<AttrPointSequenceReference> attrPointSequenceReference(
    TileFeatureLayer const& model,
    simfil::ModelNodeAddress const& sequenceAddress)
{
    return model.resolve<AttrPointSequenceReference>(simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::AttrPointSequenceReferences,
        sequenceAddress.index()});
}

/** Visit every field in a small fixed-size procedural object. */
bool iterateObject(
    simfil::ModelNode const& object,
    simfil::ModelNode::IterCallback const& callback)
{
    for (uint32_t index = 0; index < object.size(); ++index) {
        auto value = object.at(static_cast<int64_t>(index));
        if (value && !callback(*value)) {
            return false;
        }
    }
    return true;
}

/** Convert a validity direction enum into the exported JSON token. */
std::string_view directionToString(Validity::Direction const& d)
{
    switch (d) {
    case Validity::Empty: return "EMPTY";
    case Validity::Positive: return "POSITIVE";
    case Validity::Negative: return "NEGATIVE";
    case Validity::Both: return "COMPLETE";
    case Validity::None: return "NONE";
    }
    return "?";
}

}  // namespace

AttrPointIndex::AttrPointIndex(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
          std::move(model),
          address,
          key)
{
}

model_ptr<AttrPointSequence> AttrPointIndex::sequence() const
{
    auto validity = attrPointValidity(model(), addr());
    if (validity->geometryDescriptionType() != Validity::AttrPointIndexValidity) {
        raise("AttrPointIndex view does not reference an AttrPointIndex validity.");
    }
    return model().resolve<AttrPointSequence>(
        validity->data_->geomDescr_.attrPointIndex_.sequence_);
}

uint32_t AttrPointIndex::index() const
{
    auto validity = attrPointValidity(model(), addr());
    if (validity->geometryDescriptionType() != Validity::AttrPointIndexValidity) {
        raise("AttrPointIndex view does not reference an AttrPointIndex validity.");
    }
    return validity->data_->geomDescr_.attrPointIndex_.index_;
}

nlohmann::json AttrPointIndex::toJson() const
{
    return nlohmann::json::object({
        {"sequence", nlohmann::json::object({
            {"$mapgetAttrPointSequence", sequence()->addr().index()},
        })},
        {"index", index()},
    });
}

simfil::ValueType AttrPointIndex::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttrPointIndex::at(int64_t fieldIndex) const
{
    return fieldIndex >= 0 && fieldIndex < 2 ? get(keyAt(fieldIndex)) : simfil::ModelNode::Ptr{};
}

simfil::ModelNode::Ptr AttrPointIndex::get(simfil::StringId const& field) const
{
    if (field == StringPool::SequenceStr) {
        return attrPointSequenceReference(model(), sequence()->addr());
    }
    if (field == StringPool::IndexStr) {
        return model_ptr<simfil::ValueNode>::make(
            static_cast<int64_t>(index()),
            model().shared_from_this());
    }
    return {};
}

simfil::StringId AttrPointIndex::keyAt(int64_t fieldIndex) const
{
    switch (fieldIndex) {
    case 0: return StringPool::SequenceStr;
    case 1: return StringPool::IndexStr;
    default: return {};
    }
}

uint32_t AttrPointIndex::size() const
{
    return 2;
}

bool AttrPointIndex::iterate(IterCallback const& callback) const
{
    return iterateObject(*this, callback);
}

AttrPointIndexRange::AttrPointIndexRange(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
          std::move(model),
          address,
          key)
{
}

model_ptr<AttrPointSequence> AttrPointIndexRange::sequence() const
{
    auto validity = attrPointValidity(model(), addr());
    if (validity->geometryDescriptionType() != Validity::AttrPointIndexRangeValidity) {
        raise("AttrPointIndexRange view does not reference an AttrPointIndexRange validity.");
    }
    return model().resolve<AttrPointSequence>(
        validity->data_->geomDescr_.attrPointIndexRange_.sequence_);
}

uint32_t AttrPointIndexRange::start() const
{
    auto validity = attrPointValidity(model(), addr());
    if (validity->geometryDescriptionType() != Validity::AttrPointIndexRangeValidity) {
        raise("AttrPointIndexRange view does not reference an AttrPointIndexRange validity.");
    }
    return validity->data_->geomDescr_.attrPointIndexRange_.start_;
}

uint32_t AttrPointIndexRange::end() const
{
    auto validity = attrPointValidity(model(), addr());
    if (validity->geometryDescriptionType() != Validity::AttrPointIndexRangeValidity) {
        raise("AttrPointIndexRange view does not reference an AttrPointIndexRange validity.");
    }
    return validity->data_->geomDescr_.attrPointIndexRange_.end_;
}

nlohmann::json AttrPointIndexRange::toJson() const
{
    return nlohmann::json::object({
        {"sequence", nlohmann::json::object({
            {"$mapgetAttrPointSequence", sequence()->addr().index()},
        })},
        {"start", start()},
        {"end", end()},
    });
}

simfil::ValueType AttrPointIndexRange::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttrPointIndexRange::at(int64_t fieldIndex) const
{
    return fieldIndex >= 0 && fieldIndex < 3 ? get(keyAt(fieldIndex)) : simfil::ModelNode::Ptr{};
}

simfil::ModelNode::Ptr AttrPointIndexRange::get(simfil::StringId const& field) const
{
    if (field == StringPool::SequenceStr) {
        return attrPointSequenceReference(model(), sequence()->addr());
    }
    if (field == StringPool::StartStr) {
        return model_ptr<simfil::ValueNode>::make(
            static_cast<int64_t>(start()),
            model().shared_from_this());
    }
    if (field == StringPool::EndStr) {
        return model_ptr<simfil::ValueNode>::make(
            static_cast<int64_t>(end()),
            model().shared_from_this());
    }
    return {};
}

simfil::StringId AttrPointIndexRange::keyAt(int64_t fieldIndex) const
{
    switch (fieldIndex) {
    case 0: return StringPool::SequenceStr;
    case 1: return StringPool::StartStr;
    case 2: return StringPool::EndStr;
    default: return {};
    }
}

uint32_t AttrPointIndexRange::size() const
{
    return 3;
}

bool AttrPointIndexRange::iterate(IterCallback const& callback) const
{
    return iterateObject(*this, callback);
}

namespace
{

/** Convert a transition endpoint enum into the exported JSON token. */
std::string_view transitionEndToString(Validity::TransitionEnd const& end)
{
    switch (end) {
    case Validity::Start: return "START";
    case Validity::End: return "END";
    }
    return "?";
}

constexpr uint64_t SimpleValidityOwnerTag = 0x01ull << 56U;

/** Encode the owning validity-collection slot for a compact simple validity. */
int64_t encodeSimpleValidityOwner(simfil::ArrayIndex members, uint32_t elementIndex)
{
    if (members == simfil::InvalidArrayIndex || members > 0x00ffffffu) {
        raise("SimpleValidity owner members index out of range.");
    }

    return static_cast<int64_t>(
        SimpleValidityOwnerTag |
        (static_cast<uint64_t>(members) << 32U) |
        static_cast<uint64_t>(elementIndex));
}

/** Decode the owning validity-collection slot for a compact simple validity. */
std::optional<std::pair<simfil::ArrayIndex, uint32_t>> decodeSimpleValidityOwner(
    simfil::ScalarValueType const& runtimeData)
{
    auto const* encodedOwner = std::get_if<int64_t>(&runtimeData);
    if (!encodedOwner) {
        return std::nullopt;
    }

    auto const raw = static_cast<uint64_t>(*encodedOwner);
    if ((raw & 0xff00000000000000ull) != SimpleValidityOwnerTag) {
        return std::nullopt;
    }

    return std::pair{
        static_cast<simfil::ArrayIndex>((raw >> 32U) & 0x00ffffffu),
        static_cast<uint32_t>(raw & 0xffffffffu)};
}

/** Pack both transition endpoint flags into the compact stored bitfield. */
uint8_t packTransitionEnds(
    Validity::TransitionEnd fromConnectedEnd,
    Validity::TransitionEnd toConnectedEnd)
{
    return static_cast<uint8_t>(
        static_cast<uint8_t>(fromConnectedEnd) |
        (static_cast<uint8_t>(toConnectedEnd) << 1U));
}

/** Decode the source endpoint from the stored transition bitfield. */
Validity::TransitionEnd unpackFromConnectedEnd(uint8_t packedEnds)
{
    return (packedEnds & 0x1U) != 0 ? Validity::End : Validity::Start;
}

/** Decode the target endpoint from the stored transition bitfield. */
Validity::TransitionEnd unpackToConnectedEnd(uint8_t packedEnds)
{
    return (packedEnds & 0x2U) != 0 ? Validity::End : Validity::Start;
}

/** Pick the line geometry that should be used for line-based validity resolution. */
model_ptr<Geometry> resolveLineGeometry(
    model_ptr<GeometryCollection> const& geometryCollection,
    std::optional<std::string_view> referencedName)
{
    if (!geometryCollection) {
        return {};
    }
    model_ptr<Geometry> firstLine;
    model_ptr<Geometry> matchingLine;
    geometryCollection->forEachGeometry([&](model_ptr<Geometry> const& geometry)
    {
        if (geometry->geomType() != GeomType::Line) {
            return true;
        }
        if (!firstLine) {
            firstLine = geometry;
        }
        if (geometry->name() == referencedName) {
            matchingLine = geometry;
            return false;
        }
        return true;
    });
    // Unnamed validities prefer an unnamed line but retain the historical
    // fallback to the first line when a feature only has named geometry.
    return matchingLine ? matchingLine : (!referencedName ? firstLine : model_ptr<Geometry>{});
}

struct TransitionSegment
{
    // Ordered from the connected endpoint outwards. The final segment may be
    // an extrapolated endpoint tangent when the complete road is shorter than
    // the semantic transition-leg length.
    std::vector<Point> innerToOuter_;
};

constexpr double kTransitionLegLengthMeters = 10.0;

/** Compare two validity points with a small tolerance to absorb numeric noise. */
bool pointsCoincide(Point const& left, Point const& right)
{
    return left.distanceTo(right) < 1e-9;
}

/** Resolve the endpoint segment that participates in a semantic feature transition. */
std::optional<TransitionSegment> resolveTransitionSegment(
    model_ptr<Feature> const& feature,
    Validity::TransitionEnd connectedEnd,
    std::optional<std::string_view> referencedName)
{
    if (!feature) {
        return std::nullopt;
    }

    auto geometry = resolveLineGeometry(feature->geomOrNull(), referencedName);
    if (!geometry || geometry->numPoints() == 0) {
        return std::nullopt;
    }

    const auto numPoints = geometry->numPoints();
    const auto innerIndex = connectedEnd == Validity::End ? numPoints - 1U : 0U;
    TransitionSegment result;
    result.innerToOuter_.push_back(geometry->pointAt(innerIndex));

    double accumulatedMeters = 0.0;
    auto appendCandidate = [&](Point const& candidate) {
        auto const& previous = result.innerToOuter_.back();
        auto const segmentMeters = previous.geographicDistanceTo(candidate);
        if (segmentMeters <= 1.0e-6) {
            return false;
        }
        auto const remainingMeters =
            kTransitionLegLengthMeters - accumulatedMeters;
        if (segmentMeters >= remainingMeters) {
            result.innerToOuter_.push_back(Point{
                previous + (candidate - previous) *
                    (remainingMeters / segmentMeters)});
            accumulatedMeters = kTransitionLegLengthMeters;
            return true;
        }
        result.innerToOuter_.push_back(candidate);
        accumulatedMeters += segmentMeters;
        return false;
    };

    if (connectedEnd == Validity::End) {
        for (auto pointIndex = innerIndex; pointIndex-- > 0;) {
            if (appendCandidate(geometry->pointAt(pointIndex))) {
                break;
            }
        }
    } else {
        for (auto pointIndex = innerIndex + 1U; pointIndex < numPoints; ++pointIndex) {
            if (appendCandidate(geometry->pointAt(pointIndex))) {
                break;
            }
        }
    }

    if (accumulatedMeters + 1.0e-6 < kTransitionLegLengthMeters &&
        result.innerToOuter_.size() >= 2)
    {
        // Preserve the complete short road, then extend its outer endpoint
        // tangent by precisely the missing distance. This keeps the real
        // source shape while guaranteeing a readable ten-metre semantic leg.
        auto const& previous = result.innerToOuter_[result.innerToOuter_.size() - 2];
        auto const& outer = result.innerToOuter_.back();
        auto const tangentMeters = previous.geographicDistanceTo(outer);
        if (tangentMeters > 1.0e-6) {
            result.innerToOuter_.push_back(Point{
                outer + (outer - previous) *
                    ((kTransitionLegLengthMeters - accumulatedMeters) /
                     tangentMeters)});
        }
    }
    return result.innerToOuter_.size() >= 2
        ? std::optional<TransitionSegment>{std::move(result)}
        : std::nullopt;
}

/** Apply direction semantics to a resolved geometry after the shape has been computed. */
SelfContainedGeometry applyDirectionToGeometry(
    SelfContainedGeometry geometry,
    Validity::Direction direction)
{
    if (direction == Validity::Negative && geometry.points_.size() > 1) {
        if (geometry.geomType_ == GeomType::Polygon && geometry.polygonRingStarts_.size() > 1) {
            // Keep hole metadata meaningful: direction may reverse traversal,
            // but it must not flatten the outer/hole ring partition.
            for (size_t ringIndex = 0; ringIndex < geometry.polygonRingStarts_.size(); ++ringIndex) {
                auto const ringStart = geometry.polygonRingStarts_[ringIndex];
                auto const ringEnd = ringIndex + 1U < geometry.polygonRingStarts_.size()
                    ? geometry.polygonRingStarts_[ringIndex + 1U]
                    : static_cast<uint32_t>(geometry.points_.size());
                if (ringStart >= ringEnd || ringEnd > geometry.points_.size()) {
                    return geometry;
                }
                std::reverse(
                    geometry.points_.begin() + static_cast<std::ptrdiff_t>(ringStart),
                    geometry.points_.begin() + static_cast<std::ptrdiff_t>(ringEnd));
            }
            return geometry;
        }
        // Negative direction reuses the same geometric support but traverses it
        // against the feature digitization order.
        std::reverse(geometry.points_.begin(), geometry.points_.end());
    }
    return geometry;
}

/** Preserve point-vs-range validity semantics after offset resolution. */
GeomType geomTypeForOffsetValidity(
    Validity::GeometryDescriptionType descriptionType,
    std::vector<Point> const& points)
{
    if (descriptionType == Validity::OffsetPointValidity) {
        return GeomType::Points;
    }
    return points.size() > 1 ? GeomType::Line : GeomType::Points;
}

}

void Validity::ensureMaterialized()
{
    if (data_) {
        return;
    }

    auto const simpleAddress = addr();
    if (simpleAddress.column() != TileFeatureLayer::ColumnId::SimpleValidity) {
        raise("Cannot materialize validity from non-simple address.");
    }

    auto owner = decodeSimpleValidityOwner(ModelNode::data_);
    if (!owner) {
        raise("Cannot materialize detached simple validity without owner context.");
    }

    // Simple direction-only validities stay compact until one concrete
    // occurrence needs richer state. Upgrade only the owning collection slot.
    auto upgradedAddress = model().materializeSimpleValidity(
        simpleAddress,
        owner->first,
        owner->second,
        simpleDirection_);
    auto upgraded = model().resolve<Validity>(upgradedAddress);
    if (!upgraded || !upgraded->data_) {
        raise("Failed to materialize simple validity.");
    }
    data_ = upgraded->data_;
    fields_ = upgraded->fields_;
}

model_ptr<FeatureId> Validity::featureId() const
{
    if (!data_) {
        return {};
    }
    if (data_->featureAddress_) {
        return model().resolve<FeatureId>(data_->featureAddress_);
    }
    if (auto index = attrPointIndex()) {
        return index->sequence()->featureId();
    }
    if (auto range = attrPointIndexRange()) {
        return range->sequence()->featureId();
    }
    return {};
}

void Validity::setFeatureId(model_ptr<FeatureId> featureId)
{
    ensureMaterialized();
    if (!featureId) {
        data_->featureAddress_ = {};
        return;
    }
    data_->featureAddress_ = featureId->addr();
}

Validity::Validity(
    Validity::Direction direction,
    simfil::ModelConstPtr layer,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::ProceduralObject<7, Validity, TileFeatureLayer>(std::move(layer), a, key),
      simpleDirection_(direction)
{
    if (direction != Empty) {
        fields_.emplace_back(
            StringPool::DirectionStr,
            [](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(
                    directionToString(self.direction()),
                    self.model_);
            });
    }
}

Validity::Validity(
    Validity::Direction direction,
    simfil::ModelConstPtr layer,
    simfil::ModelNodeAddress a,
    simfil::ScalarValueType runtimeData,
    simfil::detail::mp_key key)
    : Validity(direction, std::move(layer), a, key)
{
    ModelNode::data_ = std::move(runtimeData);
}

Validity::Validity(Validity::Data* data,
    simfil::ModelConstPtr layer,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::ProceduralObject<7, Validity, TileFeatureLayer>(std::move(layer), a, key),
      data_(data)
{
    if (data_->direction_)
        fields_.emplace_back(
            StringPool::DirectionStr,
            [](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(
                    directionToString(self.direction()),
                    self.model_);
            });

    if (auto referencedGeometryName = geometryName()) {
        fields_.emplace_back(
            StringPool::GeometryNameStr,
            [referencedGeometryName](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(*referencedGeometryName, self.model_);
            });
    }

    if (data_->geomDescrType_ == SimpleGeometry) {
        // SimpleGeometry stores an explicit geometry node, so no offset or
        // transition metadata fields are exposed in the JSON view.
        fields_.emplace_back(
            StringPool::GeometryStr,
            [](Validity const& self)
            {
                return self.model().resolve(
                    self.data_->geomDescr_.simpleGeometry_);
            });
        return;
    }

    if (data_->geomDescrType_ == FeatureTransition) {
        // Semantic transitions serialize as feature references plus endpoint
        // metadata instead of an explicit geometry payload.
        fields_.emplace_back(
            StringPool::TransitionNumberStr,
            [](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(
                    static_cast<int64_t>(*self.transitionNumber()),
                    self.model_);
            });
        fields_.emplace_back(
            StringPool::FromStr,
            [](Validity const& self)
            {
                auto fromFeatureId = self.transitionFromFeatureId();
                return model_ptr<simfil::ValueNode>::make(
                    fromFeatureId ? fromFeatureId->toString() : std::string{},
                    self.model_);
            });
        fields_.emplace_back(
            StringPool::FromConnectedEndStr,
            [](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(
                    transitionEndToString(*self.transitionFromConnectedEnd()),
                    self.model_);
            });
        fields_.emplace_back(
            StringPool::ToStr,
            [](Validity const& self)
            {
                auto toFeatureId = self.transitionToFeatureId();
                return model_ptr<simfil::ValueNode>::make(
                    toFeatureId ? toFeatureId->toString() : std::string{},
                    self.model_);
            });
        fields_.emplace_back(
            StringPool::ToConnectedEndStr,
            [](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(
                    transitionEndToString(*self.transitionToConnectedEnd()),
                    self.model_);
            });
        return;
    }

    if (data_->geomDescrType_ == AttrPointIndexValidity) {
        fields_.emplace_back(
            StringPool::AttrPointIndexStr,
            [](Validity const& self) { return self.attrPointIndex(); });
        return;
    }

    if (data_->geomDescrType_ == AttrPointIndexRangeValidity) {
        fields_.emplace_back(
            StringPool::AttrPointIndexRangeStr,
            [](Validity const& self) { return self.attrPointIndexRange(); });
        return;
    }

    if (data_->geomOffsetType_ != InvalidOffsetType) {
        fields_.emplace_back(
            StringPool::OffsetTypeStr,
            [](Validity const& self)
            {
                std::string_view resultString = "Invalid";
                switch (self.geometryOffsetType()) {
                case InvalidOffsetType: break;
                case GeoPosOffset: resultString = "GeoPosOffset"; break;
                case BufferOffset: resultString = "BufferOffset"; break;
                case RelativeLengthOffset: resultString = "RelativeLengthOffset"; break;
                case MetricLengthOffset: resultString = "MetricLengthOffset"; break;
                }
                return model_ptr<simfil::ValueNode>::make(resultString, self.model_);
            });
    }

    // Offset-point and offset-range validities share the same storage; the
    // exported field names depend on the selected offset interpretation.
    auto exposeOffsetPoint = [this](StringId fieldName, uint32_t pointIndex, Point const& p)
    {
        fields_.emplace_back(
            fieldName,
            [pointIndex, p](Validity const& self) -> ModelNode::Ptr
            {
                switch (self.geometryOffsetType()) {
                case InvalidOffsetType: return ModelNode::Ptr{};
                case GeoPosOffset:
                    return self.model().resolve(
                        ModelNodeAddress{
                            TileFeatureLayer::ColumnId::ValidityPoints,
                            self.addr().index()},
                        pointIndex);
                case BufferOffset:
                case RelativeLengthOffset:
                case MetricLengthOffset:
                    return model_ptr<simfil::ValueNode>::make(p.x, self.model_);
                }
                return {};
            });
    };

    if (data_->geomDescrType_ == OffsetRangeValidity) {
        auto& start = data_->geomDescr_.range_.first;
        auto& end = data_->geomDescr_.range_.second;
        exposeOffsetPoint(StringPool::StartStr, 1, start);
        exposeOffsetPoint(StringPool::EndStr, 2, end);
    }
    else if (data_->geomDescrType_ == OffsetPointValidity) {
        exposeOffsetPoint(StringPool::PointStr, 0, data_->geomDescr_.point_);
    }

    if (data_->featureAddress_) {
        fields_.emplace_back(
            StringPool::FeatureIdStr,
            [](Validity const& self)
            {
                return self.featureId();
            });
    }
}

void Validity::setDirection(const Validity::Direction& v)
{
    ensureMaterialized();
    data_->direction_ = v;
}

Validity::Direction Validity::direction() const
{
    if (!data_) {
        return simpleDirection_;
    }
    return data_->direction_;
}

Validity::GeometryOffsetType Validity::geometryOffsetType() const
{
    if (!data_) {
        return InvalidOffsetType;
    }
    return data_->geomOffsetType_;
}

Validity::GeometryDescriptionType Validity::geometryDescriptionType() const
{
    if (!data_) {
        return NoGeometry;
    }
    return data_->geomDescrType_;
}

void Validity::setGeometryName(std::optional<std::string_view> geometryName)
{
    ensureMaterialized();
    data_->referencedGeometryName_ = model().encodeGeometryName(geometryName);
}

std::optional<std::string_view> Validity::geometryName() const
{
    if (!data_) {
        return {};
    }
    return model().decodeGeometryName(data_->referencedGeometryName_);
}

void Validity::setOffsetPoint(Point pos) {
    ensureMaterialized();
    data_->geomDescrType_ = OffsetPointValidity;
    data_->geomOffsetType_ = GeoPosOffset;
    data_->geomDescr_.point_ = pos;
}

void Validity::setOffsetPoint(Validity::GeometryOffsetType offsetType, double pos) {
    ensureMaterialized();
    assert(offsetType != InvalidOffsetType && offsetType != GeoPosOffset);
    data_->geomDescrType_ = OffsetPointValidity;
    data_->geomOffsetType_ = offsetType;
    data_->geomDescr_.point_ = Point{pos, 0, 0};
}

std::optional<Point> Validity::offsetPoint() const
{
    if (!data_) {
        return {};
    }
    if (data_->geomDescrType_ != OffsetPointValidity) {
        return {};
    }
    return data_->geomDescr_.point_;
}

void Validity::setOffsetRange(Point start, Point end) {
    ensureMaterialized();
    data_->geomDescrType_ = OffsetRangeValidity;
    data_->geomOffsetType_ = GeoPosOffset;
    data_->geomDescr_.range_ = {start, end};
}

void Validity::setOffsetRange(Validity::GeometryOffsetType offsetType, double start, double end) {
    ensureMaterialized();
    assert(offsetType != InvalidOffsetType && offsetType != GeoPosOffset);
    data_->geomDescrType_ = OffsetRangeValidity;
    data_->geomOffsetType_ = offsetType;
    data_->geomDescr_.range_ = {Point{start, 0, 0}, Point{end, 0, 0}};
}

std::optional<std::pair<Point, Point>> Validity::offsetRange() const
{
    if (!data_) {
        return {};
    }
    if (data_->geomDescrType_ != OffsetRangeValidity) {
        return {};
    }
    return std::pair<Point, Point>{data_->geomDescr_.range_.first, data_->geomDescr_.range_.second};
}

void Validity::setAttrPointIndex(
    model_ptr<AttrPointSequence> const& sequence,
    uint32_t index)
{
    ensureMaterialized();
    if (!sequence || sequence->owningModel().get() != &model()) {
        raise("Validity AttrPointSequence must belong to the same TileFeatureLayer.");
    }
    if (index >= sequence->positionCount()) {
        raiseFmt(
            "AttrPointIndex {} is out of range for a sequence with {} positions.",
            index,
            sequence->positionCount());
    }
    data_->geomDescrType_ = AttrPointIndexValidity;
    data_->geomOffsetType_ = InvalidOffsetType;
    data_->geomDescr_.attrPointIndex_ = {
        .sequence_ = sequence->addr(),
        .index_ = index,
    };
    data_->featureAddress_ = {};
    data_->referencedGeometryName_ = 0;
}

model_ptr<AttrPointIndex> Validity::attrPointIndex() const
{
    if (!data_ || data_->geomDescrType_ != AttrPointIndexValidity) {
        return {};
    }
    return model().resolve<AttrPointIndex>(simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::AttrPointIndexView,
        addr().index()});
}

void Validity::setAttrPointIndexRange(
    model_ptr<AttrPointSequence> const& sequence,
    uint32_t start,
    uint32_t end)
{
    ensureMaterialized();
    if (!sequence || sequence->owningModel().get() != &model()) {
        raise("Validity AttrPointSequence must belong to the same TileFeatureLayer.");
    }
    if (start >= sequence->positionCount() || end >= sequence->positionCount()) {
        raiseFmt(
            "AttrPointIndexRange {}..{} is out of range for a sequence with {} positions.",
            start,
            end,
            sequence->positionCount());
    }
    if (start > end) {
        raiseFmt(
            "AttrPointIndexRange start {} exceeds end {}.",
            start,
            end);
    }
    data_->geomDescrType_ = AttrPointIndexRangeValidity;
    data_->geomOffsetType_ = InvalidOffsetType;
    data_->geomDescr_.attrPointIndexRange_ = {
        .sequence_ = sequence->addr(),
        .start_ = start,
        .end_ = end,
    };
    data_->featureAddress_ = {};
    data_->referencedGeometryName_ = 0;
}

model_ptr<AttrPointIndexRange> Validity::attrPointIndexRange() const
{
    if (!data_ || data_->geomDescrType_ != AttrPointIndexRangeValidity) {
        return {};
    }
    return model().resolve<AttrPointIndexRange>(simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::AttrPointIndexRangeView,
        addr().index()});
}

void Validity::setSimpleGeometry(model_ptr<Geometry> geom) {
    ensureMaterialized();
    if (geom) {
        data_->geomDescrType_ = SimpleGeometry;
        data_->geomDescr_.simpleGeometry_ = geom->addr();
    }
    else {
        data_->geomDescrType_ = NoGeometry;
        data_->geomDescr_.simpleGeometry_ = {};
    }
    data_->geomOffsetType_ = InvalidOffsetType;
}

model_ptr<Geometry> Validity::simpleGeometry() const
{
    if (!data_) {
        return {};
    }
    if (data_->geomDescrType_ != SimpleGeometry) {
        return {};
    }
    return model().resolve<Geometry>(data_->geomDescr_.simpleGeometry_);
}

void Validity::setFeatureTransition(
    model_ptr<FeatureId> const& fromFeatureId,
    TransitionEnd fromConnectedEnd,
    model_ptr<FeatureId> const& toFeatureId,
    TransitionEnd toConnectedEnd,
    uint32_t transitionNumber)
{
    ensureMaterialized();
    if (!fromFeatureId || !toFeatureId) {
        raise("Validity::setFeatureTransition requires both from/to feature IDs.");
    }
    data_->geomDescrType_ = FeatureTransition;
    data_->geomOffsetType_ = InvalidOffsetType;
    data_->referencedGeometryName_ = 0;
    data_->featureAddress_ = {};
    data_->geomDescr_.featureTransition_ = {
        fromFeatureId->addr(),
        toFeatureId->addr(),
        transitionNumber,
        packTransitionEnds(fromConnectedEnd, toConnectedEnd),
    };
}

void Validity::setFeatureTransition(
    model_ptr<Feature> const& fromFeature,
    TransitionEnd fromConnectedEnd,
    model_ptr<Feature> const& toFeature,
    TransitionEnd toConnectedEnd,
    uint32_t transitionNumber)
{
    if (!fromFeature || !toFeature) {
        raise("Validity::setFeatureTransition requires both from/to features.");
    }
    setFeatureTransition(
        fromFeature->id(),
        fromConnectedEnd,
        toFeature->id(),
        toConnectedEnd,
        transitionNumber);
}

model_ptr<FeatureId> Validity::transitionFromFeatureId() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return {};
    }
    return model().resolve<FeatureId>(data_->geomDescr_.featureTransition_.fromFeatureId_);
}

model_ptr<FeatureId> Validity::transitionToFeatureId() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return {};
    }
    return model().resolve<FeatureId>(data_->geomDescr_.featureTransition_.toFeatureId_);
}

model_ptr<Feature> Validity::transitionFromFeature() const
{
    auto featureId = transitionFromFeatureId();
    if (!featureId || featureId->mapId() != model().mapId()) {
        return {};
    }
    return model().find(featureId->typeId(), featureId->keyValuePairs());
}

model_ptr<Feature> Validity::transitionToFeature() const
{
    auto featureId = transitionToFeatureId();
    if (!featureId || featureId->mapId() != model().mapId()) {
        return {};
    }
    return model().find(featureId->typeId(), featureId->keyValuePairs());
}

std::optional<Validity::TransitionEnd> Validity::transitionFromConnectedEnd() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return std::nullopt;
    }
    return unpackFromConnectedEnd(data_->geomDescr_.featureTransition_.connectedEnds_);
}

std::optional<Validity::TransitionEnd> Validity::transitionToConnectedEnd() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return std::nullopt;
    }
    return unpackToConnectedEnd(data_->geomDescr_.featureTransition_.connectedEnds_);
}

std::optional<uint32_t> Validity::transitionNumber() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return std::nullopt;
    }
    return data_->geomDescr_.featureTransition_.transitionNumber_;
}

SelfContainedGeometry Validity::computeGeometry(
    model_ptr<GeometryCollection> geometryCollection,
    std::string* error,
    uint32_t* transitionPivotIndex) const
{
    if (transitionPivotIndex) {
        *transitionPivotIndex = std::numeric_limits<uint32_t>::max();
    }
    if (geometryDescriptionType() == SimpleGeometry) {
        // Return the self-contained geometry points.
        auto simpleGeom = simpleGeometry();
        assert(simpleGeom);
        return applyDirectionToGeometry(simpleGeom->toSelfContained(), direction());
    }

    if (auto index = attrPointIndex()) {
        return applyDirectionToGeometry(
            {{index->sequence()->pointAt(index->index())}, {}, GeomType::Points},
            direction());
    }

    if (auto range = attrPointIndexRange()) {
        auto start = range->start();
        auto end = range->end();
        if (end < start) {
            std::swap(start, end);
        }
        auto points = range->sequence()->points(start, end);
        return applyDirectionToGeometry(
            {points, {}, points.size() > 1 ? GeomType::Line : GeomType::Points},
            direction());
    }

    const auto referencedName = geometryName();

    if (geometryDescriptionType() == FeatureTransition) {
        auto fromFeatureId = transitionFromFeatureId();
        auto toFeatureId = transitionToFeatureId();
        auto fromFeature = transitionFromFeature();
        auto toFeature = transitionToFeature();
        auto fromConnectedEnd = transitionFromConnectedEnd();
        auto toConnectedEnd = transitionToConnectedEnd();
        if (!fromFeatureId || !toFeatureId || !fromConnectedEnd || !toConnectedEnd) {
            if (error) {
                *error = "Malformed semantic feature transition validity.";
            }
            return {};
        }
        if (!fromFeature) {
            if (error) {
                *error = fmt::format(
                    "Transition source geometry is unavailable for feature {}.",
                    fromFeatureId->toString());
            }
            return {};
        }
        if (!toFeature) {
            if (error) {
                *error = fmt::format(
                    "Transition target geometry is unavailable for feature {}.",
                    toFeatureId->toString());
            }
            return {};
        }

        auto fromSegment = resolveTransitionSegment(fromFeature, *fromConnectedEnd, referencedName);
        if (!fromSegment) {
            if (error) {
                *error = fmt::format(
                    "Failed to resolve transition source geometry for feature {}.",
                    fromFeature->id()->toString());
            }
            return {};
        }

        auto toSegment = resolveTransitionSegment(toFeature, *toConnectedEnd, referencedName);
        if (!toSegment) {
            if (error) {
                *error = fmt::format(
                    "Failed to resolve transition target geometry for feature {}.",
                    toFeature->id()->toString());
            }
            return {};
        }

        std::vector<Point> points;
        points.reserve(
            fromSegment->innerToOuter_.size() +
            toSegment->innerToOuter_.size() + 1);
        // Preserve the exact incoming road slice in traversal order.
        points.insert(
            points.end(),
            fromSegment->innerToOuter_.rbegin(),
            fromSegment->innerToOuter_.rend());

        // The pivot prefers the hosting feature geometry (for example an
        // intersection point) and otherwise falls back to the connected road
        // endpoints. Keep the pivot as an explicit point even if it coincides
        // with an endpoint: the subset metadata must identify one unambiguous
        // split between incoming and outgoing road slices.
        Point pivot;
        bool appendedHostMidpoint = false;
        if (geometryCollection) {
            geometryCollection->forEachGeometry([&](auto&& geom) {
                if (geom->geomType() != GeomType::Points || geom->numPoints() == 0) {
                    return true;
                }
                pivot = geom->pointAt(0);
                appendedHostMidpoint = true;
                return false;
            });
        }
        if (!appendedHostMidpoint) {
            auto const& fromInner = fromSegment->innerToOuter_.front();
            auto const& toInner = toSegment->innerToOuter_.front();
            if (pointsCoincide(fromInner, toInner)) {
                pivot = fromInner;
            } else {
                pivot = Point{
                    (fromInner.x + toInner.x) * 0.5,
                    (fromInner.y + toInner.y) * 0.5,
                    (fromInner.z + toInner.z) * 0.5,
                };
            }
        }
        if (transitionPivotIndex) {
            *transitionPivotIndex = static_cast<uint32_t>(points.size());
        }
        points.push_back(pivot);
        points.insert(
            points.end(),
            toSegment->innerToOuter_.begin(),
            toSegment->innerToOuter_.end());
        return {points, {}, points.size() > 1 ? GeomType::Line : GeomType::Points};
    }

    // If this validity references some feature directly,
    // use the geometry collection of that feature.
    if (auto featureIdNode = featureId()) {
        auto feature = model().find(featureIdNode->typeId(), featureIdNode->keyValuePairs());
        if (feature) {
            geometryCollection = feature->geomOrNull();
        } else {
            log().warn("Could not find feature by its ID {}", featureIdNode->toString());
        }
    }

    if (!geometryCollection) {
        return {};
    }

    // Line-based validities resolve by semantic geometry name, never by
    // presentation fidelity or loading stage.
    auto geometry = resolveLineGeometry(geometryCollection, referencedName);

    if (!geometry) {
        if (error) {
            if (referencedName) {
                *error = fmt::format(
                    "Failed to find line geometry named '{}' for validity.",
                    *referencedName);
            } else {
                *error = "Failed to find a line geometry for validity.";
            }
        }
        return {};
    }

    // No geometry description from the attribute - just return the whole
    // geometry from the collection.
    if (geometryDescriptionType() == NoGeometry) {
        return applyDirectionToGeometry(geometry->toSelfContained(), direction());
    }

    // Now we have OffsetPointValidity or OffsetRangeValidity
    auto offsetType = geometryOffsetType();
    if (offsetType == InvalidOffsetType) {
        if (error) {
            *error = fmt::format("Encountered InvalidOffsetType in Validity::computeGeometry.");
        }
        return {};
    }

    Point startPoint;
    std::optional<Point> endPoint;
    if (geometryDescriptionType() == OffsetPointValidity) {
        startPoint = *offsetPoint();
    }
    else {
        auto rangePair = *offsetRange();
        startPoint = rangePair.first;
        endPoint = rangePair.second;
    }

    // Handle GeoPosOffset (a range of the geometry line, bound by two positions).
    if (offsetType == GeoPosOffset) {
        auto points = geometry->pointsFromPositionBound(startPoint, endPoint);
        return applyDirectionToGeometry({points, {}, geomTypeForOffsetValidity(geometryDescriptionType(), points)}, direction());
    }

    // Handle BufferOffset (a range of the geometry bound by two indices).
    if (offsetType == BufferOffset) {
        auto startPointIndex = static_cast<uint32_t>(startPoint.x);
        if (startPointIndex >= geometry->numPoints()) {
            if (error) {
                *error = fmt::format("Validity::computeGeometry: Start point index {} is out-of-bounds.",
                    startPointIndex);
            }
            return {};
        }

        auto endPointIndex = endPoint ? static_cast<uint32_t>(endPoint->x) : startPointIndex;
        if (endPointIndex >= geometry->numPoints()) {
            if (error) {
                *error = fmt::format("Validity::computeGeometry: End point index {} is out-of-bounds.",
                    startPointIndex);
            }
            return {};
        }

        if (endPointIndex < startPointIndex) {
            // Buffer indices are treated as an inclusive range independent of
            // authoring order, so normalize to ascending storage order first.
            std::swap(startPointIndex, endPointIndex);
        }

        std::vector<Point> points;
        for (auto pointIndex = startPointIndex; pointIndex <= endPointIndex; ++pointIndex) {
            points.emplace_back(geometry->pointAt(pointIndex));
        }
        return applyDirectionToGeometry({points, {}, geomTypeForOffsetValidity(geometryDescriptionType(), points)}, direction());
    }

    // Handle RelativeLengthOffset (a percentage range of the geometry).
    //  - we convert the percentages to length values, and then fall through to MetricLengthOffset.
    if (offsetType == RelativeLengthOffset) {
        auto lineLength = geometry->length();
        startPoint.x *= lineLength;
        if (endPoint) {
            endPoint->x *= lineLength;
        }
    }

    // Handle MetricLengthOffset (a length range of the geometry in meters).
    if (offsetType == MetricLengthOffset || offsetType == RelativeLengthOffset) {
        auto points = geometry->pointsFromLengthBound(startPoint.x, endPoint ? std::optional<double>(endPoint->x) : std::optional<double>());
        return applyDirectionToGeometry({points, {}, geomTypeForOffsetValidity(geometryDescriptionType(), points)}, direction());
    }

    if (error) {
        *error = fmt::format("Validity::computeGeometry: Unexpected invalid offsetType {}", static_cast<uint32_t>(offsetType));
    }
    return {};
}

TileFeatureLayer& MultiValidity::featureLayer()
{
    return static_cast<TileFeatureLayer&>(model());
}

model_ptr<Validity>
MultiValidity::newPoint(
    Point pos,
    std::optional<std::string_view> geometryName,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetPoint(pos);
    result->setGeometryName(geometryName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newRange(
    Point start,
    Point end,
    std::optional<std::string_view> geometryName,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetRange(start, end);
    result->setGeometryName(geometryName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newAttrPointIndex(
    model_ptr<AttrPointSequence> const& sequence,
    uint32_t index,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setAttrPointIndex(sequence, index);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newAttrPointIndexRange(
    model_ptr<AttrPointSequence> const& sequence,
    uint32_t start,
    uint32_t end,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setAttrPointIndexRange(sequence, start, end);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newPoint(
    Validity::GeometryOffsetType offsetType,
    double pos,
    std::optional<std::string_view> geometryName,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetPoint(offsetType, pos);
    result->setGeometryName(geometryName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newPoint(
    Validity::GeometryOffsetType offsetType,
    int32_t pos,
    std::optional<std::string_view> geometryName,
    Validity::Direction direction)
{
    return newPoint(offsetType, static_cast<double>(pos), geometryName, direction);
}

model_ptr<Validity> MultiValidity::newRange(
    Validity::GeometryOffsetType offsetType,
    double start,
    double end,
    std::optional<std::string_view> geometryName,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetRange(offsetType, start, end);
    result->setGeometryName(geometryName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newRange(
    Validity::GeometryOffsetType offsetType,
    int32_t start,
    int32_t end,
    std::optional<std::string_view> geometryName,
    Validity::Direction direction)
{
    return newRange(
        offsetType,
        static_cast<double>(start),
        static_cast<double>(end),
        geometryName,
        direction);
}

model_ptr<Validity>
MultiValidity::newGeometry(model_ptr<Geometry> geom, Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setSimpleGeometry(geom);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity>
MultiValidity::newFeatureId(model_ptr<FeatureId> const& featureId, Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setFeatureId(featureId);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity>
MultiValidity::newGeomName(std::string_view geometryName, Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setGeometryName(geometryName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newFeatureTransition(
    model_ptr<FeatureId> const& fromFeatureId,
    Validity::TransitionEnd fromConnectedEnd,
    model_ptr<FeatureId> const& toFeatureId,
    Validity::TransitionEnd toConnectedEnd,
    uint32_t transitionNumber,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setFeatureTransition(
        fromFeatureId,
        fromConnectedEnd,
        toFeatureId,
        toConnectedEnd,
        transitionNumber);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newFeatureTransition(
    model_ptr<Feature> const& fromFeature,
    Validity::TransitionEnd fromConnectedEnd,
    model_ptr<Feature> const& toFeature,
    Validity::TransitionEnd toConnectedEnd,
    uint32_t transitionNumber,
    Validity::Direction direction)
{
    if (!fromFeature || !toFeature) {
        raise("MultiValidity::newFeatureTransition requires both from/to features.");
    }
    return newFeatureTransition(
        fromFeature->id(),
        fromConnectedEnd,
        toFeature->id(),
        toConnectedEnd,
        transitionNumber,
        direction);
}

model_ptr<Validity> MultiValidity::newComplete(Validity::Direction direction)
{
    return newDirection(direction == Validity::Empty ? Validity::Both : direction);
}

model_ptr<Validity> MultiValidity::newDirection(Validity::Direction direction)
{
    auto const elementIndex = size();
    const auto simpleAddr = simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::SimpleValidity,
        static_cast<uint32_t>(direction)};
    appendInternal(model_ptr<simfil::ModelNode>::make(model_, simpleAddr));
    return featureLayer().resolve<Validity>(
        simpleAddr,
        encodeSimpleValidityOwner(members_, elementIndex));
}

ModelNode::Ptr MultiValidity::at(int64_t i) const
{
    if (i < 0 || i >= static_cast<int64_t>(storage_->size(members_))) {
        return {};
    }

    auto value = storage_->at(members_, static_cast<size_t>(i));
    if (!value) {
        return {};
    }

    auto const memberAddress = value->get();
    if (memberAddress.column() != TileFeatureLayer::ColumnId::SimpleValidity) {
        return ModelNode::Ptr::make(model_, memberAddress);
    }

    return ModelNode::Ptr::make(
        model_,
        memberAddress,
        encodeSimpleValidityOwner(members_, static_cast<uint32_t>(i)));
}

bool MultiValidity::iterate(ModelNode::IterCallback const& cb) const
{
    bool cont = true;
    auto resolveAndCb = simfil::Model::Lambda([&cb, &cont](auto&& node) { cont = cb(node); });
    for (int64_t index = 0; index < static_cast<int64_t>(size()); ++index) {
        auto value = at(index);
        if (!value) {
            return false;
        }
        model_->resolve(*value, resolveAndCb);
        if (!cont) {
            return false;
        }
    }
    return true;
}

template<>
model_ptr<MultiValidity> resolveInternal(
    simfil::res::tag<MultiValidity>,
    TileFeatureModelLayerBase const& model,
    ModelNode const& node)
{
    if (node.addr().column() != TileFeatureModelLayerBase::ColumnId::ValidityCollections) {
        raise("Cannot cast this node to a ValidityCollection.");
    }
    if (dynamic_cast<TileFeatureLayer const*>(&model) == nullptr) {
        raise("Validity collections are only supported by TileFeatureLayer.");
    }
    return MultiValidity(
        model.shared_from_this(),
        node.addr(),
        model.mpKey_);
}

}
