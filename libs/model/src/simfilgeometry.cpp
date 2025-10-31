#include "simfilgeometry.h"
#include "stringpool.h"

#include "simfil/value.h"
#include "simfil/result.h"
#include "simfil/operator.h"

#include "fmt/core.h"
#include "mapget/log.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <string>

using namespace std::string_literals;
using namespace simfil;

namespace mapget
{

// Parse a single Point object from a node
static auto parseCoordinatePoint(const ModelNode& node, Point& pt)
{
    auto nx = node.at(0);
    auto ny = node.at(1);
    if (!nx || !ny)
        return false;

    auto vx = nx->value();
    auto vy = ny->value();
    auto [xok, fx] = getNumeric<double>(vx);
    if (!xok)
        return false;
    auto [yok, fy] = getNumeric<double>(vy);
    if (!yok)
        return false;

    pt.x = fx;
    pt.y = fy;
    return true;
}

template <typename ResultFn>
static auto handleNullResult(Context& ctx, const ResultFn& res, std::optional<Error>& capturedError) -> Result
{
    if (auto resVal = res(ctx, Value::null()); !resVal) {
        capturedError = resVal.error();
        return Result::Stop;
    } else {
        return resVal.value();
    }
}

static auto inline min(double a, double b)
{
    return std::min<double>(a, b);
}

static auto inline max(double a, double b)
{
    return std::max<double>(a, b);
}

/**
 * Returns if point q is on _segment_ ab
 */
static auto pointOnSegment(const Point& a, const Point& q, const Point& b)
{
    return q.x <= max(a.x, b.x) && q.x >= min(a.x, b.x) &&
           q.y <= max(a.y, b.y) && q.y >= min(a.y, b.y);
}

static auto dot(const Point& a, const Point& b)
{
    return a.x*b.x + a.y*b.y;
}

static auto cross(const Point& a, const Point& b)
{
    return a.x*b.y - a.y*b.x;
}

static auto pointOnLine(const Point& a, const Point& q, const Point& b)
{
    auto v = Point{a.x - q.x, a.y - q.y};
    auto w = Point{q.x - b.x, q.y - b.y};

    return cross(v, w) == 0 && dot(v, w) > 0;
}

/**
 * 0 => Collinear
 * 1 => CW
 * 2 => CCW
 */
static auto orientation(const Point& a, const Point& q, const Point& b)
{
    auto val = (q.y - a.y) * (b.x - q.x) -
               (q.x - a.x) * (b.y - q.y);

    if (val == 0)
        return 0;
    return val > 0 ? 1 : 2;
}

static auto lineIntersects(const Point& a, const Point& b, const Point& c, const Point& d)
{
    // Algorithm from: https://www.geeksforgeeks.org/check-if-two-given-line-segments-intersect/?ref=lbp
    auto o1 = orientation(a, b, c);
    auto o2 = orientation(a, b, d);
    auto o3 = orientation(c, d, a);
    auto o4 = orientation(c, d, b);

    if (o1 != o2 && o3 != o4)
        return true;

    if (o1 == 0 && pointOnSegment(a, c, b)) return true;
    if (o2 == 0 && pointOnSegment(a, d, b)) return true;
    if (o3 == 0 && pointOnSegment(c, a, d)) return true;
    if (o4 == 0 && pointOnSegment(c, b, d)) return true;

    return false;
}

auto LineString::intersects(const Point& p) const -> bool
{
    for (auto i = 1; i < points.size(); ++i) {
        const auto& a = points[i-1];
        const auto& b = points[i-0];
        if (pointOnLine(a, p, b))
            return true;
    }

    return false;
}

auto LineString::intersects(const LineString& o) const -> bool
{
    for (auto i = 1; i < points.size(); ++i) {
        const auto& a = points[i-1];
        const auto& b = points[i-0];

        for (auto j = 1; j < o.points.size(); ++j) {
            const auto& c = o.points[j-1];
            const auto& d = o.points[j-0];

            if (lineIntersects(a, b, c, d))
                return true;
        }
    }

    return false;
}

auto LineString::intersects(const BBox& b) const -> bool
{
    return b.intersects(*this);
}

auto LineString::intersects(const Polygon& p) const -> bool
{
    return p.intersects(*this);
}

auto LineString::operator==(const LineString& o) const -> bool = default;

auto LineString::toString() const -> std::string
{
    auto str = "["s;
    auto i = 0;
    for (const auto& p : points) {
        if (i++ > 0)
            str.push_back(',');
        str += p.toString();
    }

    return str + "]";
}

void BBox::extend(const Point& p)
{
    p1.x = min(p1.x, p.x);
    p1.y = min(p1.y, p.y);
    p2.x = max(p2.x, p.x);
    p2.y = max(p2.y, p.y);
}

auto BBox::normalized() const -> BBox
{
    auto minx = std::min<double>(p1.x, p2.x);
    auto maxx = std::max<double>(p1.x, p2.x);
    auto miny = std::min<double>(p1.y, p2.y);
    auto maxy = std::max<double>(p1.y, p2.y);

    return {{minx, miny}, {maxx, maxy}};
}

auto BBox::edges() const -> LineString
{
    LineString edges;
    edges.points = {p1, Point{p2.x, p1.y}, p2, Point{p1.x, p2.y}, p1};
    return edges;
}

auto BBox::contains(const Point& p) const -> bool
{
    auto norm = normalized();
    return norm.p1.x <= p.x && p.x <= norm.p2.x &&
           norm.p1.y <= p.y && p.y <= norm.p2.y;
}

auto BBox::contains(const BBox& b) const -> bool
{
    return contains(b.p1) && contains(b.p2);
}

auto BBox::contains(const LineString& p) const -> bool
{
    for (const auto& p : p.points) {
        if (!contains(p))
            return false;
    }

    return !p.points.empty();
}

auto BBox::contains(const Polygon& p) const -> bool
{
    if (!p.polys.empty())
        return contains(p.polys[0]);

    return false;
}

auto BBox::intersects(const BBox& o) const -> bool
{
    auto a = normalized();
    auto b = o.normalized();

    return b.p2.x >= a.p1.x && b.p1.x <= a.p2.x &&
           b.p2.y >= a.p1.y && b.p1.y <= a.p2.y;
}

auto BBox::intersects(const LineString& p) const -> bool
{
    auto pbb = p.bbox();
    if (!intersects(pbb))
        return false;

    if (contains(pbb))
        return true;

    return edges().intersects(p);
}

auto BBox::operator==(const BBox& o) const -> bool = default;

auto BBox::toString() const -> std::string
{
    return fmt::format("[{},{}]", p1.toString(), p2.toString());
}

auto Polygon::bbox() const -> BBox
{
    if (polys.empty())
        return {{0, 0}, {0, 0}};

    return polys[0].bbox();
}

auto Polygon::area() const -> double
{
    // The first LineString represents the exterior ring,
    // all following LineStringPool represent holes/interior rings.
    if (polys.empty())
        return 0.0;

    double a = std::abs(polys[0].linear_ring_signed_area());
    for (auto i = 1; i < polys.size(); ++i)
        a -= std::abs(polys[i].linear_ring_signed_area());

    return a;
}

auto LineString::bbox() const -> BBox
{
    if (points.empty())
        return {{0, 0}, {0, 0}};

    BBox bbox{points[0], points[0]};
    for (const auto& p : points)
        bbox.extend(p);

    return bbox;
}

auto LineString::linear_ring_signed_area() const -> double
{
    double a = 0;

    const auto n = points.size();
    for (auto i = 0; i < n; ++i) {
        const auto& p_prev = points[(i - 1) % n];
        const auto& p_next = points[(i + 1) % n];
        const auto& p_curr = points[i];

        a += p_curr.x * (p_next.y - p_prev.y);
    }

    return a / 2;
}

static auto pointInPoly(const LineString& edges, const Point& p)
{
    if (edges.points.size() <= 2)
        return false;

    if (!edges.bbox().contains(p))
        return false;

    /* Ray */
    const auto rb = Point{std::numeric_limits<double>::max(), p.y};

    size_t n = 0;
    for (auto i = 0; i < edges.points.size(); ++i) {
        if (edges.points[i] == p)
            return true; /* Point on edge */

        const auto& a = edges.points[i];
        const auto& b = edges.points[(i + 1) % edges.points.size()];

        if (lineIntersects(a, b, p, rb))
            n++;
    }

    return n % 2 != 0;
};

static auto pointsInPoly(const LineString& poly, const LineString& l)
{
    return std::ranges::all_of(
        l.points.cbegin(),
        l.points.cend(),
        [&](const auto& p) {
            return pointInPoly(poly, p);
        });
}

auto Polygon::contains(const Point& p) const -> bool
{
    if (polys.empty())
        return false;

    if (pointInPoly(polys[0], p)) {
        for (auto i = 1; i < polys.size(); ++i)
            if (pointInPoly(polys[i], p))
                return false;

        return true;
    }

    return false;
}

auto Polygon::contains(const BBox& b) const -> bool
{
    return contains(b.edges());
}

auto Polygon::contains(const LineString& l) const -> bool
{
    if (l.points.empty())
        return false;

    return std::ranges::all_of(
        l.points.cbegin(),
        l.points.cend(),
        [this](const auto& p) {
            return contains(p);
        });
}

auto Polygon::intersects(const BBox& b) const -> bool
{
    if (polys.empty())
        return false;

    if (polys[0].intersects(b)) {
        if (polys.size() > 1) {
            /* Check if the bounding box is inside a hole */
            auto edges = b.edges();
            for (auto i = 1; i < polys.size(); ++i) {
                if (pointsInPoly(polys[i], edges))
                    return false;
            }
        }

        return true;
    }

    return false;
}

auto Polygon::intersects(const LineString& l) const -> bool
{
    if (polys.empty())
        return false;

    for (const auto& p : l.points) {
        if (contains(p))
            return true;
    }

    return polys[0].intersects(l);
}

auto Polygon::intersects(const Polygon& l) const -> bool
{
    if (polys.empty())
        return false;

    return polys[0].intersects(l);
}

auto Polygon::operator==(const Polygon& o) const -> bool = default;

auto Polygon::toString() const -> std::string
{
    auto str = "["s;
    auto i = 0;
    for (const auto& ls : polys) {
        if (i++ > 0)
            str.push_back(',');
        str += ls.toString();
    }
    return str + "]";
}

namespace meta
{

#define COMMON_UNARY_OPS(obj)                   \
    if (op == OperatorTypeof::name())           \
        return Value::make(ident);              \
    if (op == OperatorBool::name())             \
        return Value::t();                      \
    if (op == OperatorAsString::name())         \
        return Value::make(obj.toString());

#define COMPARISON_OPS(type, obj)               \
    if (op == OperatorEq::name()) { /* == */    \
        if (r.isa(ValueType::Null))             \
            return Value::f();                  \
        if (auto o = getObject<type>(r, this))  \
            return Value::make(obj == *o);      \
    }                                           \
    if (op == OperatorNeq::name()) { /* != */   \
        if (r.isa(ValueType::Null))             \
            return Value::f();                  \
        if (auto o = getObject<type>(r, this))  \
            return Value::make(!(obj == *o));   \
    }

template <typename Self>
static auto handleIntersectionOp(const Self& self, const Value& r) -> Value {
    if (r.isa(ValueType::Null))
        return Value::f();

    if constexpr (std::is_same_v<Self, Point>) {
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(o->contains(self));
        if (auto o = getObject<Point>(r, &PointType::Type))
            return Value::make(self == *o);
        if (auto o = getObject<LineString>(r, &LineStringType::Type))
            return Value::make(o->intersects(self));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(o->contains(self));
    } else if constexpr (std::is_same_v<Self, BBox>) {
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<Point>(r, &PointType::Type))
            return Value::make(self.contains(*o));
        if (auto o = getObject<LineString>(r, &LineStringType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(o->intersects(self));
    } else if constexpr (std::is_same_v<Self, LineString>) {
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<Point>(r, &PointType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<LineString>(r, &LineStringType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(self.intersects(*o));
    } else if constexpr (std::is_same_v<Self, Polygon>) {
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<Point>(r, &PointType::Type))
            return Value::make(self.contains(*o));
        if (auto o = getObject<LineString>(r, &LineStringType::Type))
            return Value::make(self.intersects(*o));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(self.intersects(*o));
    }

    return Value::f();
}

PointType PointType::Type; /* Global Meta-Type */

PointType::PointType()
    : TypedMetaType("point")
{}

auto PointType::make(double x, double y) -> Value
{
    auto obj = TransientObject(&PointType::Type);
    auto pt = get(obj);
    pt->x = x;
    pt->y = y;

    return Value(ValueType::TransientObject, std::move(obj));
}

auto PointType::unaryOp(std::string_view op, const Point& self) const -> tl::expected<Value, Error>
{
    COMMON_UNARY_OPS(self);

    mapget::raiseFmt("Invalid operator {} for operand {}", op, ident);
}

auto PointType::binaryOp(std::string_view op, const Point& p, const Value& r) const -> tl::expected<Value, Error>
{
    COMPARISON_OPS(Point, p);

    if (op == OP_NAME_WITHIN) {
        if (r.isa(ValueType::Null))
            return Value::f();
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(o->contains(p));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(o->contains(p));
    }

    if (op == OP_NAME_INTERSECTS) {
        return handleIntersectionOp(p, r);
    }

    mapget::raiseFmt("Invalid operator {} for operands {} and {}", op, ident, valueType2String(r.type));
}

auto PointType::binaryOp(std::string_view op, const Value& l, const Point& r) const -> tl::expected<Value, Error>
{
    if (op == OperatorEq::name() || op == OperatorNeq::name() || op == OP_NAME_INTERSECTS)
        return binaryOp(op, r, l);

    mapget::raiseFmt("Invalid operator {} for operands {} and {}", op, valueType2String(l.type), ident);
}

auto PointType::unpack(const Point& self, std::function<bool(Value)> res) const -> tl::expected<void, Error>
{
    if (!res(Value(ValueType::Float, self.x))) return {};
    if (!res(Value(ValueType::Float, self.y))) return {};
    return {};
}

BBoxType BBoxType::Type;

BBoxType::BBoxType()
    : TypedMetaType("bbox")
{}

auto BBoxType::make(BBox o) -> Value
{
    auto obj = TransientObject(&BBoxType::Type);
    auto bb = get(obj);
    *bb = o;

    return Value(ValueType::TransientObject, std::move(obj));
}

auto BBoxType::make(double x1, double y1, double x2, double y2) -> Value
{
    auto obj = TransientObject(&BBoxType::Type);
    auto bb = get(obj);
    bb->p1 = {x1, y1};
    bb->p2 = {x2, y2};

    return Value(ValueType::TransientObject, std::move(obj));
}

auto BBoxType::unaryOp(std::string_view op, const BBox& self) const -> tl::expected<Value, Error>
{
    COMMON_UNARY_OPS(self);

    mapget::raiseFmt("Invalid operator {} for operand {}", op, ident);
}

auto BBoxType::binaryOp(std::string_view op, const BBox& b, const Value& r) const -> tl::expected<Value, Error>
{
    COMPARISON_OPS(BBox, b);

    if (op == OP_NAME_WITHIN) {
        if (r.isa(ValueType::Null))
            return Value::f();
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(o->contains(b));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(o->contains(b));
    }

    if (op == OP_NAME_CONTAINS) {
        if (r.isa(ValueType::Null))
            return Value::f();
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(b.contains(*o));
        if (auto o = getObject<Point>(r, &PointType::Type))
            return Value::make(b.contains(*o));
        if (auto o = getObject<LineString>(r, &LineStringType::Type))
            return Value::make(b.contains(*o));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(b.contains(*o));
    }

    if (op == OP_NAME_INTERSECTS) {
        return handleIntersectionOp(b, r);
    }

    mapget::raiseFmt("Invalid operator {} for operands {} and {}", op, ident, valueType2String(r.type));
}

auto BBoxType::binaryOp(std::string_view op, const Value& l, const BBox& r) const -> tl::expected<Value, Error>
{
    if (op == OperatorEq::name() || op == OperatorNeq::name() || op == OP_NAME_INTERSECTS)
        return binaryOp(op, r, l);

    mapget::raiseFmt("Invalid operator {} for operands {} and {}", op, valueType2String(l.type), ident);
}

auto BBoxType::unpack(const BBox& self, std::function<bool(Value)> res) const -> tl::expected<void, Error>
{
    if (!res(PointType::Type.make(self.p1.x, self.p1.y))) return {};
    if (!res(PointType::Type.make(self.p2.x, self.p2.y))) return {};
    return {};
}


LineStringType LineStringType::Type;

LineStringType::LineStringType()
    : TypedMetaType("linestring")
{}

auto LineStringType::make(std::vector<Point> pts) -> Value
{
    auto obj = TransientObject(&LineStringType::Type);
    auto ls = get(obj);
    ls->points = std::move(pts);

    return Value(ValueType::TransientObject, std::move(obj));
}

auto LineStringType::unaryOp(std::string_view op, const LineString& self) const -> tl::expected<Value, Error>
{
    COMMON_UNARY_OPS(self);

    if (op == OperatorLen::name())
        return Value::make((int64_t)self.points.size());

    mapget::raiseFmt("Invalid operator {} for operand {}", op, ident);
}

auto LineStringType::binaryOp(std::string_view op, const LineString& ls, const Value& r) const -> tl::expected<Value, Error>
{
    COMPARISON_OPS(LineString, ls);

    if (op == OP_NAME_WITHIN) {
        if (r.isa(ValueType::Null))
            return Value::f();
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(o->contains(ls));
        if (auto o = getObject<Polygon>(r, &PolygonType::Type))
            return Value::make(o->contains(ls));
    }

    if (op == OP_NAME_INTERSECTS) {
        return handleIntersectionOp(ls, r);
    }

    mapget::raiseFmt("Invalid operator {} for operands {} and {}", op, ident, valueType2String(r.type));
}

auto LineStringType::binaryOp(std::string_view op, const Value& l, const LineString& r) const -> tl::expected<Value, Error>
{
    if (op == OperatorEq::name() || op == OperatorNeq::name() || op == OP_NAME_INTERSECTS)
        return binaryOp(op, r, l);

    mapget::raiseFmt("Invalid operator {} for operands {} and {}", op, valueType2String(l.type), ident);
}

auto LineStringType::unpack(const LineString& ls, std::function<bool(Value)> res) const -> tl::expected<void, Error>
{
    for (const auto& p : ls.points) {
        if (!res(PointType::Type.make(p.x, p.y))) return {};
    }
    return {};
}

PolygonType PolygonType::Type;

PolygonType::PolygonType()
    : TypedMetaType("polygon")
{}

auto PolygonType::make(LineString outer) -> Value
{
    auto obj = TransientObject(&PolygonType::Type);
    auto pl = get(obj);
    pl->polys = {std::move(outer)};

    return Value(ValueType::TransientObject, std::move(obj));
}

auto PolygonType::make(std::vector<LineString> full) -> Value
{
    auto obj = TransientObject(&PolygonType::Type);
    auto pl = get(obj);
    pl->polys = full;

    return Value(ValueType::TransientObject, std::move(obj));
}

auto PolygonType::unaryOp(std::string_view op, const Polygon& self) const -> tl::expected<Value, Error>
{
    COMMON_UNARY_OPS(self);

    if (op == OperatorLen::name())
        return Value::make(self.polys.size() > 0 ? (int64_t)self.polys[0].points.size() : 0);

    mapget::raiseFmt<>("Invalid operator {} for operand {}", op, ident);
}

auto PolygonType::binaryOp(std::string_view op, const Polygon& l, const Value& r) const -> tl::expected<Value, Error>
{
    COMPARISON_OPS(Polygon, l);

    if (op == OP_NAME_WITHIN) {
        if (r.isa(ValueType::Null))
            return Value::f();
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(o->contains(l));
        // TODO: Polygon within polygon is not implemented
    }

    if (op == OP_NAME_CONTAINS) {
        if (r.isa(ValueType::Null))
            return Value::f();
        if (auto o = getObject<BBox>(r, &BBoxType::Type))
            return Value::make(l.contains(*o));
        if (auto o = getObject<Point>(r, &PointType::Type))
            return Value::make(l.contains(*o));
        if (auto o = getObject<LineString>(r, &LineStringType::Type))
            return Value::make(l.contains(*o));
        // TODO: Polygon contains polygon is not implemented
    }

    if (op == OP_NAME_INTERSECTS) {
        return handleIntersectionOp(l, r);
    }

    mapget::raiseFmt<>("Invalid operator {} for operands {} and {}", op, ident, valueType2String(r.type));
}

auto PolygonType::binaryOp(std::string_view op, const Value& l, const Polygon& r) const -> tl::expected<Value, Error>
{
    if (op == OperatorEq::name() || op == OperatorNeq::name() || op == OP_NAME_INTERSECTS)
        return binaryOp(op, r, l);

    mapget::raiseFmt<>("Invalid operator {} for operands {} and {}", op, valueType2String(l.type), ident);
}

auto PolygonType::unpack(const Polygon& self, std::function<bool(Value)> res) const -> tl::expected<void, Error>
{
    if (self.polys.empty()) {
        res(Value::null());
        return {};
    }

    /* Unpack the outer polygon only */
    return LineStringType::Type.unpack(self.polys[0], std::move(res));
}
}


GeoFn GeoFn::Fn;
auto GeoFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "geo",
        "Returns one or more GeoJSON geometry types built from the input node.\n"
        "The function searches for the field 'geometry' and/or 'type' to find its entry node.",
        "geo(root=_) -> <null|point|linestring|polygon>"
    };
    return info;
}

auto GeoFn::eval(Context ctx, const Value& val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.size() > 1)
        return tl::unexpected(Error(Error::Type::InvalidArguments, "geo() expects 0 or 1 arguments"));

    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    std::optional<Error> capturedError;
    
