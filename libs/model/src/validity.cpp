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

Validity::Validity(
    mapget::Validity::Data* data,
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

    auto exposeOffsetPoint = [this](StringId fieldName, uint32_t pointIndex, mapget::Point const& p)
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
