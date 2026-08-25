#include "mapget/model/attrpoint.h"

#include "mapget/model/feature.h"
#include "mapget/model/featurelayer.h"
#include "mapget/model/stringpool.h"
#include "mapget/log.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace mapget
{

namespace
{
/** Return a scalar node owned by the same model as the supplied mapget node. */
template <typename Value>
simfil::ModelNode::Ptr valueNode(
    simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer> const& owner,
    Value&& value)
{
    return model_ptr<simfil::ValueNode>::make(
        std::forward<Value>(value),
        owner.model().shared_from_this());
}

/** Visit every array item in ascending order. */
bool iterateArray(
    simfil::ModelNode const& array,
    simfil::ModelNode::IterCallback const& callback)
{
    for (uint32_t index = 0; index < array.size(); ++index) {
        auto item = array.at(static_cast<int64_t>(index));
        if (item && !callback(*item)) {
            return false;
        }
    }
    return true;
}

/** Return the dynamic SIMFIL field order for one attribute point. */
std::vector<simfil::StringId> attrPointFields(AttrPoint const& point)
{
    std::vector<simfil::StringId> result{
        StringPool::IndexStr,
        StringPool::PointStr,
    };
    if (point.sourceDataReferences()) {
        result.push_back(StringPool::SourceDataStr);
    }
    return result;
}

/** Return the dynamic SIMFIL field order for one sequence. */
std::vector<simfil::StringId> sequenceFields(AttrPointSequence const& sequence)
{
    std::vector<simfil::StringId> result{
        StringPool::FeatureIdStr,
        StringPool::GeometryIndexStr,
    };
    if (sequence.geometry()->name()) {
        result.push_back(StringPool::GeometryNameStr);
    }
    result.push_back(StringPool::AttrPointsStr);
    result.push_back(StringPool::PositionCountStr);
    if (sequence.sourceDataReferences()) {
        result.push_back(StringPool::SourceDataStr);
    }
    return result;
}

/** Find the first inserted point whose logical index is not below the target. */
uint32_t lowerBoundAttrPoint(
    model_ptr<AttrPointArray> const& points,
    uint32_t pointCount,
    uint32_t logicalIndex)
{
    uint32_t first = 0;
    uint32_t count = pointCount;
    while (count > 0) {
        auto const step = count / 2U;
        auto const candidate = first + step;
        if (points->attrPointAt(candidate)->index() < logicalIndex) {
            first = candidate + 1U;
            count -= step + 1U;
        }
        else {
            count = step;
        }
    }
    return first;
}

/** Visit an inclusive logical range by merging shape and inserted-point storage once. */
template <typename Callback>
bool forEachSequencePoint(
    AttrPointSequence const& sequence,
    uint32_t start,
    uint32_t end,
    Callback const& callback)
{
    auto const attrPoints = sequence.attrPoints();
    auto const attrPointCount = sequence.attrPointCount();
    auto attrPointIndex = lowerBoundAttrPoint(attrPoints, attrPointCount, start);
    auto geometryPointIndex = start - attrPointIndex;
    auto const geometry = sequence.geometry();

    for (auto logicalIndex = start;; ++logicalIndex) {
        Point point;
        if (attrPointIndex < attrPointCount) {
            auto const attrPoint = attrPoints->attrPointAt(attrPointIndex);
            if (attrPoint->index() == logicalIndex) {
                point = attrPoint->point();
                ++attrPointIndex;
            }
            else {
                point = geometry->pointAt(geometryPointIndex++);
            }
        }
        else {
            point = geometry->pointAt(geometryPointIndex++);
        }

        if (!callback(point)) {
            return false;
        }
        if (logicalIndex == end) {
            return true;
        }
    }
}
}

AttrPoint::AttrPoint(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
          std::move(model),
          address,
          key)
{
}

uint32_t AttrPoint::index() const
{
    return model().attrPointData(addr().index()).index_;
}

Point AttrPoint::point() const
{
    auto const& stored = model().attrPointData(addr().index()).point_;
    auto const anchor = model().geometryAnchor();
    return Point{
        anchor.x + static_cast<double>(stored.x),
        anchor.y + static_cast<double>(stored.y),
        anchor.z + static_cast<double>(stored.z)};
}

model_ptr<SourceDataReferenceCollection> AttrPoint::sourceDataReferences() const
{
    auto const address = model().attrPointData(addr().index()).sourceData_;
    return address
        ? model().resolve<SourceDataReferenceCollection>(address)
        : model_ptr<SourceDataReferenceCollection>{};
}

nlohmann::json AttrPoint::toJson() const
{
    auto result = nlohmann::json::object({
        {"index", index()},
        {"point", point()},
    });
    if (auto sourceData = sourceDataReferences()) {
        result["_sourceData"] = sourceData->toJson();
    }
    return result;
}

simfil::ValueType AttrPoint::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttrPoint::at(int64_t fieldIndex) const
{
    return fieldIndex >= 0 && static_cast<size_t>(fieldIndex) < attrPointFields(*this).size()
        ? get(keyAt(fieldIndex))
        : simfil::ModelNode::Ptr{};
}

simfil::ModelNode::Ptr AttrPoint::get(simfil::StringId const& field) const
{
    switch (field) {
    case StringPool::IndexStr:
        return valueNode(*this, static_cast<int64_t>(index()));
    case StringPool::PointStr:
        // Generic SIMFIL has no geographic-position scalar. Keep inspection
        // readable while strict JSON retains the lossless coordinate array.
        return valueNode(*this, point().toString());
    case StringPool::SourceDataStr:
        return sourceDataReferences();
    default:
        return {};
    }
}

simfil::StringId AttrPoint::keyAt(int64_t fieldIndex) const
{
    auto const fields = attrPointFields(*this);
    return fieldIndex >= 0 && static_cast<size_t>(fieldIndex) < fields.size()
        ? fields[static_cast<size_t>(fieldIndex)]
        : simfil::StringId{};
}

uint32_t AttrPoint::size() const
{
    return static_cast<uint32_t>(attrPointFields(*this).size());
}

bool AttrPoint::iterate(IterCallback const& callback) const
{
    return iterateArray(*this, callback);
}

AttrPointArray::AttrPointArray(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
          std::move(model),
          address,
          key)
{
}

model_ptr<AttrPoint> AttrPointArray::attrPointAt(uint32_t index) const
{
    auto const& sequence = model().attrPointSequenceData(addr().index());
    if (index >= sequence.attrPointCount_) {
        raiseFmt(
            "AttrPoint index {} is out of range for a sequence with {} inserted points.",
            index,
            sequence.attrPointCount_);
    }
    return model().resolve<AttrPoint>(simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::AttrPoints,
        sequence.firstAttrPoint_ + index});
}