    auto getType = [&](Value v) -> std::optional<std::string> {
        if (v.node()) {
            if (auto typenode = v.node()->get(StringPool::TypeStr))
                if (Value value = typenode->value();
                    value.isa(ValueType::String))
                    return value.as<ValueType::String>();
        }
        return {};
    };

    auto forEachGeometry = [&](Value v, auto&& callback) {
        if (!v.node())
            return Result::Stop;

        if (auto geonode = v.node()->get(StringPool::GeometryStr)) {
            v = Value::field(geonode);
            if (!v.node())
                return Result::Stop;
        }

        auto type = getType(v);
        if (!type)
            return Result::Stop;

        if (type == "GeometryCollection"s) {
            auto node = v.node()->get(StringPool::GeometriesStr);
            if (!node)
                return Result::Stop;

            return node->iterate(ModelNode::IterLambda([&](auto &&v) {
                return callback(Value::field(v)) == Result::Continue;
            })) ? Result::Continue : Result::Stop;
        } else {
            return callback(v);
        }
    };

    auto evalValue = [&](Value v) {
        if (!v.node())
            return Result::Stop;

        if (auto geonode = v.node()->get(StringPool::GeometryStr)) {
            v = Value::field(geonode);
            if (!v.node())
                return Result::Stop;
        }

        auto type = ""s;
        if (auto typestr = getType(v)) {
            type = std::move(*typestr);
        } else {
            if (auto resVal = res(ctx, std::move(v)); !resVal) {
                capturedError = resVal.error();
                return Result::Stop;
            } else {
                return resVal.value();
            }
        }

        if (auto coordnode = v.node()->get(StringPool::CoordinatesStr)) {
            if (type == "Point") {
                Point pt;
                if (!parseCoordinatePoint(*coordnode, pt)) {
                    return handleNullResult(ctx, res, capturedError);
                }

                if (auto resVal = res(ctx, meta::PointType::Type.make(pt.x, pt.y)); !resVal) {
                    capturedError = resVal.error();
                    return Result::Stop;
                } else {
                    return resVal.value();
                }
            } else if (type == "MultiPoint") {
                for (auto i = 0; i < coordnode->size(); ++i) {
                    Point pt;
                    if (!parseCoordinatePoint(*coordnode->at(i), pt)) {
                        if (auto result = handleNullResult(ctx, res, capturedError); result == Result::Stop)
                            return Result::Stop;
                    }

                    if (auto resVal = res(ctx, meta::PointType::Type.make(pt.x, pt.y)); !resVal) {
                        capturedError = resVal.error();
                        return Result::Stop;
                    } else {
                        return resVal.value();
                    }
                }

                return Result::Continue;
            } else if (type == "LineString") {
                std::vector<Point> pts;
                for (auto i = 0; i < coordnode->size(); ++i) {
                    if (!parseCoordinatePoint(*coordnode->at(i), pts.emplace_back())) {
                        if (auto result = handleNullResult(ctx, res, capturedError); result == Result::Stop)
                            return Result::Stop;
                    }
                }

                if (auto resVal = res(ctx, meta::LineStringType::Type.make(std::move(pts))); !resVal) {
                    capturedError = resVal.error();
                    return Result::Stop;
                } else {
                    return resVal.value();
                }
            } else if (type == "MultiLineString") {
                for (auto i = 0; i < coordnode->size(); ++i) {
                    std::vector<Point> pts;
                    auto subline = coordnode->at(i);
                    for (auto i = 0; i < subline->size(); ++i) {
                        if (!parseCoordinatePoint(*subline->at(i), pts.emplace_back()))
                            if (auto resVal = res(ctx, Value::null()); !resVal || resVal.value() == Result::Stop)
                                return Result::Stop;
                    }

                    if (auto resVal = res(ctx, meta::LineStringType::Type.make(std::move(pts))); !resVal) {
                        capturedError = resVal.error();
                        return Result::Stop;
                    } else if (resVal.value() == Result::Stop) {
                        return Result::Stop;
                    }
                }

                return Result::Continue;
            } else if (type == "Polygon") {
                std::vector<LineString> polys;
                for (auto i = 0; i < coordnode->size(); ++i) {
                    std::vector<Point> pts;
                    auto subline = coordnode->at(i);
                    for (auto i = 0; i < subline->size(); ++i) {
                        if (!parseCoordinatePoint(*subline->at(i), pts.emplace_back()))
                            if (auto resVal = res(ctx, Value::null()); !resVal || resVal.value() == Result::Stop)
                                return Result::Stop;
                    }

                    polys.push_back({std::move(pts)});
                }

                if (auto resVal = res(ctx, meta::PolygonType::Type.make(std::move(polys))); !resVal) {
                    capturedError = resVal.error();
                    return Result::Stop;
                } else {
                    return resVal.value();
                }
            } else if (type == "MultiPolygon") {
                for (auto i = 0; i < coordnode->size(); ++i) {
                    std::vector<LineString> polys;
                    auto subpoly = coordnode->at(i);
                    for (auto i = 0; i < subpoly->size(); ++i) {
                        std::vector<Point> pts;
                        auto subline = subpoly->at(i);
                        for (auto i = 0; i < subline->size(); ++i) {
                            if (!parseCoordinatePoint(*subline->at(i), pts.emplace_back()))
                                if (auto resVal = res(ctx, Value::null()); !resVal || resVal.value() == Result::Stop)
                                    return Result::Stop;
                        }

                        polys.push_back({std::move(pts)});
                    }

                    if (auto resVal = res(ctx, meta::PolygonType::Type.make(std::move(polys))); !resVal) {
                        capturedError = resVal.error();
                        return Result::Stop;
                    } else if (resVal.value() == Result::Stop) {
                        return Result::Stop;
                    }
                }

                return Result::Continue;
            } else {
                if (auto resVal = res(ctx, Value::null()); !resVal) {
                    capturedError = resVal.error();
                    return Result::Stop;
                } else {
                    return resVal.value();
                }
            }
        } else {
            if (auto resVal = res(ctx, Value::null()); !resVal) {
                capturedError = resVal.error();
                return Result::Stop;
            } else {
                return resVal.value();
            }
        }

        return Result::Continue;
    };

