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


std::optional<glm::dvec3>
projectPointOnLine(const glm::dvec3& point, const glm::dvec3& a, const glm::dvec3& b)
{
    // Check if A and B are the same point (zero-length line segment).
    if (a == b) {
        return std::nullopt; // Projection is undefined for a zero-length line segment.
    }

    // Calculate 2d vectors AB and AP (from A to Point).
    glm::dvec2 AB = b - a;
    glm::dvec2 AP = point - a;

    // Calculate the squared length of AB to check for numerical stability.
    double lengthSquaredAB = glm::dot(AB, AB);
    if (lengthSquaredAB == 0.0) {
        return std::nullopt; // Avoid division by zero.
    }

    // Project AP onto AB using dot product.
    double dotProduct = glm::dot(AP, AB);
    double projectionFactor = dotProduct / lengthSquaredAB;

    // Check if projection extends beyond A or B.
    if (projectionFactor < 0 || projectionFactor > 1) {
        return std::nullopt; // Projection outside the segment AB.
    }

    // Calculate the 3d projection point.
    return projectionFactor * (b - a);
}

double calcLineLengthInM(const mapget::Geometry &lineGeom)
{
    auto length = 0.0;
    if (lineGeom.numPoints() < 2) return length;
    for (auto i = 0; i < lineGeom.numPoints()-1; ++i)
    {
        auto pos = lineGeom.pointAt(i);
        auto posNext = lineGeom.pointAt(i+1);
        length += pos.geographicDistanceTo(posNext);
    }
    return length;
}

std::vector<Point> geometryFromPositionBound(
    model_ptr<Geometry>& lineGeom,
    const Point& start,
    const std::optional<Point>& end)
{
    if (!lineGeom)
        return {};

    // Find the line segments which are closest to start/end.
    uint32_t startClosestIndex = 0;
    uint32_t endClosestIndex = 0;
    double startClosestDistance = std::numeric_limits<double>::max();
    double endClosestDistance = std::numeric_limits<double>::max();
    glm::dvec3 startOffsetFromClosest;
    glm::dvec3 endOffsetFromClosest;

    // Function which works generically for start or end in the loop below.
    // Updates index, offset and distance if the line segment [newIndex->newIndex+1]
    // is closer to the point than [index->index+1].
    auto updateClosestIndex = [&lineGeom](auto& index, auto& offset, auto& distance, auto newIndex, auto const& point) {
        auto linePointA = lineGeom->pointAt(newIndex);
        auto linePointB = lineGeom->pointAt(newIndex+1);
        auto newDistance = glm::distance(glm::vec2(linePointA), glm::vec2(point));
        auto newOffset = projectPointOnLine(point, linePointA, linePointB);
        if (newDistance < distance && newOffset) {
            distance = newDistance;
            index = newIndex;
            offset = *newOffset;
        }
    };

    // Loop which actually finds the closest indices of the line shape points.
    for (auto i = 0; i < lineGeom->numPoints()-1; ++i) {
        updateClosestIndex(startClosestIndex, startOffsetFromClosest, startClosestDistance, i, start);
        if (end)
            updateClosestIndex(endClosestIndex, endOffsetFromClosest, endClosestDistance, i, *end);
    }

    // Make sure that end comes after start.
    if (end && endClosestIndex < startClosestIndex) {
        std::swap(startClosestIndex, endClosestIndex);
        std::swap(startClosestDistance, endClosestDistance);
        std::swap(startOffsetFromClosest, endOffsetFromClosest);
    }

    // Assemble geometry - just a point if end is not given.
    std::vector<Point> result;

    // Add the start point.
    auto startClosestPoint = lineGeom->pointAt(startClosestIndex);
    result.emplace_back(
         startClosestPoint.x + startOffsetFromClosest.x,
         startClosestPoint.y + startOffsetFromClosest.y,
         startClosestPoint.z + startOffsetFromClosest.z);

    // Add additional line points.
    if (end) {
        for (auto i = startClosestIndex + 1; i <= endClosestIndex; ++i) {
            result.emplace_back(lineGeom->pointAt(i));
        }
        auto endClosestPoint = lineGeom->pointAt(endClosestIndex);
        result.emplace_back(
            endClosestPoint.x + endOffsetFromClosest.x,
            endClosestPoint.y + endOffsetFromClosest.y,
            endClosestPoint.z + endOffsetFromClosest.z);
    }

    return result;
}

