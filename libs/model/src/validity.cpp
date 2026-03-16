#include "validity.h"
#include "mapget/log.h"
#include "stringpool.h"
#include "featurelayer.h"

#include <limits>

namespace mapget
{

namespace
{
std::string_view directionToString(Validity::Direction const& d)
{
    switch (d) {
    case Validity::Empty: return "EMPTY";
    case Validity::Positive: return "POSITIVE";
    case Validity::Negative: return "NEGATIVE";
    case Validity::Both: return "BOTH";
    case Validity::None: return "NONE";
    }
    return "?";
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

    auto upgradedAddress = model().materializeSimpleValidity(simpleAddress, simpleDirection_);
    auto upgraded = model().resolve<Validity>(upgradedAddress);
    if (!upgraded || !upgraded->data_) {
        raise("Failed to materialize simple validity.");
    }
    data_ = upgraded->data_;
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
    : simfil::ProceduralObject<6, Validity, TileFeatureLayer>(std::move(layer), a, key),
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

Validity::Validity(Validity::Data* data,
    simfil::ModelConstPtr layer,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::ProceduralObject<6, Validity, TileFeatureLayer>(std::move(layer), a, key),
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

    if (data_->geomDescrType_ == SimpleGeometry) {
        fields_.emplace_back(
            StringPool::GeometryStr,
            [](Validity const& self)
            {
                return self.model().resolve(
                    self.data_->geomDescr_.simpleGeometry_);
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
                return model_ptr<simfil::ValueNode>::make(
                    self.featureId()->toString(),
                    self.model_);
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

SelfContainedGeometry Validity::computeGeometry(
    model_ptr<GeometryCollection> geometryCollection,
    std::string* error) const
{
    if (geometryDescriptionType() == SimpleGeometry) {
        // Return the self-contained geometry points.
        auto simpleGeom = simpleGeometry();
        assert(simpleGeom);
        return simpleGeom->toSelfContained();
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

    const auto referencedStage = geometryStage();

    // Resolve validity geometry by stage first (if specified), then by line type.
    model_ptr<Geometry> geometry;
    geometryCollection->forEachGeometry([&](auto&& geom){
        if (referencedStage) {
            const auto geometryStage = geom->model().stage().value_or(0U);
            if (geometryStage != *referencedStage) {
                return true;
            }
        }
        if (!geometry && geom->geomType() == GeomType::Line) {
            geometry = geom;
            return false;
        }
        return true;
    });

    if (!geometry) {
        if (error) {
            if (referencedStage) {
                *error = fmt::format(
                    "Failed to find line geometry for validity stage {}.",
                    *referencedStage);
            } else {
                *error = "Failed to find line geometry for validity.";
            }
        }
        return {};
    }

    // No geometry description from the attribute - just return the whole
    // geometry from the collection.
    if (geometryDescriptionType() == NoGeometry) {
        return geometry->toSelfContained();
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
        return {points, points.size() > 1 ? GeomType::Line : GeomType::Points};
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
            std::swap(startPointIndex, endPointIndex);
        }

        std::vector<Point> points;
        for (auto pointIndex = startPointIndex; pointIndex <= endPointIndex; ++pointIndex) {
            points.emplace_back(geometry->pointAt(pointIndex));
        }
        return {points, points.size() > 1 ? GeomType::Line : GeomType::Points};
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
        return {points, points.size() > 1 ? GeomType::Line : GeomType::Points};
    }

    if (error) {
        *error = fmt::format("Validity::computeGeometry: Unexpected invalid offsetType {}", static_cast<uint32_t>(offsetType));
    }
    return {};
}

model_ptr<Validity>
MultiValidity::newPoint(Point pos, std::optional<uint32_t> geometryStage, Validity::Direction direction)
{
    auto result = model().newValidity();
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
    auto result = model().newValidity();
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
    auto result = model().newValidity();
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
    auto result = model().newValidity();
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
    auto result = model().newValidity();
    result->setSimpleGeometry(geom);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity>
MultiValidity::newFeatureId(model_ptr<FeatureId> const& featureId, Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setFeatureId(featureId);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity>
MultiValidity::newGeomStage(uint32_t geometryStage, Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setGeometryStage(geometryStage);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newDirection(Validity::Direction direction)
{
    const auto simpleAddr = simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::SimpleValidity,
        static_cast<uint32_t>(direction)};
    if (auto upgradedAddress = model().upgradedSimpleValidityAddress(simpleAddr)) {
        appendInternal(model_ptr<simfil::ModelNode>::make(model_, *upgradedAddress));
        return model().resolve<Validity>(*upgradedAddress);
    }
    appendInternal(model_ptr<simfil::ModelNode>::make(model_, simpleAddr));
    return model().resolve<Validity>(simpleAddr);
}

}