    if (!args.empty()) {
        auto result = args[0]->eval(ctx, std::move(val),
                             LambdaResultFn([&](auto ctx, auto v) {
                                 return forEachGeometry(std::move(v), evalValue);
                             }));
        if (capturedError) {
            return tl::unexpected(*capturedError);
        }
        return result;
    }
    auto result = forEachGeometry(std::move(val), evalValue);
    if (capturedError) {
        return tl::unexpected(*capturedError);
    }
    return result;
};

PointFn PointFn::Fn;
auto PointFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "point",
        "Returns a GeoJSON point object.",
        "point(x, y) -> <point>"
    };
    return info;
}

auto PointFn::eval(Context ctx, const Value& val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.size() != 2)
        return tl::unexpected(Error(Error::Type::InvalidArguments, fmt::format("point() expects 2 arguments, got {}", args.size())));

    auto [xok, xval] = util::evalArg1Any(ctx, val, args[0]);
    if (!xok)
        return tl::unexpected(Error(Error::Type::ExpectedSingleValue, "point() expects single value for argument 0"));

    auto [yok, yval] = util::evalArg1Any(ctx, val, args[1]);
    if (!yok)
        return tl::unexpected(Error(Error::Type::ExpectedSingleValue, "point() expects single value for argument 1"));

    auto [xtypeok, x] = getNumeric<double>(xval);
    if (!xtypeok)
        return tl::unexpected(Error(Error::Type::TypeMissmatch, fmt::format("point() argument 0 expects numeric type, got {}", valueType2String(xval.type))));

    auto [ytypeok, y] = getNumeric<double>(yval);
    if (!ytypeok)
        return tl::unexpected(Error(Error::Type::TypeMissmatch, fmt::format("point() argument 1 expects numeric type, got {}", valueType2String(yval.type))));

    return res(ctx, meta::PointType::Type.make(x, y));
}

