#pragma once

#include "mapget/model/pointseries.h"

#include <map>
#include <type_traits>

namespace mapget::point_schema
{
/** One layer's native producer declarations. No JSON, evaluated records or foreign IDs are
 * retained. */
struct Declarations
{
    std::vector<PointSeries> series;
    std::map<std::string, PointSeries::Codec, std::less<>> codecs;
    bool empty() const { return series.empty() && codecs.empty(); }
};

// V10 wire order. The JSON projection uses the same members by name.
template <class U, class F>
void memberPointers(F&& field)
{
#define POINT_MEMBER(key) field(#key, &U::key##_)
    if constexpr (std::is_same_v<U, PointSeries>) {
        POINT_MEMBER(featureType);
        POINT_MEMBER(attributeLayer);
        POINT_MEMBER(attribute);
        POINT_MEMBER(geometryName);
        POINT_MEMBER(geometryType);
        POINT_MEMBER(omittedValidityCovers);
        POINT_MEMBER(measure);
        POINT_MEMBER(assignmentMeasure);
        POINT_MEMBER(records);
        POINT_MEMBER(parallel);
        POINT_MEMBER(position);
        POINT_MEMBER(sampling);
        POINT_MEMBER(fields);
        POINT_MEMBER(markerTag);
        POINT_MEMBER(branches);
        POINT_MEMBER(branchTarget);
        POINT_MEMBER(branchRecords);
        POINT_MEMBER(hostTarget);
        POINT_MEMBER(alternative);
    }
    else if constexpr (std::is_same_v<U, PointSeries::Position>) {
        POINT_MEMBER(kind);
        POINT_MEMBER(field);
        POINT_MEMBER(bits);
        POINT_MEMBER(bitsFromAttribute);
        POINT_MEMBER(end);
        POINT_MEMBER(intervalAxis);
        POINT_MEMBER(includeStart);
        POINT_MEMBER(includeEnd);
        POINT_MEMBER(fromAssignmentStart);
        POINT_MEMBER(reverse);
        POINT_MEMBER(distanceScale);
        POINT_MEMBER(vertexMeasures);
    }
    else if constexpr (std::is_same_v<U, PointSeries::Field>) {
        POINT_MEMBER(name);
        POINT_MEMBER(path);
        POINT_MEMBER(unit);
        POINT_MEMBER(codec);
    }
    else if constexpr (std::is_same_v<U, PointSeries::Codec>) {
        POINT_MEMBER(scale);
        POINT_MEMBER(offset);
        POINT_MEMBER(tableOnly);
        POINT_MEMBER(codes);
    }
    else {
        static_assert(std::is_same_v<U, PointSeries::Codec::Code>);
        POINT_MEMBER(raw);
        POINT_MEMBER(value);
        POINT_MEMBER(status);
        POINT_MEMBER(lower);
        POINT_MEMBER(upper);
    }
#undef POINT_MEMBER
}

template <class T, class F>
void members(T& value, F&& field)
{
    memberPointers<std::remove_const_t<T>>(
        [&](auto key, auto member) { field(key, value.*member); });
}

// Bounds apply to both the generated declaration API and length-prefixed artifact input.
constexpr size_t MaxText = 4096;
template <class T>
constexpr size_t maxCount = std::is_same_v<T, std::string> ?
    64 :
    std::is_same_v<T, PointSeries::Field> ?
    256 :
    std::is_same_v<T, PointSeries::Codec::Code> ?
    65536 :
    UINT16_MAX;

}  // namespace mapget::point_schema
