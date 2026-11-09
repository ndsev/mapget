#pragma once

#include "simfil/function.h"
#include "simfil/typed-meta-type.h"
#include "point.h"

using simfil::Error;
using simfil::Result;
using simfil::FnInfo;
using simfil::Context;
using simfil::ExprPtr;
using simfil::ResultFn;
using simfil::Value;

/** Simfil GeoJSON Extension */
namespace mapget
{

/* Custom Operator Names */
static constexpr auto OP_NAME_WITHIN     = "within";
static constexpr auto OP_NAME_CONTAINS   = "contains";
static constexpr auto OP_NAME_INTERSECTS = "intersects";

struct BBox;
struct LineString;
struct Polygon;

/** Axis-aligned bounds for planar x/y predicates; z is ignored and corners may be unordered. */
struct BBox
{
    Point p1, p2;

    void extend(const Point& p);

    auto edges() const -> LineString;
    auto normalized() const -> BBox;

    auto contains(const BBox& b) const -> bool;
    auto contains(const Point& p) const -> bool;
    auto contains(const LineString& p) const -> bool;
    auto contains(const Polygon& p) const -> bool;

    auto intersects(const BBox& b) const -> bool;
    auto intersects(const LineString& p) const -> bool;

    auto operator==(const BBox&) const -> bool;
    auto toString() const -> std::string;
};

/** Ordered planar x/y segments; no geodesic projection or antimeridian wrapping is applied. */
struct LineString
{
    std::vector<Point> points;

    auto bbox() const -> BBox;

    /** Signed area in coordinate units squared, treating the last point as joined to the first. */
    auto linear_ring_signed_area() const -> double;

    auto intersects(const BBox&) const -> bool;
    /** Exact planar interior-point test; segment endpoints are excluded. */
    auto intersects(const Point&) const -> bool;
    auto intersects(const LineString&) const -> bool;
    auto intersects(const Polygon&) const -> bool;

    auto operator==(const LineString&) const -> bool;
    auto toString() const -> std::string;
};

/** Planar rings with the exterior first and holes afterward; area ignores ring winding. */
struct Polygon
{
    std::vector<LineString> polys;

    auto bbox() const -> BBox;
    auto area() const -> double;

    auto contains(const BBox&) const -> bool;
    auto contains(const Point&) const -> bool;

    /** Test the supplied vertices; segments between them are not checked for leaving the polygon.
     */
    auto contains(const LineString&) const -> bool;

    auto intersects(const BBox&) const -> bool;
    auto intersects(const LineString&) const -> bool;
    auto intersects(const Polygon&) const -> bool;

    auto operator==(const Polygon&) const -> bool;
    auto toString() const -> std::string;
};

/** Convert geometry nodes into transient 2D query values, discarding elevation.
 * Reads either the input's geometry member or the input itself, and expands one
 * GeometryCollection level. Multi geometries use result emissions rather than a
 * separate Multi type; this is not a lossless GeoJSON import/export interface.
 */
class GeoFn : public simfil::Function
{
public:
    static GeoFn Fn;

    GeoFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Construct transient query points from two single numeric arguments. */
class PointFn : public simfil::Function
{
public:
    static PointFn Fn;

    PointFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Construct query bounds from one geometry value, two points or four numbers. */
class BBoxFn : public simfil::Function
{
public:
    static BBoxFn Fn;

    BBoxFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/** Collect argument emissions into a transient x/y point sequence. */
class LineStringFn : public simfil::Function
{
public:
    static LineStringFn Fn;

    LineStringFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};

/* TODO: Not implemented
class PolygonFn : public simfil::Function
{
public:
    static PolygonFn Fn;

    PolygonFn() = default;

    auto ident() const -> const FnInfo& override;
    auto eval(Context, const Value&, const std::vector<ExprPtr>&, const ResultFn&) const -> tl::expected<Result, Error> override;
};
*/

/** Simfil Meta-Types */
namespace meta
{

class PointType : public simfil::TypedMetaType<Point>
{
public:
    static PointType Type;

    PointType();

    auto make(double x, double y) -> Value;

    auto unaryOp(std::string_view op, const Point& p) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Point& p, const Value& r) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Value& l, const Point& r) const -> tl::expected<Value, Error> override;
    auto unpack(const Point& p, std::function<bool(Value)> res) const -> tl::expected<void, Error> override;
};

class BBoxType : public simfil::TypedMetaType<BBox>
{
public:
    static BBoxType Type;

    BBoxType();

    auto make(BBox) -> Value;
    auto make(double x1, double y1, double x2, double y2) -> Value;

    auto unaryOp(std::string_view op, const BBox& b) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const BBox& b, const Value& r) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Value& l, const BBox& r) const -> tl::expected<Value, Error> override;
    auto unpack(const BBox& b, std::function<bool(Value)> res) const -> tl::expected<void, Error> override;
};

class LineStringType : public simfil::TypedMetaType<LineString>
{
public:
    static LineStringType Type;

    LineStringType();

    auto make(std::vector<Point> pts) -> Value;

    auto unaryOp(std::string_view op, const LineString& ls) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const LineString& ls, const Value& r) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Value& l, const LineString& r) const -> tl::expected<Value, Error> override;
    auto unpack(const LineString& ls, std::function<bool(Value)> res) const -> tl::expected<void, Error> override;
};

class PolygonType : public simfil::TypedMetaType<Polygon>
{
public:
    static PolygonType Type;

    PolygonType();

    auto make(LineString outer) -> Value;
    auto make(std::vector<LineString> full) -> Value;

    auto unaryOp(std::string_view op, const Polygon&) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Polygon& l, const Value& r) const -> tl::expected<Value, Error> override;
    auto binaryOp(std::string_view op, const Value& l, const Polygon& r) const -> tl::expected<Value, Error> override;
    auto unpack(const Polygon&, std::function<bool(Value)> res) const -> tl::expected<void, Error> override;
};

}

}