auto BBoxFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "bbox",
        "Returns a BBox (bounding box) object.",
        "bbox(p1, p2) -> <bbox>\n"
        "bbox(x1, y1, x2, y2) -> <bbox>\n"
    };
    return info;
}

BBoxFn BBoxFn::Fn;
auto BBoxFn::eval(Context ctx, const Value& v, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    /* Get the bounding box of other geometry types */
    if (args.size() == 1) {
        auto [ok, val] = util::evalArg1Any(ctx, v, args[0]);
        if (!ok)
            return tl::unexpected(Error(Error::Type::ExpectedSingleValue, "function expects single value for argument 0"));

        if (auto o = getObject<BBox>(val, &meta::BBoxType::Type))
            return res(ctx, meta::BBoxType::Type.make(*o));
        if (auto o = getObject<Point>(val, &meta::PointType::Type))
            return res(ctx, meta::BBoxType::Type.make(o->x, o->y, o->x, o->y));
        if (auto o = getObject<LineString>(val, &meta::LineStringType::Type))
            return res(ctx, meta::BBoxType::Type.make(o->bbox()));
        if (auto o = getObject<Polygon>(val, &meta::PolygonType::Type))
            return res(ctx, meta::BBoxType::Type.make(o->bbox()));
    }

    /* Construct with 2 points */
    if (args.size() == 2) {
        auto [p1ok, p1val] = util::evalArg1Any(ctx, v, args[0]);
        if (!p1ok)
            return tl::unexpected(Error(Error::Type::ExpectedSingleValue, "function expects single value for argument 0"));

        auto [p2ok, p2val] = util::evalArg1Any(ctx, v, args[1]);
        if (!p2ok)
            return tl::unexpected(Error(Error::Type::ExpectedSingleValue, "function expects single value for argument 1"));

        Point p1, p2;
        if (auto a1 = getObject<Point>(p1val, &meta::PointType::Type)) {
            p1 = *a1;
        } else {
            return tl::unexpected(Error(Error::Type::TypeMissmatch, fmt::format("function argument 0 expects point type, got {}", valueType2String(p1val.type))));
        }

        if (auto a2 = getObject<Point>(p2val, &meta::PointType::Type)) {
            p2 = *a2;
        } else {
            return tl::unexpected(Error(Error::Type::TypeMissmatch, fmt::format("function argument 1 expects point type, got {}", valueType2String(p2val.type))));
        }

        return res(ctx, meta::BBoxType::Type.make(p1.x, p1.y, p2.x, p2.y));
    }

    /* Construct with 4 values */
    if (args.size() != 4)
        return tl::unexpected(Error(Error::Type::InvalidArguments, fmt::format("bbox() expects 1, 2, or 4 arguments, got {}", args.size())));

    std::array<double, 4> fargs;
    for (auto i = 0; i < 4; ++i) {
        auto [ok, val] = util::evalArg1Any(ctx, v, args[i]);
        if (!ok)
            return tl::unexpected(Error(Error::Type::ExpectedSingleValue, fmt::format("function expects single value for argument {}", i)));

        auto [typeok, v] = getNumeric<double>(val);
        if (!typeok)
            return tl::unexpected(Error(Error::Type::TypeMissmatch, fmt::format("function argument {} expects numeric type, got {}", i, valueType2String(val.type))));

        fargs[i] = v;
    }

    return res(ctx, meta::BBoxType::Type.make(fargs[0], fargs[1], fargs[2], fargs[3]));
}