simfil::ValueType AttrPointArray::type() const
{
    return simfil::ValueType::Array;
}

simfil::ModelNode::Ptr AttrPointArray::at(int64_t index) const
{
    return index >= 0 && static_cast<uint64_t>(index) < size()
        ? attrPointAt(static_cast<uint32_t>(index))
        : simfil::ModelNode::Ptr{};
}

uint32_t AttrPointArray::size() const
{
    return model().attrPointSequenceData(addr().index()).attrPointCount_;
}

bool AttrPointArray::iterate(IterCallback const& callback) const
{
    return iterateArray(*this, callback);
}

AttrPointSequence::AttrPointSequence(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
          std::move(model),
          address,
          key)
{
}

model_ptr<FeatureId> AttrPointSequence::featureId() const
{
    return model().resolve<FeatureId>(
        model().attrPointSequenceData(addr().index()).featureId_);
}

model_ptr<Geometry> AttrPointSequence::geometry() const
{
    return model().resolve<Geometry>(
        model().attrPointSequenceData(addr().index()).geometry_);
}

uint32_t AttrPointSequence::geometryIndex() const
{
    auto const host = model().find(featureId()->toString());
    if (!host) {
        raiseFmt(
            "AttrPointSequence {} references missing feature '{}'.",
            addr().index(),
            featureId()->toString());
    }

    auto geometries = host->geomOrNull();
    if (!geometries) {
        raiseFmt(
            "AttrPointSequence {} references feature '{}' without geometry.",
            addr().index(),
            featureId()->toString());
    }

    uint32_t result = 0;
    bool found = false;
    geometries->forEachGeometry([&](model_ptr<Geometry> const& candidate) {
        if (candidate->addr().value_ == geometry()->addr().value_) {
            found = true;
            return false;
        }
        ++result;
        return true;
    });
    if (!found) {
        raiseFmt(
            "AttrPointSequence {} geometry is not attached to feature '{}'.",
            addr().index(),
            featureId()->toString());
    }
    return result;
}