std::vector<Point> geometryFromLengthBound(
    model_ptr<Geometry>& lineGeom,
    double start,
    std::optional<double> end)
{
    if (!lineGeom)
        return {};

    // Make sure that end comes after start.
    if (end && *end < start) {
        std::swap(start, *end);
    }
    int32_t innerIndexStart = 0, innerIndexEnd = 0;
    auto startPos = lineGeom->pointAt(innerIndexStart), endPos = lineGeom->pointAt(innerIndexEnd);
    double coveredLength = 0;
    bool startReached = false;
    for (auto i = 0; i < lineGeom->numPoints()-1; ++i)
    {
        innerIndexStart = 0;
        auto pos = lineGeom->pointAt(i);
        auto posNext = lineGeom->pointAt(i+1);
        auto dist = pos.geographicDistanceTo(posNext);
        coveredLength += dist;

        if (!startReached && start <= coveredLength)
        {
            innerIndexStart = i;
            // TODO: Determine, if this fast calculation (opposed to using Wgs84::move) is accurate enough.
            auto lerp = static_cast<double>(dist - (coveredLength - start)) / static_cast<double>(dist);
            startPos = pos + (posNext - pos) * lerp;
            startReached = true;
            if (!end)
                break;
        }
        if (startReached && end && *end <= coveredLength) {
            innerIndexEnd = i;
            auto lerp = static_cast<double>(dist - (coveredLength - *end)) / static_cast<double>(dist);
            endPos = pos + (posNext - pos) * lerp;
            break;
        }
    }

    // Assemble geometry - just a point if end is not given.
    std::vector<Point> result;

    // Add the start point.
    result.emplace_back(startPos);

    // Add additional line points.
    if (end) {
        for (auto i = innerIndexStart + 1; i <= innerIndexEnd; ++i) {
            result.emplace_back(lineGeom->pointAt(i));
        }
        result.emplace_back(endPos);
    }

    return result;
}
}

Validity::Validity(
    Validity::Data* data,
    simfil::ModelConstPtr layer,
    simfil::ModelNodeAddress a)
    : simfil::ProceduralObject<2, Validity, TileFeatureLayer>(std::move(layer), a), data_(data)
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
                case AbsoluteLengthOffset:
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

std::vector<Point> Validity::computeGeometry(
    const model_ptr<GeometryCollection>& geometryCollection,
    std::string* error)
{
    if (data_->geomDescrType_ == SimpleGeometry) {
        // Return the self-contained geometry points.
        auto simpleGeom = simpleGeometry();
        assert(simpleGeom);
        std::vector<Point> result;
        result.reserve(simpleGeom->numPoints());
        simpleGeom->forEachPoint([&result](auto&& point){
             result.emplace_back(point);
             return true;
        });
        return result;
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
        std::vector<Point> result;
        result.reserve(geometry->numPoints());
        geometry->forEachPoint([&result](auto&& point){
             result.emplace_back(point);
             return true;
        });
        return result;
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
        return geometryFromPositionBound(geometry, startPoint, endPoint);
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

        std::vector<Point> result;
        for (auto pointIndex = startPointIndex; pointIndex <= endPointIndex; ++pointIndex) {
            result.emplace_back(geometry->pointAt(pointIndex));
        }
        return result;
    }

    // Handle RelativeLengthOffset (a percentage range of the geometry).
    //  - we convert the percentages to length values, and then fall through to AbsoluteLengthOffset.
    if (offsetType == RelativeLengthOffset) {
        auto lineLength = calcLineLengthInM(*geometry);
        startPoint.x *= lineLength;
        if (endPoint) {
            endPoint->x *= lineLength;
        }
    }

    // Handle AbsoluteLengthOffset (a length range of the geometry in meters).
    if (offsetType == AbsoluteLengthOffset || offsetType == RelativeLengthOffset) {
        return geometryFromLengthBound(geometry, startPoint.x, endPoint ? std::optional<double>(endPoint->x) : std::optional<double>());
    }

    if (error) {
        *error = fmt::format("Validity::computeGeometry: Unexpected invalid offsetType {}", static_cast<uint32_t>(offsetType));
    }
    return {};
}

model_ptr<Validity>
ValidityCollection::newValidity(Point pos, std::string_view geomName, Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setOffsetPoint(pos);
    result->setGeometryName(geomName);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> ValidityCollection::newValidity(
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

model_ptr<Validity> ValidityCollection::newValidity(
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

model_ptr<Validity> ValidityCollection::newValidity(
    Validity::GeometryOffsetType offsetType,
    int32_t pos,
    std::string_view geomName,
    Validity::Direction direction)
{
    return newValidity(offsetType, static_cast<double>(pos), geomName, direction);
}

model_ptr<Validity> ValidityCollection::newValidity(
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

model_ptr<Validity> ValidityCollection::newValidity(
    Validity::GeometryOffsetType offsetType,
    int32_t start,
    int32_t end,
    std::string_view geomName,
    Validity::Direction direction)
{
    return newValidity(offsetType, static_cast<double>(start), static_cast<double>(end), geomName, direction);
}

model_ptr<Validity>
ValidityCollection::newValidity(model_ptr<Geometry> geom, Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setSimpleGeometry(geom);
    result->setDirection(direction);
    append(result);
    return result;
}

model_ptr<Validity> ValidityCollection::newValidity(Validity::Direction direction)
{
    auto result = model().newValidity();
    result->setDirection(direction);
    append(result);
    return result;
}

}
