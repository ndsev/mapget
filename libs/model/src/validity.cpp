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
    std::optional<uint32_t> referencedStage)
{
    if (!geometryCollection) {
        return {};
    }
    return geometryCollection->geometryOfTypeAtPreferredStage(GeomType::Line, referencedStage);
}

struct TransitionSegment
{
    Point outer_;
    Point inner_;
};

/** Compare two validity points with a small tolerance to absorb numeric noise. */
bool pointsCoincide(Point const& left, Point const& right)
{
    return left.distanceTo(right) < 1e-9;
}

/** Resolve the endpoint segment that participates in a semantic feature transition. */
std::optional<TransitionSegment> resolveTransitionSegment(
    model_ptr<Feature> const& feature,
    Validity::TransitionEnd connectedEnd,
    std::optional<uint32_t> referencedStage)
{
    if (!feature) {
        return std::nullopt;
    }

    auto geometry = resolveLineGeometry(feature->geomOrNull(), referencedStage);
    if (!geometry || geometry->numPoints() == 0) {
        return std::nullopt;
    }

    const auto numPoints = geometry->numPoints();
    const auto innerIndex = connectedEnd == Validity::End ? numPoints - 1U : 0U;
    auto outerIndex = innerIndex;
    auto const innerPoint = geometry->pointAt(innerIndex);
    if (connectedEnd == Validity::End) {
        // Transition endpoints may be repeated at the tail, so walk backwards
        // until the first distinct point to get a visible outgoing segment.
        for (auto pointIndex = innerIndex; pointIndex-- > 0;) {
            if (!pointsCoincide(geometry->pointAt(pointIndex), innerPoint)) {
                outerIndex = pointIndex;
                break;
            }
        }
    } else {
        // Likewise, repeated points at the head must be skipped when entering
        // a transition from the start of a polyline.
        for (auto pointIndex = innerIndex + 1U; pointIndex < numPoints; ++pointIndex) {
            if (!pointsCoincide(geometry->pointAt(pointIndex), innerPoint)) {
                outerIndex = pointIndex;
                break;
            }
        }
    }
    return TransitionSegment{
        geometry->pointAt(outerIndex),
        innerPoint,
    };
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

/** Map a stored geometry stage back to the optional exported `geometryName`. */
std::optional<std::string_view> geometryNameForStage(
    TileFeatureLayer const& model,
    std::optional<uint32_t> geometryStage)
{
    if (!geometryStage || !model.layerInfo()) {
        return std::nullopt;
    }
    auto const& layerInfo = *model.layerInfo();
    if (*geometryStage <= layerInfo.highFidelityStage_) {
        // High-fidelity geometries intentionally omit a stage label in JSON.
        return std::nullopt;
    }
    if (*geometryStage >= layerInfo.stageLabels_.size()) {
        return std::nullopt;
    }
    auto const& label = layerInfo.stageLabels_.at(*geometryStage);
    if (label.empty()) {
        return std::nullopt;
    }
    return label;
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
    if (!data_->featureAddress_) {
        return {};
    }
    return model().resolve<FeatureId>(data_->featureAddress_);
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

    if (auto geometryName = geometryNameForStage(model(), geometryStage())) {
        fields_.emplace_back(
            StringPool::GeometryNameStr,
            [geometryName](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(*geometryName, self.model_);
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
                auto fromFeature = self.transitionFromFeature();
                return model_ptr<simfil::ValueNode>::make(
                    fromFeature ? fromFeature->id()->toString() : std::string{},
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
                auto toFeature = self.transitionToFeature();
                return model_ptr<simfil::ValueNode>::make(
                    toFeature ? toFeature->id()->toString() : std::string{},
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

void Validity::setGeometryStage(std::optional<uint32_t> geometryStage)
{
    ensureMaterialized();
    if (!geometryStage) {
        data_->referencedStage_ = Data::InvalidReferencedStage;
        return;
    }
    if (*geometryStage > static_cast<uint32_t>(std::numeric_limits<int8_t>::max())) {
        raise("Validity::setGeometryStage: stage is out of int8_t range.");
    }
    data_->referencedStage_ = static_cast<int8_t>(*geometryStage);
}

std::optional<uint32_t> Validity::geometryStage() const
{
    if (!data_) {
        return {};
    }
    if (data_->referencedStage_ == Data::InvalidReferencedStage) {
        return {};
    }
    return static_cast<uint32_t>(data_->referencedStage_);
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
    model_ptr<Feature> const& fromFeature,
    TransitionEnd fromConnectedEnd,
    model_ptr<Feature> const& toFeature,
    TransitionEnd toConnectedEnd,
    uint32_t transitionNumber)
{
    ensureMaterialized();
    if (!fromFeature || !toFeature) {
        raise("Validity::setFeatureTransition requires both from/to features.");
    }
    data_->geomDescrType_ = FeatureTransition;
    data_->geomOffsetType_ = InvalidOffsetType;
    data_->referencedStage_ = Data::InvalidReferencedStage;
    data_->featureAddress_ = {};
    data_->geomDescr_.featureTransition_ = {
        fromFeature->addr(),
        toFeature->addr(),
        transitionNumber,
        packTransitionEnds(fromConnectedEnd, toConnectedEnd),
    };
}

model_ptr<Feature> Validity::transitionFromFeature() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return {};
    }
    return model().resolve<Feature>(data_->geomDescr_.featureTransition_.fromFeature_);
}

model_ptr<Feature> Validity::transitionToFeature() const
{
    if (!data_ || data_->geomDescrType_ != FeatureTransition) {
        return {};
    }
    return model().resolve<Feature>(data_->geomDescr_.featureTransition_.toFeature_);
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
    std::optional<uint32_t> defaultGeometryStage) const
{
    if (geometryDescriptionType() == SimpleGeometry) {
        // Return the self-contained geometry points.
        auto simpleGeom = simpleGeometry();
        assert(simpleGeom);
        return applyDirectionToGeometry(simpleGeom->toSelfContained(), direction());
    }

    const auto referencedStage = geometryStage().has_value()
        ? geometryStage()
        : defaultGeometryStage;

    if (geometryDescriptionType() == FeatureTransition) {
        auto fromFeature = transitionFromFeature();
        auto toFeature = transitionToFeature();
        auto fromConnectedEnd = transitionFromConnectedEnd();
        auto toConnectedEnd = transitionToConnectedEnd();
        if (!fromFeature || !toFeature || !fromConnectedEnd || !toConnectedEnd) {
            if (error) {
                *error = "Failed to resolve semantic feature transition validity.";
            }
            return {};
        }

        auto fromSegment = resolveTransitionSegment(fromFeature, *fromConnectedEnd, referencedStage);
        if (!fromSegment) {
            if (error) {
                *error = fmt::format(
                    "Failed to resolve transition source geometry for feature {}.",
                    fromFeature->id()->toString());
            }
            return {};
        }

        auto toSegment = resolveTransitionSegment(toFeature, *toConnectedEnd, referencedStage);
        if (!toSegment) {
            if (error) {
                *error = fmt::format(
                    "Failed to resolve transition target geometry for feature {}.",
                    toFeature->id()->toString());
            }
            return {};
        }

        std::vector<Point> points;
        points.reserve(3);
        auto appendIfNotDuplicate = [&](Point const& point)
        {
            if (points.empty() || !pointsCoincide(points.back(), point)) {
                points.emplace_back(point);
            }
        };
        // Render the transition as outer-from -> shared transition midpoint -> outer-to.
        // The midpoint prefers the hosting feature geometry (for example an intersection point)
        // and otherwise falls back to the connected road endpoints.
        appendIfNotDuplicate(fromSegment->outer_);
        bool appendedHostMidpoint = false;
        if (geometryCollection) {
            geometryCollection->forEachGeometry([&](auto&& geom) {
                if (geom->geomType() != GeomType::Points || geom->numPoints() == 0) {
                    return true;
                }
                appendIfNotDuplicate(geom->pointAt(0));
                appendedHostMidpoint = true;
                return false;
            });
        }
        if (!appendedHostMidpoint) {
            if (pointsCoincide(fromSegment->inner_, toSegment->inner_)) {
                appendIfNotDuplicate(fromSegment->inner_);
            } else {
                appendIfNotDuplicate(Point{
                    (fromSegment->inner_.x + toSegment->inner_.x) * 0.5,
                    (fromSegment->inner_.y + toSegment->inner_.y) * 0.5,
                    (fromSegment->inner_.z + toSegment->inner_.z) * 0.5,
                });
            }
        }
        appendIfNotDuplicate(toSegment->outer_);
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

    // Line-based validities always resolve against the preferred line geometry
    // for the chosen stage, not against arbitrary polygons or meshes.
    auto geometry = resolveLineGeometry(geometryCollection, referencedStage);

    if (!geometry) {
        if (error) {
            if (referencedStage) {
                *error = fmt::format(
                    "Failed to find line geometry for validity stage {}.",
                    *referencedStage);
            } else {
                *error = "Failed to find line geometry for validity at the configured high-fidelity stage.";
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
MultiValidity::newPoint(Point pos, std::optional<uint32_t> geometryStage, Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetPoint(pos);
    result->setGeometryStage(geometryStage);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newRange(
    Point start,
    Point end,
    std::optional<uint32_t> geometryStage,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetRange(start, end);
    result->setGeometryStage(geometryStage);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newPoint(
    Validity::GeometryOffsetType offsetType,
    double pos,
    std::optional<uint32_t> geometryStage,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetPoint(offsetType, pos);
    result->setGeometryStage(geometryStage);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newPoint(
    Validity::GeometryOffsetType offsetType,
    int32_t pos,
    std::optional<uint32_t> geometryStage,
    Validity::Direction direction)
{
    return newPoint(offsetType, static_cast<double>(pos), geometryStage, direction);
}

model_ptr<Validity> MultiValidity::newRange(
    Validity::GeometryOffsetType offsetType,
    double start,
    double end,
    std::optional<uint32_t> geometryStage,
    Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setOffsetRange(offsetType, start, end);
    result->setGeometryStage(geometryStage);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newRange(
    Validity::GeometryOffsetType offsetType,
    int32_t start,
    int32_t end,
    std::optional<uint32_t> geometryStage,
    Validity::Direction direction)
{
    return newRange(
        offsetType,
        static_cast<double>(start),
        static_cast<double>(end),
        geometryStage,
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
MultiValidity::newGeomStage(uint32_t geometryStage, Validity::Direction direction)
{
    auto result = featureLayer().newValidity();
    result->setGeometryStage(geometryStage);
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
    auto result = featureLayer().newValidity();
    result->setFeatureTransition(
        fromFeature,
        fromConnectedEnd,
        toFeature,
        toConnectedEnd,
        transitionNumber);
    result->setDirection(direction);
    append(result);
    return result;
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