model_ptr<AttrPointArray> AttrPointSequence::attrPoints() const
{
    return model().resolve<AttrPointArray>(simfil::ModelNodeAddress{
        TileFeatureLayer::ColumnId::AttrPointArrayView,
        addr().index()});
}

uint32_t AttrPointSequence::attrPointCount() const
{
    return model().attrPointSequenceData(addr().index()).attrPointCount_;
}

uint32_t AttrPointSequence::positionCount() const
{
    auto const shapePointCount = geometry()->numPoints();
    if (shapePointCount > std::numeric_limits<uint32_t>::max() - attrPointCount()) {
        raise("AttrPointSequence position count exceeds uint32_t range.");
    }
    return static_cast<uint32_t>(shapePointCount) + attrPointCount();
}

Point AttrPointSequence::pointAt(uint32_t logicalIndex) const
{
    if (logicalIndex >= positionCount()) {
        raiseFmt(
            "AttrPointSequence index {} is out of range for {} positions.",
            logicalIndex,
            positionCount());
    }

    Point result;
    forEachSequencePoint(*this, logicalIndex, logicalIndex, [&](Point const& point) {
        result = point;
        return false;
    });
    return result;
}

std::vector<Point> AttrPointSequence::points(uint32_t start, uint32_t end) const
{
    if (start > end) {
        raiseFmt("AttrPointSequence range start {} exceeds end {}.", start, end);
    }
    if (end >= positionCount()) {
        raiseFmt(
            "AttrPointSequence range {}..{} is out of range for {} positions.",
            start,
            end,
            positionCount());
    }

    std::vector<Point> result;
    result.reserve(static_cast<size_t>(end - start) + 1U);
    forEachSequencePoint(*this, start, end, [&](Point const& point) {
        result.push_back(point);
        return true;
    });
    return result;
}

bool AttrPointSequence::isAttrPoint(uint32_t logicalIndex) const
{
    if (logicalIndex >= positionCount()) {
        return false;
    }
    auto const points = attrPoints();
    auto const candidate = lowerBoundAttrPoint(points, attrPointCount(), logicalIndex);
    return candidate < attrPointCount() &&
        points->attrPointAt(candidate)->index() == logicalIndex;
}

double AttrPointSequence::metricOffsetAt(uint32_t logicalIndex) const
{
    if (logicalIndex >= positionCount()) {
        raiseFmt(
            "AttrPointSequence index {} is out of range for {} positions.",
            logicalIndex,
            positionCount());
    }
    double result = 0.0;
    Point previous;
    bool first = true;
    forEachSequencePoint(*this, 0, logicalIndex, [&](Point const& point) {
        if (!first) {
            result += previous.geographicDistanceTo(point);
        }
        previous = point;
        first = false;
        return true;
    });
    return result;
}

model_ptr<AttrPoint> AttrPointSequence::appendAttrPoint(
    uint32_t index,
    Point const& point,
    model_ptr<SourceDataReferenceCollection> const& sourceData)
{
    return model().appendAttrPoint(addr().index(), index, point, sourceData);
}

model_ptr<SourceDataReferenceCollection> AttrPointSequence::sourceDataReferences() const
{
    auto const address = model().attrPointSequenceData(addr().index()).sourceData_;
    return address
        ? model().resolve<SourceDataReferenceCollection>(address)
        : model_ptr<SourceDataReferenceCollection>{};
}

