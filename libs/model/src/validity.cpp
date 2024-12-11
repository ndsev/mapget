#include "validity.h"
#include "mapget/log.h"
#include "stringpool.h"
#include "featurelayer.h"

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

model_ptr<FeatureId> Validity::featureId() const
{
    if (!data_->featureAddress_) {
        return {};
    }
    return model().resolveFeatureId(*ModelNode::Ptr::make(model_, data_->featureAddress_));
}

void Validity::setFeatureId(model_ptr<FeatureId> feature)
{
    if (!feature) {
        data_->featureAddress_ = {};
        return;
    }
    data_->featureAddress_ = feature->addr();
}

Validity::Validity(Validity::Data* data,
    simfil::ModelConstPtr layer,
    simfil::ModelNodeAddress a)
    : simfil::ProceduralObject<6, Validity, TileFeatureLayer>(std::move(layer), a), data_(data)
{
    if (data_->direction_)
        fields_.emplace_back(
            StringPool::DirectionStr,
            [](Validity const& self)
            {
                return model_ptr<simfil::ValueNode>::make(
                    directionToString(self.data_->direction_),
                    self.model_);
            });

    if (data_->geomDescrType_ == SimpleGeometry) {
        fields_.emplace_back(
            StringPool::GeometryStr,
            [](Validity const& self)
            {
                return ModelNode::Ptr::make(
                    self.model_,
                    std::get<ModelNodeAddress>(self.data_->geomDescr_));
            });
        return;
    }

    if (data_->referencedGeomName_) {
        fields_.emplace_back(
            StringPool::GeometryNameStr,
            [](Validity const& self)
            {
                auto resolvedString =
                    self.model().strings()->resolve(self.data_->referencedGeomName_);
                return model_ptr<simfil::ValueNode>::make(
                    resolvedString ?
                        *resolvedString :
                        std::string_view("<Could not resolve geometry name>"),
                    self.model_);
            });
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
                switch (self.data_->geomOffsetType_) {
                case InvalidOffsetType: return ModelNode::Ptr{};
                case GeoPosOffset:
                    return ModelNode::Ptr::make(
                        self.model_,
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
        auto& [start, end] = std::get<Data::Range>(data_->geomDescr_);
        exposeOffsetPoint(StringPool::StartStr, 1, start);
        exposeOffsetPoint(StringPool::EndStr, 2, end);
    }
    else if (data_->geomDescrType_ == OffsetPointValidity) {
        exposeOffsetPoint(StringPool::PointStr, 0, std::get<Point>(data_->geomDescr_));
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
    data_->direction_ = v;
}

Validity::Direction Validity::direction() const
{
    return data_->direction_;
}

Validity::GeometryOffsetType Validity::geometryOffsetType() const
{
    return data_->geomOffsetType_;
}

Validity::GeometryDescriptionType Validity::geometryDescriptionType() const
{
    return data_->geomDescrType_;
}

void Validity::setGeometryName(const std::optional<std::string_view>& geometryName) {
    data_->referencedGeomName_ = geometryName ? model().strings()->emplace(*geometryName) : Empty;
}

std::optional<std::string_view> Validity::geometryName() const
{
    if (!data_->referencedGeomName_) {
        return {};
    }
    return model().strings()->resolve(data_->referencedGeomName_);
}

void Validity::setOffsetPoint(Point pos) {
    data_->geomDescrType_ = OffsetPointValidity;
    data_->geomOffsetType_ = GeoPosOffset;
    data_->geomDescr_ = pos;
}

void Validity::setOffsetPoint(Validity::GeometryOffsetType offsetType, double pos) {
    assert(offsetType != InvalidOffsetType && offsetType != GeoPosOffset);
    data_->geomDescrType_ = OffsetPointValidity;
    data_->geomOffsetType_ = offsetType;
    data_->geomDescr_ = Point{pos, 0, 0};
}

std::optional<Point> Validity::offsetPoint() const
{
    if (data_->geomDescrType_ != OffsetPointValidity) {
        return {};
    }
    return std::get<Point>(data_->geomDescr_);
}

void Validity::setOffsetRange(Point start, Point end) {
    data_->geomDescrType_ = OffsetRangeValidity;
    data_->geomOffsetType_ = GeoPosOffset;
    data_->geomDescr_ = std::make_pair(start, end);
}

void Validity::setOffsetRange(Validity::GeometryOffsetType offsetType, double start, double end) {
    assert(offsetType != InvalidOffsetType && offsetType != GeoPosOffset);
    data_->geomDescrType_ = OffsetRangeValidity;
    data_->geomOffsetType_ = offsetType;
    data_->geomDescr_ = std::make_pair(Point{start, 0, 0}, Point{end, 0, 0});
}

std::optional<std::pair<Point, Point>> Validity::offsetRange() const
{
    if (data_->geomDescrType_ != OffsetRangeValidity) {
        return {};
    }
    return std::get<Data::Range>(data_->geomDescr_);
}

void Validity::setSimpleGeometry(model_ptr<Geometry> geom) {
    if (geom) {
        data_->geomDescrType_ = SimpleGeometry;
    }
    else {
        data_->geomDescrType_ = NoGeometry;
    }
    data_->geomOffsetType_ = InvalidOffsetType;
    data_->geomDescr_ = geom->addr();
}

model_ptr<Geometry> Validity::simpleGeometry() const
{
    if (data_->geomDescrType_ != SimpleGeometry) {
        return {};
    }
    return model().resolveGeometry(*ModelNode::Ptr::make(model_, std::get<ModelNodeAddress>(data_->geomDescr_)));
}

SelfContainedGeometry Validity::computeGeometry(
    model_ptr<GeometryCollection> geometryCollection,
    std::string* error) const
{
    if (data_->geomDescrType_ == SimpleGeometry) {
        // Return the self-contained geometry points.
        auto simpleGeom = simpleGeometry();
        assert(simpleGeom);
        return simpleGeom->toSelfContained();
    }

    // If this validity references some feature directly,
    // use the geometry collection of that feature.
    if (data_->featureAddress_) {
        auto feature = model().find(featureId()->typeId(), featureId()->keyValuePairs());
        if (feature) {
            geometryCollection = feature->geomOrNull();
        } else {
            mapget::log().warn("Could not find feature by its ID {}", featureId()->toString());
        }
    }

    if (!geometryCollection) {
        return {};
    }

    // Find a geometry with a matching name.
    auto requiredGeomName = geometryName();
    model_ptr<Geometry> geometry;
    geometryCollection->forEachGeometry([&requiredGeomName, &geometry](auto&& geom){
        if (geom->name() == requiredGeomName && geom->geomType() == GeomType::Line) {
            geometry = geom;
            return false;  // abort iteration.
        }
        return true;
    });

    if (!geometry) {
        if (error) {
            *error = fmt::format("Failed to find geometry for {}", requiredGeomName ? *requiredGeomName : "");
        }
        return {};
    }

    // No geometry description from the attribute - just return the whole
    // geometry from the collection.
    if (data_->geomDescrType_ == NoGeometry) {
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
    if (data_->geomDescrType_ == OffsetPointValidity) {
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

    // Handle BufferOffset (a range of the goemetry bound by two indices).
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
MultiValidity::newPoint(Point pos, std::string_view geomName, Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setOffsetPoint(pos);
    result->setGeometryName(geomName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newRange(
    Point start,
    Point end,
    std::string_view geomName,
    Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setOffsetRange(start, end);
    result->setGeometryName(geomName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newPoint(
    Validity::GeometryOffsetType offsetType,
    double pos,
    std::string_view geomName,
    Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setOffsetPoint(offsetType, pos);
    result->setGeometryName(geomName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newPoint(
    Validity::GeometryOffsetType offsetType,
    int32_t pos,
    std::string_view geomName,
    Validity::Direction direction)
{
    return newPoint(offsetType, static_cast<double>(pos), geomName, direction);
}

model_ptr<Validity> MultiValidity::newRange(
    Validity::GeometryOffsetType offsetType,
    double start,
    double end,
    std::string_view geomName,
    Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setOffsetRange(offsetType, start, end);
    result->setGeometryName(geomName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> MultiValidity::newRange(
    Validity::GeometryOffsetType offsetType,
    int32_t start,
    int32_t end,
    std::string_view geomName,
    Validity::Direction direction)
{
    return newRange(
        offsetType,
        static_cast<double>(start),
        static_cast<double>(end),
        geomName,
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

model_ptr<Validity> MultiValidity::newDirection(Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setDirection(direction);
    append(result);
    return result;
}

}