LineStringFn LineStringFn::Fn;
auto LineStringFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "linestring",
        "Returns a GeoJSON linestring object.",
        "linestring(point...) -> <linestring>\n"
        "linestring(<x, y>...) -> <linestring>\n"
    };
    return info;
}

auto LineStringFn::eval(Context ctx, const Value& val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (args.size() < 1)
        return tl::unexpected(Error(Error::Type::InvalidArguments, fmt::format("function expects at least 1 argument, got {}", args.size())));

    enum class Mode {
        None,
        Points,
        Floats,
    };

    Mode mode = Mode::None;

    std::vector<double> floats;
    floats.reserve(4);

    for (const auto& arg : args) {
        (void)arg->eval(ctx, val, LambdaResultFn([&mode, &floats](auto, Value v) {
            /* Argument 0 determines the argument type: Point or Float, Float */
            if (mode == Mode::None)
                mode = v.isa(ValueType::TransientObject) ? Mode::Points : Mode::Floats;

            if (auto pt = getObject<Point>(v, &meta::PointType::Type)) {
                if (mode != Mode::Points)
                    mapget::raiseFmt("linestring: Expected value of type point; got {}", v.toString());

                floats.push_back(pt->x);
                floats.push_back(pt->y);
            } else {
                auto [ok, f] = getNumeric<double>(v);
                if (!ok)
                    mapget::raiseFmt("linestring: Expected numeric value; got {}", v.toString());

                floats.push_back(f);
            }

            return Result::Continue;
        }));
    }

    if (mode == Mode::Floats)
        if (floats.size() % 2 != 0)
            mapget::raiseFmt("linestring: Uneven number of values");

    std::vector<Point> points;
    for (auto i = 1; i < floats.size(); i += 2) {
        points.emplace_back(floats[i-1], floats[i-0]);
    }

    return res(ctx, meta::LineStringType::Type.make(std::move(points)));
}

/*
PolygonFn PolygonFn::Fn;
auto PolyognFn::ident() const -> const FnInfo&
{
    static const FnInfo info{
        "polygon",
        "Returns a GeoJSON polygon object.",
        "polygon(point...) -> <polygon>\n"
        "polygon(<x, y>...) -> <polygon>\n"
    };
    return info;
}

auto PolygonFn::eval(Context ctx, const Value& val, const std::vector<ExprPtr>& args, const ResultFn& res) const -> tl::expected<esult, Error>
{
    TODO: Implement.
}
*/

} // namespace mapget