void AttrPointSequence::setSourceDataReferences(
    model_ptr<SourceDataReferenceCollection> const& sourceData)
{
    if (sourceData && sourceData->owningModel().get() != &model()) {
        raise("AttrPointSequence source-data references must belong to its TileFeatureLayer.");
    }
    model().attrPointSequenceData(addr().index()).sourceData_ =
        sourceData ? sourceData->addr() : simfil::ModelNodeAddress{};
}

nlohmann::json AttrPointSequence::toJson() const
{
    auto result = nlohmann::json::object({
        {"id", addr().index()},
        {"featureId", featureId()->toJson()},
        {"geometryIndex", geometryIndex()},
        {"attrPoints", nlohmann::json::array()},
    });
    if (auto name = geometry()->name()) {
        result["geometryName"] = *name;
    }
    for (uint32_t index = 0; index < attrPointCount(); ++index) {
        result["attrPoints"].push_back(attrPoints()->attrPointAt(index)->toJson());
    }
    if (auto sourceData = sourceDataReferences()) {
        result["_sourceData"] = sourceData->toJson();
    }
    return result;
}

simfil::ValueType AttrPointSequence::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttrPointSequence::at(int64_t fieldIndex) const
{
    return fieldIndex >= 0 && static_cast<size_t>(fieldIndex) < sequenceFields(*this).size()
        ? get(keyAt(fieldIndex))
        : simfil::ModelNode::Ptr{};
}

simfil::ModelNode::Ptr AttrPointSequence::get(simfil::StringId const& field) const
{
    switch (field) {
    case StringPool::FeatureIdStr:
        return featureId();
    case StringPool::GeometryIndexStr:
        return valueNode(*this, static_cast<int64_t>(geometryIndex()));
    case StringPool::GeometryNameStr:
        if (auto name = geometry()->name()) {
            return valueNode(*this, *name);
        }
        return {};
    case StringPool::AttrPointsStr:
        return attrPoints();
    case StringPool::PositionCountStr:
        return valueNode(*this, static_cast<int64_t>(positionCount()));
    case StringPool::SourceDataStr:
        return sourceDataReferences();
    default:
        return {};
    }
}

simfil::StringId AttrPointSequence::keyAt(int64_t fieldIndex) const
{
    auto const fields = sequenceFields(*this);
    return fieldIndex >= 0 && static_cast<size_t>(fieldIndex) < fields.size()
        ? fields[static_cast<size_t>(fieldIndex)]
        : simfil::StringId{};
}

uint32_t AttrPointSequence::size() const
{
    return static_cast<uint32_t>(sequenceFields(*this).size());
}

bool AttrPointSequence::iterate(IterCallback const& callback) const
{
    return iterateArray(*this, callback);
}

AttrPointSequenceReference::AttrPointSequenceReference(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
          std::move(model),
          address,
          key)
{
}

model_ptr<AttrPointSequence> AttrPointSequenceReference::sequence() const
{
    return model().attrPointSequenceAt(addr().index());
}

nlohmann::json AttrPointSequenceReference::toJson() const
{
    return nlohmann::json::object({
        {"$mapgetAttrPointSequence", addr().index()},
    });
}

simfil::ValueType AttrPointSequenceReference::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr AttrPointSequenceReference::at(int64_t index) const
{
    return index == 0 ? get(StringPool::MapgetAttrPointSequenceStr) : simfil::ModelNode::Ptr{};
}

simfil::ModelNode::Ptr AttrPointSequenceReference::get(simfil::StringId const& field) const
{
    return field == StringPool::MapgetAttrPointSequenceStr
        ? valueNode(*this, static_cast<int64_t>(addr().index()))
        : simfil::ModelNode::Ptr{};
}

simfil::StringId AttrPointSequenceReference::keyAt(int64_t index) const
{
    return index == 0 ? StringPool::MapgetAttrPointSequenceStr : simfil::StringId{};
}

uint32_t AttrPointSequenceReference::size() const
{
    return 1;
}

bool AttrPointSequenceReference::iterate(IterCallback const& callback) const
{
    return iterateArray(*this, callback);
}

}  // namespace mapget
