#include "mapschema-impl.h"

#include <cmath>
#include <tuple>

namespace mapget
{
namespace
{
using Schema = simfil::SchemaType;
using Role = Schema::FieldRole;
using Value = simfil::ValueType;
using Position = PointSeries::Position;
using Nodes = std::vector<TypeId>;
using Path = PointSeries::Path;
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string const& why)
{
    throw std::invalid_argument("PointSeries: " + why);
}

template <class E>
constexpr auto enumNames()
{
    if constexpr (std::is_same_v<E, Position::Kind>)
        return std::to_array<std::string_view>(
            {"dense",
             "vertexIndex",
             "fraction",
             "distance",
             "logicalIndex",
             "interval",
             "vertexMeasure"});
    else if constexpr (std::is_same_v<E, GeomType>)
        return std::to_array<std::string_view>(
            {"points", "line", "polygon", "mesh", "aabb", "gltfNodeIndex"});
    else if constexpr (std::is_same_v<E, PointSeries::Sampling>)
        return std::to_array<std::string_view>({"exact", "linear"});
    else if constexpr (std::is_same_v<E, PointSeries::Markers>)
        return std::to_array<std::string_view>({"none", "appendToValidity", "pointValidities"});
    else {
        static_assert(std::is_same_v<E, PointSeries::Measure>);
        return std::to_array<std::string_view>({"wgs84Planar2D", "geographicMetres"});
    }
}

/** Native equality for idempotent registration and composition. Declarations with the
 * same selector may contribute additional fields; no transport tree or hash is needed. */
template <class T>
bool equalMembers(T const& a, T const& b, bool selectorOnly = false)
{
    if constexpr (
        std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string> ||
        requires { a.has_value(); })
        return a == b;
    else if constexpr (requires { a.capacity(); })
        return std::ranges::equal(
            a,
            b,
            [](auto const& x, auto const& y) { return equalMembers(x, y); });
    else {
        bool equal = true;
        point_schema::memberPointers<T>(
            [&](std::string_view key, auto member)
            {
                if (equal && !(selectorOnly && key == "fields"))
                    equal = equalMembers(a.*member, b.*member);
            });
        return equal;
    }
}

/** The native and binary APIs do not pass through JSON. This is the layer JSON interchange
 * boundary. */
template <class T>
Json exportMember(T const& value)
{
    if constexpr (std::is_enum_v<T>)
        return enumNames<T>().at(size_t(value));
    else if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>)
        return value;
    else if constexpr (requires { value.has_value(); })
        return value ? exportMember(*value) : Json(nullptr);
    else if constexpr (requires { value.capacity(); }) {
        auto result = Json::array();
        for (auto const& item : value)
            result.push_back(exportMember(item));
        return result;
    }
    else {
        auto result = Json::object();
        point_schema::members(
            value,
            [&](auto key, auto const& member) { result[key] = exportMember(member); });
        return result;
    }
}

template <class T>
void importMember(Json const& json, T& value)
{
    if constexpr (std::is_enum_v<T>) {
        auto names = enumNames<T>();
        auto it = std::ranges::find(names, json.get_ref<std::string const&>());
        if (it == names.end())
            invalid("unknown tag: " + json.dump());
        value = T(it - names.begin());
    }
    else if constexpr (std::is_same_v<T, std::string>) {
        auto const& text = json.get_ref<std::string const&>();
        if (text.size() > point_schema::MaxText)
            invalid("text is too long");
        value = text;
    }
    else if constexpr (std::is_same_v<T, int64_t>) {
        if (!json.is_number_integer() ||
            (json.is_number_unsigned() && json.get<uint64_t>() > INT64_MAX))
            invalid("a raw codec key must be a signed 64-bit integer");
        value = json.get<int64_t>();
    }
    else if constexpr (std::is_arithmetic_v<T>)
        value = json.get<T>();
    else if constexpr (requires { value.has_value(); }) {
        if (!json.is_null())
            importMember(json, value.emplace());
    }
    else if constexpr (requires { value.capacity(); }) {
        if (!json.is_array() || json.size() > point_schema::maxCount<typename T::value_type>)
            invalid("invalid declaration array");
        value.resize(json.size());
        for (size_t i = 0; i < value.size(); ++i)
            importMember(json[i], value[i]);
    }
    else {
        if (!json.is_object())
            invalid("expected a declaration object");
        size_t known = 0;
        point_schema::members(
            value,
            [&](auto key, auto& member)
            {
                if (auto it = json.find(key); it != json.end()) {
                    importMember(*it, member);
                    ++known;
                }
            });
        if (known != json.size())
            invalid("unrecognized declaration member");
    }
}

/** Enforce wire limits before registration, including unused operands and codec entries. */
template <class T>
void checkMember(T const& value)
{
    if constexpr (std::is_enum_v<T>) {
        if (size_t(value) >= enumNames<T>().size())
            invalid("invalid enum tag");
    }
    else if constexpr (std::is_same_v<T, double>) {
        if (!std::isfinite(value))
            invalid("non-finite numeric operand");
    }
    else if constexpr (std::is_same_v<T, std::string>) {
        if (value.size() > point_schema::MaxText || value.find('\0') != std::string::npos)
            invalid("invalid or oversized text");
    }
    else if constexpr (requires { value.has_value(); }) {
        if (value)
            checkMember(*value);
    }
    else if constexpr (requires { value.capacity(); }) {
        if (value.size() > point_schema::maxCount<typename T::value_type>)
            invalid("declaration array is too long");
        for (auto const& item : value) {
            if constexpr (std::is_same_v<typename T::value_type, std::string>)
                if (item.empty())
                    invalid("empty field name in path");
            checkMember(item);
        }
    }
    else if constexpr (!std::is_arithmetic_v<T>)
        point_schema::members(value, [](auto, auto const& member) { checkMember(member); });
}

void checkCodec(std::string const& name, PointSeries::Codec const& codec)
{
    checkMember(name);
    checkMember(codec);
    if (name.empty() || (codec.tableOnly_ && codec.codes_.empty()))
        invalid("a named table codec requires entries");
    std::optional<int64_t> last;
    for (auto const& code : codec.codes_) {
        if (last && *last >= code.raw_)
            invalid("duplicate or unordered raw code in " + name);
        last = code.raw_;
        if ((code.value_ && (!code.status_.empty() || code.lower_ || code.upper_)) ||
            (!code.value_ && code.status_.empty()) ||
            (code.lower_ && code.upper_ && *code.lower_ > *code.upper_))
            invalid("invalid physical value, status or bounds in " + name);
    }
}

auto ownerKey(PointSeries const& series)
{
    return std::tie(series.featureType_, series.attributeLayer_, series.attribute_);
}

auto seriesKey(PointSeries const& series)
{
    return std::tuple_cat(ownerKey(series), std::tie(series.fields_.front().name_));
}

void checkSeries(PointSeries const& s)
{
    checkMember(s);
    if (s.featureType_.empty() || s.attributeLayer_.empty() || s.attribute_.empty() ||
        (s.geometryName_ && s.geometryName_->empty()) || s.fields_.empty())
        invalid("missing owner, geometry name or output field");
    if (s.branches_.empty() != s.branchTarget_.empty() ||
        s.branches_.empty() != s.branchRecords_.empty() ||
        s.branches_.empty() != s.hostTarget_.empty())
        invalid("referenced profiles require branches, branchTarget, branchRecords and hostTarget");
    auto const& p = s.position_;
    using K = Position::Kind;
    bool interval = p.kind_ == K::Interval;
    auto axis = interval ? p.intervalAxis_ : p.kind_;
    if (interval &&
        (axis == K::Interval || axis == K::Dense || p.end_.empty() ||
         s.sampling_ != PointSeries::Sampling::Exact))
        invalid("invalid interval axis, bounds or sampling");
    if (!interval &&
        (!p.end_.empty() || p.intervalAxis_ != K::VertexIndex || !p.includeEnd_ ||
         !p.includeStart_))
        invalid("interval bounds supplied on a non-interval series");
    if (axis == K::Fraction ?
            p.bits_.empty() == p.bitsFromAttribute_.empty() :
            (!p.bits_.empty() || !p.bitsFromAttribute_.empty()))
        invalid("a fraction needs exactly one width operand, and other axes take none");
    if ((axis == K::VertexMeasure) != !p.vertexMeasures_.empty())
        invalid("a vertex-measure axis requires its own occurrence-relative measure array");
    if (p.distanceScale_ <= 0 || (axis != K::Distance && p.distanceScale_ != 1))
        invalid("distanceScale must be a positive distance conversion");
    if (axis == K::Dense && !p.field_.empty())
        invalid("dense alignment has no position field");
    for (auto const& field : s.fields_)
        if (field.name_.empty() || field.name_.front() == '$')
            invalid("empty or reserved point output name");
}

/** Ignore the nullable bit when checking a concrete scalar or collection, never a mixed/unknown
 * kind. */
bool only(Schema type, Value affinity)
{
    auto bits = Schema::affinities(type.kind());
    auto mask = simfil::valueTypeAffinity(affinity);
    return (bits & mask) && !(bits & ~(mask | simfil::valueTypeAffinity(Value::Null)));
}

// A browser's long double can be just double. Compare 64-bit raw codes without
// rounding away adjacent integer bounds above 2^53, including mixed signedness.
using Bound = Schema::NumericValue;
template <class I>
int compareReal(I integer, double real)
{
    constexpr auto digits = std::numeric_limits<I>::digits;
    auto limit = std::ldexp(1., digits);
    if (real >= limit)
        return -1;
    if (real < (std::is_signed_v<I> ? -limit : 0.))
        return 1;
    auto truncated = static_cast<I>(real);
    if (integer != truncated)
        return integer < truncated ? -1 : 1;
    return (std::trunc(real) > real) - (std::trunc(real) < real);
}

bool less(Bound const& a, Bound const& b)
{
    return std::visit(
        [](auto x, auto y)
        {
            if constexpr (std::is_integral_v<decltype(x)> && std::is_integral_v<decltype(y)>)
                return std::cmp_less(x, y);
            else if constexpr (std::is_integral_v<decltype(x)>)
                return compareReal(x, y) < 0;
            else if constexpr (std::is_integral_v<decltype(y)>)
                return compareReal(y, x) > 0;
            else
                return x < y;
        },
        a,
        b);
}

void unique(Nodes& nodes)
{
    std::ranges::sort(nodes);
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
}

std::string childKey(std::string_view prefix, std::string_view spelling)
{
    return std::string(prefix) + "/" + schema_detail::pointerToken(spelling);
}
}  // namespace

void LayerSchema::addPointSeries(PointSeries series)
{
    auto& graph = *mutableOwner().schema_;
    checkSeries(series);
    auto& rows = graph.pointDeclarations[addr_.index()].series;
    if (rows.size() == UINT16_MAX)
        invalid("too many series in one layer");
    std::ranges::sort(series.fields_, {}, &PointSeries::Field::name_);
    rows.push_back(std::move(series));
}

void LayerSchema::addPointCodec(std::string name, PointSeries::Codec codec)
{
    auto& graph = *mutableOwner().schema_;
    std::ranges::sort(codec.codes_, {}, &PointSeries::Codec::Code::raw_);
    checkCodec(name, codec);
    auto& codecs = graph.pointDeclarations[addr_.index()].codecs;
    if (auto found = codecs.find(name); found != codecs.end()) {
        if (!equalMembers(found->second, codec))
            invalid("conflicting declaration of codec " + name);
        return;
    }
    if (codecs.size() == UINT16_MAX)
        invalid("too many codecs in one layer");
    codecs.emplace(std::move(name), std::move(codec));
}

std::span<PointSeries const> LayerSchema::pointSeries() const
{
    auto const& entries = mapSchema()->schema_->pointDeclarations;
    if (auto it = entries.find(addr_.index()); it != entries.end())
        return it->second.series;
    return {};
}

std::span<PointSeries const> LayerSchema::pointSeries(
    std::string_view feature,
    std::string_view attributeLayer,
    std::string_view name) const
{
    if (!mapSchema()->finalized())
        throw std::logic_error("Point-series owner lookup requires a finalized schema.");
    auto range = std::ranges::equal_range(
        pointSeries(),
        std::tuple{feature, attributeLayer, name},
        {},
        [](PointSeries const& row)
        {
            return std::tuple{
                std::string_view(row.featureType_),
                std::string_view(row.attributeLayer_),
                std::string_view(row.attribute_)};
        });
    return {range.begin(), range.end()};
}

PointSeries::Codec const* LayerSchema::pointCodec(std::string_view name) const
{
    auto const& entries = mapSchema()->schema_->pointDeclarations;
    if (auto it = entries.find(addr_.index()); it != entries.end())
        if (auto codec = it->second.codecs.find(name); codec != it->second.codecs.end())
            return &codec->second;
    return nullptr;
}

TypeId LayerSchema::shapePointQuerySchema(std::string_view feature) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.frozen)
        return simfil::NoTypeId;
    auto it = graph.pointRoots.find({addr_.index(), graph.dictionary.get(feature)});
    return it == graph.pointRoots.end() ? simfil::NoTypeId : it->second;
}

Json MapSchema::Impl::pointDeclarationsJson(size_t index) const
{
    auto found = pointDeclarations.find(index);
    if (found == pointDeclarations.end())
        return nullptr;
    auto result = Json{{"series", exportMember(found->second.series)}, {"codecs", Json::object()}};
    for (auto const& [name, codec] : found->second.codecs)
        result["codecs"][name] = exportMember(codec);
    return result;
}

void MapSchema::Impl::importPointDeclarations(size_t index, Json const& json)
{
    if (!json.is_object() || json.size() != 2 || !json.at("codecs").is_object() ||
        json.at("codecs").size() > UINT16_MAX)
        invalid("invalid point declaration inventory");
    auto layer = pool.layer(name(layerAt(index).name));
    for (auto const& [key, value] : json.at("codecs").items()) {
        PointSeries::Codec codec;
        importMember(value, codec);
        layer->addPointCodec(key, std::move(codec));
    }
    std::vector<PointSeries> series;
    importMember(json.at("series"), series);
    for (auto& row : series)
        layer->addPointSeries(std::move(row));
}

/** Compile (or check, on input) the exact same point shapes from physical owner edges.
 * Keys identify generated types across composition without borrowing a constituent's TypeIds.
 */
class MapSchema::Impl::PointBuilder
{
    struct Edge
    {
        Nodes types;
        Role role = Role::Data;
        bool required = false;
        bool multimap = false;
        Edge() = default;
        Edge(TypeId id, Role role = Role::Data, bool required = false)
            : types{id}, role(role), required(required)
        {
        }
    };
    using Fields = std::map<std::string, Edge, std::less<>>;
    struct Site
    {
        Nodes attributes;
        std::vector<PointSeries const*> series;
    };
    using Sites = std::map<std::string, std::map<std::string, Site, std::less<>>, std::less<>>;

    Impl& graph_;
    MapSchema& pool_;
    bool verify_;
    size_t work_ = 0;
    std::map<std::string, TypeId, std::less<>> generated_;
    std::set<TypeId> used_;
    TypeId integer_ = 0, number_ = 0, string_ = 0, indices_ = 0, path_ = 0;
    struct MarkerContext
    {
        Nodes samples;
        bool append = false, separate = false;
    };
    std::map<TypeId, MarkerContext> markers_;

    void charge(size_t n = 1)
    {
        constexpr size_t limit = 16 * 1024 * 1024;
        if (n > limit - work_)
            throw std::length_error("Point schema validation work budget exceeded.");
        work_ += n;
    }

    /** Expand choice arms, never arrays or wildcard/context edges. A null arm means absence. */
    Nodes leaves(Nodes const& roots, bool discovery = false)
    {
        Nodes result;
        std::set<TypeId> done, active;
        auto visit = [&](auto&& self, TypeId id) -> void
        {
            charge();
            if (!graph_.valid(id) || graph_.data(id).flags) {
                if (discovery)
                    return;
                invalid("unknown or derived type in a raw accessor");
            }
            if (done.contains(id))
                return;
            if (active.size() == 128 || !active.insert(id).second) {
                if (discovery)
                    return;
                invalid("cyclic or excessive alternatives in a raw accessor");
            }
            auto type = pool_.schemaType(id);
            if (!type.alternatives().empty()) {
                for (auto child : type.alternatives())
                    self(self, child);
            }
            else if (Schema::kindNameId(type.kind()) != Schema::kindNameId(Kind::Null))
                result.push_back(id);
            active.erase(id);
            done.insert(id);
        };
        for (auto id : roots)
            visit(visit, id);
        unique(result);
        return result;
    }

    /** Paths are literal object members. A named choice may select its arm while locating
     * records; once a record is selected every possible element must support its operands.
     */
    Nodes follow(Nodes ids, Path const& path, bool selectChoice = false)
    {
        std::set<Nodes> positions{std::move(ids)};
        for (auto const& key : path) {
            std::set<Nodes> next;
            for (auto const& position : positions) {
                bool matched = false;
                for (auto id : leaves(position)) {
                    charge();
                    auto type = pool_.schemaType(id);
                    auto field = type.field(key);
                    if (!field && selectChoice && only(type, Value::Object))
                        continue;
                    if (!only(type, Value::Object) || !field || field.name() != key ||
                        field.role() == Role::Context || field.multimap() || field.types().empty())
                        invalid("unresolved or non-scalar traversal at " + key);
                    auto types = field.types();
                    charge(types.size());
                    next.emplace(types.begin(), types.end());
                    matched = true;
                }
                // A named arm can discriminate a choice. Once two arms have matched,
                // a valid tail in one must not hide a broken accessor in the other.
                if (!matched)
                    invalid("no declaration for path member " + key);
            }
            positions = std::move(next);
        }
        ids.clear();
        for (auto const& position : positions) {
            auto resolved = leaves(position);
            ids.insert(ids.end(), resolved.begin(), resolved.end());
        }
        unique(ids);
        if (ids.empty())
            invalid("raw path has no concrete target");
        return ids;
    }

    Nodes elements(Nodes const& arrays)
    {
        Nodes result;
        for (auto id : leaves(arrays)) {
            auto type = pool_.schemaType(id);
            auto items = type.elements();
            if (!only(type, Value::Array) || items.empty())
                invalid("expected a typed array of records or column values");
            charge(items.size());
            result.insert(result.end(), items.begin(), items.end());
        }
        return follow(std::move(result), {});
    }

    /** Use the literal domain when it is known. A broader integer domain (e.g. uint8 for a
     * bit width) still requires a runtime check; only an impossible domain fails publication.
     */
    void numeric(
        Nodes const& ids,
        bool integral,
        std::optional<Bound> minimum = {},
        std::optional<Bound> maximum = {})
    {
        for (auto id : ids) {
            auto type = pool_.schemaType(id);
            if (!only(type, Value::Int) && (integral || !only(type, Value::Float)))
                invalid(integral ? "expected an integer operand" : "expected a numeric operand");
            if (auto d = type.numericDomain()) {
                if ((minimum && d->maximum && less(*d->maximum, *minimum)) ||
                    (maximum && d->minimum && less(*maximum, *d->minimum)))
                    invalid("numeric operand is outside its declared bounds");
            }
        }
    }

    /** The widest possible bound across arms; an unbounded arm prevents an absence proof. */
    std::optional<Bound> outerBound(Nodes const& ids, bool upper) const
    {
        std::optional<Bound> result;
        for (auto id : ids) {
            auto domain = pool_.schemaType(id).numericDomain();
            auto bound = domain ? (upper ? domain->maximum : domain->minimum) : std::nullopt;
            if (!bound)
                return {};
            if (!result || (upper ? less(*result, *bound) : less(*bound, *result)))
                result = bound;
        }
        return result;
    }

    /** A closed generated node is verified against its producer-derived fields on read.
     * Never accept a checksum-valid artifact that changes a Context edge to enumerable data.
     */
    TypeId node(
        std::string key,
        Kind kind,
        Fields fields = {},
        Nodes items = {},
        std::string_view unit = {},
        bool open = false)
    {
        charge(1 + fields.size() + items.size());
        auto [pos, inserted] = generated_.try_emplace(key, 0);
        if (!inserted && used_.contains(pos->second))
            return pos->second;
        if (verify_ && inserted)
            invalid("missing generated type " + key);
        auto id = verify_ ? pos->second : pool_.addSchema(kind);
        pos->second = id;
        used_.insert(id);
        auto type = pool_.schemaType(id);
        if (verify_) {
            auto const& row = graph_.data(id);
            if (row.owners || row.pointer || row.metaType || row.zserioType || row.attributeType ||
                row.attributeCode || row.annotations || !type.typeName().empty() ||
                type.nullable() || type.enumValueCount() || !type.directEnumSymbols().empty())
                invalid("unexpected generated type metadata: " + key);
            type.forEachFieldAlias([&](auto, auto)
                                   { invalid("unexpected generated field alias: " + key); });
            type.forEachNamedValue([&](auto const&)
                                   { invalid("unexpected generated literal: " + key); });
            if (type.kind() != kind || type.open() != open || type.baseType() ||
                !type.alternatives().empty() || type.composition() != Composition::None ||
                !std::ranges::equal(type.elements(), items))
                invalid("invalid generated type shape: " + key);
            size_t count = 0;
            type.forEachDirectField(
                [&](auto const& field)
                {
                    auto it = fields.find(field.name());
                    if (it == fields.end() ||
                        !std::ranges::equal(it->second.types, field.types()) ||
                        field.role() != it->second.role ||
                        field.required() != it->second.required ||
                        field.multimap() != it->second.multimap)
                        invalid(
                            "invalid generated field: " + key + "/" + std::string(field.name()));
                    ++count;
                });
            if (count != fields.size())
                invalid("incomplete generated fields: " + key);
            auto d = type.numericDomain();
            if (bool(d) != !unit.empty() ||
                (d &&
                 (d->unit != unit || d->scale != 1 || d->offset != 0 || !d->encoding.empty() ||
                  d->minimum || d->maximum || !d->ordinaryRanges.empty() || d->complete)))
                invalid("invalid generated physical unit");
            return id;
        }
        auto& row = graph_.data(id);
        row.flags = PointQueryFlag;
        row.key = graph_.text(key);
        if (open)
            pool_.setOpen(id, true);
        for (auto const& [name, edge] : fields) {
            for (auto child : edge.types)
                pool_.addFieldSchema(id, name, child, edge.multimap);
            pool_.setFieldRole(id, name, edge.role);
            pool_.setFieldRequired(id, name, edge.required);
        }
        for (auto child : items)
            pool_.addElementSchema(id, child);
        if (!unit.empty())
            type.asScalar().setNumericDomain({.unit = unit});
        return id;
    }

    /** Collect member names on concrete payload/choice arms, retaining all raw targets. */
    Fields fieldsOf(Nodes const& types)
    {
        Fields result;
        for (auto id : leaves(types, true)) {
            if (!only(pool_.schemaType(id), Value::Object))
                continue;
            pool_.schemaType(id).forEachField(
                [&](auto const& f)
                {
                    charge(1 + f.types().size());
                    auto& edge = result[std::string(f.name())];
                    edge.role = f.role();
                    edge.multimap |= f.multimap();
                    auto children = f.types();
                    if (children.empty())
                        edge.types.push_back(0);
                    else
                        edge.types.insert(edge.types.end(), children.begin(), children.end());
                });
        }
        for (auto& [_, edge] : result)
            unique(edge.types);
        return result;
    }

    /** Owner metadata alone is not proof of placement. Start at this host's physical layer map
     * and require a matching named Attribute use site on the reachable edge or choice arm.
     */
    Sites sites(TypeId feature, std::string_view featureName, std::string_view featureLayer)
    {
        Sites result;
        auto properties = pool_.schemaType(feature).field("properties");
        if (!properties || properties.role() != Role::Data || properties.multimap())
            return result;
        Nodes props(properties.types().begin(), properties.types().end());
        std::erase(props, simfil::NoTypeId);
        auto propFields = fieldsOf(props);
        auto layerMap = propFields.find("layer");
        if (layerMap == propFields.end() || layerMap->second.role != Role::Data)
            return result;
        auto layerTypes = layerMap->second.types;
        std::erase(layerTypes, simfil::NoTypeId);
        for (auto& [layerName, layer] : fieldsOf(layerTypes)) {
            if (layer.role != Role::Data)
                continue;
            std::erase(layer.types, simfil::NoTypeId);
            for (auto& [attrName, attr] : fieldsOf(layer.types)) {
                if (attr.role != Role::Data)
                    continue;
                for (auto id : attr.types) {
                    if (!id)
                        continue;
                    auto owns = [&](TypeId t)
                    {
                        for (auto const& site : graph_.ownerSites(graph_.data(t).owners)) {
                            charge();
                            if ((!site.featureLayer ||
                                 graph_.name(site.featureLayer) == featureLayer) &&
                                graph_.name(site.feature) == featureName &&
                                graph_.name(site.layer) == layerName &&
                                graph_.name(site.name) == attrName)
                                return true;
                        }
                        return false;
                    };
                    bool named = owns(id);
                    if (!named)
                        for (auto arm : leaves({id}, true))
                            named |= owns(arm);
                    if (named) {
                        // Keep the whole physical edge: a second target may not have a
                        // separate owner row but is still a possible payload at this site.
                        result[layerName][attrName].attributes = attr.types;
                        break;
                    }
                }
            }
        }
        return result;
    }

    struct Output
    {
        Nodes raw, alternative;
        Edge physical;
        std::string unit;
    };

    /** Bind both local and referenced records. No implicit array flattening, fallback to a
     * first union arm, or resolution by an unqualified feature name is allowed here. */
    std::map<std::string, Output, std::less<>>
    outputs(LayerSchema const& layer, PointSeries const& s, Nodes const& attributes, TypeId feature)
    {
        auto records = follow(attributes, s.records_, true);
        Nodes alternatives;
        if (!s.branches_.empty()) {
            auto branches = elements(follow(attributes, s.branches_, true));
            auto target = follow(branches, s.branchTarget_);
            auto host = follow({feature}, s.hostTarget_);
            auto kind = [&](TypeId id)
            {
                auto t = pool_.schemaType(id);
                if (only(t, Value::String))
                    return Value::String;
                if (only(t, Value::Int))
                    return Value::Int;
                invalid("referenced profile target is not an identity scalar");
            };
            auto expected = kind(host.front());
            for (auto id : host)
                if (kind(id) != expected)
                    invalid("ambiguous host target type");
            for (auto id : target)
                if (kind(id) != expected)
                    invalid("incompatible referenced profile target type");
            auto local = follow(branches, s.branchRecords_);
            records.insert(records.end(), local.begin(), local.end());
            unique(records);
            alternatives = std::move(target);
        }
        if (!s.parallel_)
            records = elements(records);
        else
            for (auto id : records)
                if (!only(pool_.schemaType(id), Value::Object))
                    invalid("parallel records require an object of columns");
        auto operand = [&](Path const& path)
        {
            auto result = follow(records, path);
            return s.parallel_ ? elements(result) : result;
        };
        auto const& p = s.position_;
        auto axis = p.kind_ == Position::Kind::Interval ? p.intervalAxis_ : p.kind_;
        if (axis != Position::Kind::Dense) {
            auto start = operand(p.field_);
            std::optional<Bound> upper;
            if (axis == Position::Kind::Fraction) {
                auto bits = p.bits_.empty() ?
                    follow(attributes, p.bitsFromAttribute_, true) :
                    operand(p.bits_);
                numeric(bits, true, int64_t{1}, int64_t{63});
                unsigned maxBits = 63;
                if (auto width = outerBound(bits, true); width && less(*width, int64_t{63}))
                    maxBits = std::visit([](auto n) { return static_cast<unsigned>(n); }, *width);
                upper = (uint64_t{1} << maxBits) - 1;
            }
            std::optional<std::string_view> axisUnit;
            auto station = [&](Nodes const& ids)
            {
                if (axis != Position::Kind::VertexMeasure) {
                    numeric(ids, axis != Position::Kind::Distance, int64_t{0}, upper);
                    return;
                }
                numeric(ids, false);  // An M coordinate can have a negative origin.
                for (auto id : ids)
                    if (auto d = pool_.schemaType(id).numericDomain(); d && !d->unit.empty()) {
                        if (axisUnit && *axisUnit != d->unit)
                            invalid("vertex measures and record stations use different units");
                        axisUnit = d->unit;
                    }
            };
            if (axis == Position::Kind::VertexMeasure)
                station(elements(follow(attributes, p.vertexMeasures_, true)));
            station(start);
            if (p.kind_ == Position::Kind::Interval) {
                auto end = operand(p.end_);
                station(end);
                auto first = outerBound(start, false), last = outerBound(end, true);
                if (first && last &&
                    (less(*last, *first) ||
                     ((!p.includeStart_ || !p.includeEnd_) && !less(*first, *last))))
                    invalid("interval endpoints cannot form a nonempty interval");
            }
        }
        if (!s.alternative_.empty()) {
            alternatives = operand(s.alternative_);
            for (auto id : alternatives) {
                auto type = pool_.schemaType(id);
                if (!only(type, Value::String) && !only(type, Value::Int))
                    invalid("alternative must be an integer or string key");
            }
        }
        std::map<std::string, Output, std::less<>> result;
        for (auto const& field : s.fields_) {
            auto& out = result[field.name_];
            out.raw = operand(field.path_);
            out.alternative = alternatives;
            auto* codec = layer.pointCodec(field.codec_);
            if (!field.codec_.empty() && !codec)
                invalid("unknown portable codec " + field.codec_);
            bool linear = s.sampling_ == PointSeries::Sampling::Linear;
            if (linear && codec && codec->tableOnly_ &&
                std::ranges::none_of(
                    codec->codes_,
                    [](auto const& code) { return code.value_.has_value(); }))
                invalid("linear interpolation requires ordinary numeric codec values");
            bool onlyIntegers = true, transformed = codec || linear, differentUnit = false;
            std::optional<std::string> unit;
            for (auto id : out.raw) {
                auto type = pool_.schemaType(id);
                bool number = only(type, Value::Int) || only(type, Value::Float);
                onlyIntegers &= only(type, Value::Int);
                if (!number && !only(type, Value::Bool) && !only(type, Value::String) &&
                    !only(type, Value::Bytes) && !type.isBitmask())
                    invalid("point output is not a declared scalar: " + field.name_);
                if (codec || linear || !field.unit_.empty())
                    numeric({id}, codec && !codec->codes_.empty());
                auto domain = type.numericDomain();
                auto u = !field.unit_.empty() ? std::string_view(field.unit_) :
                    domain                    ? domain->unit :
                                                std::string_view{};
                if (unit && *unit != u)
                    invalid("conflicting physical units for " + field.name_);
                unit = std::string(u);
                differentUnit |= !field.unit_.empty() && (!domain || domain->unit != field.unit_);
                if (domain)
                    transformed |= domain->scale != 1 || domain->offset != 0;
                // A shared table must be viable for every selected encoded scalar type.
                if (codec && domain && !codec->codes_.empty()) {
                    numeric({id}, true, {}, codec->codes_.front().raw_);
                    numeric({id}, true, codec->codes_.back().raw_);
                }
            }
            out.unit = unit.value_or("");
            if (!transformed && !differentUnit)
                out.physical.types = out.raw;  // Preserve raw scalar metadata on a true identity.
            else {
                auto kind = !transformed && onlyIntegers ? Kind::Int : Kind::Float;
                out.physical.types = {
                    out.unit.empty() ?
                        number_ :
                        node(
                            childKey(kind == Kind::Int ? "integral-unit" : "physical", out.unit),
                            kind,
                            {},
                            {},
                            out.unit)};
            }
        }
        return result;
    }

    TypeId
    occurrence(std::string const& key, LayerSchema const& layer, Site const& site, TypeId feature)
    {
        Fields fields = fieldsOf(site.attributes);
        fields["$attributeIndex"] = {integer_, Role::Data, true};
        fields["$layer"] = {string_, Role::Data, true};
        fields["$name"] = {string_, Role::Data, true};
        fields["$layerId"] = integer_;
        fields["$validities"] = {indices_, Role::Data, true};
        auto hide = [&](std::string_view name)
        {
            if (auto it = fields.find(name); it != fields.end())
                it->second.role = Role::Context;
        };
        Fields samples, markerFields;
        bool append = false, separate = false;
        // These are procedural members (or occurrence routing hints), not ordinary
        // stored payload members. Named lookup still delegates to the raw Attribute.
        for (auto name :
             {"validity",
              "pointValidities",
              "_sourceData",
              "_mapgetPointSeriesBranch",
              "_mapgetPointSeriesGeometry"})
            hide(name);
        if (!site.series.empty())
            hide("attributeValue");
        for (auto series : site.series) {
            auto carrier = [&](Path const& path)
            {
                if (!path.empty())
                    hide(path.front());
            };
            carrier(series->records_);
            carrier(series->branches_);
            carrier(series->position_.vertexMeasures_);
            if (series->parallel_ && series->records_.empty()) {
                for (auto path :
                     {&series->position_.field_,
                      &series->position_.end_,
                      &series->position_.bits_,
                      &series->alternative_})
                    carrier(*path);
                for (auto const& field : series->fields_)
                    carrier(field.path_);
            }
        }
        for (auto series : site.series) {
            append |= series->markers_ == PointSeries::Markers::AppendToValidity;
            separate |= series->markers_ == PointSeries::Markers::PointValidities;
            for (auto& [name, out] : outputs(layer, *series, site.attributes, feature)) {
                auto sampleKey = childKey(key + "/sample", name);
                Fields contributors{
                    {"recordIndex", {integer_, Role::Data, true}},
                    {"weight", {number_, Role::Data, true}},
                    {"path", {path_, Role::Data, true}}};
                contributors["raw"].types = std::move(out.raw);
                auto contributor =
                    node(sampleKey + "/source", Kind::Object, std::move(contributors));
                Fields candidate{
                    {"contributors",
                     {node(sampleKey + "/sources", Kind::Array, {}, {contributor}),
                      Role::Data,
                      true}},
                    {"value", out.physical},
                    {"status", string_},
                    {"lower", number_},
                    {"upper", number_},
                    {"unit", string_}};
                if (!out.alternative.empty())
                    candidate["alternative"].types = std::move(out.alternative);
                auto value = node(sampleKey, Kind::Object, std::move(candidate));
                samples[name] = node(sampleKey + "/list", Kind::Array, {}, {value});
                if (series->markers_ != PointSeries::Markers::None)
                    markerFields[name] = samples[name];
                fields[name] = std::move(
                    out.physical);  // Contextual output takes precedence over the raw carrier.
            }
        }
        if (!markerFields.empty()) {
            auto marker = node(key + "/marker-samples", Kind::Object, std::move(markerFields));
            // Some tiles annotate a union Attribute, others its selected physical arm.
            // The shared attribute-query root must include all *possible* named uses.
            auto attrs = leaves(site.attributes);
            attrs.insert(attrs.end(), site.attributes.begin(), site.attributes.end());
            unique(attrs);
            bool bound = false;
            for (auto attr : attrs)
                if (auto query = graph_.findQueryRoot(feature, attr)) {
                    auto& context = markers_[query];
                    context.samples.push_back(marker);
                    context.append |= append;
                    context.separate |= separate;
                    bound = true;
                }
            if (!bound)
                invalid("no attribute-query root for a marker use");
        }
        fields["$samples"] = {
            samples.empty() ?
                node("empty-samples", Kind::Object) :
                node(key + "/samples", Kind::Object, std::move(samples)),
            Role::Context};
        auto open = std::ranges::any_of(
            leaves(site.attributes, true),
            [&](TypeId id) { return pool_.schemaType(id).open(); });
        return node(key, Kind::Object, std::move(fields), {}, {}, open);
    }

    void feature(LayerSchema const& layer, std::string const& name, TypeId feature, Sites& owners)
    {
        auto key = childKey(childKey("point", layer.name()), name);
        Fields indexes;
        Nodes all;
        for (auto const& [layerName, attributes] : owners) {
            auto indexKey = childKey(key + "/index", layerName);
            Fields named;
            for (auto const& [attrName, site] : attributes) {
                auto use = childKey(indexKey + "/use", attrName);
                auto occ = occurrence(use, layer, site, feature);
                all.push_back(occ);
                named[attrName] = {node(use + "/list", Kind::Array, {}, {occ}), Role::Context};
            }
            indexes[layerName] = {node(indexKey, Kind::Object, std::move(named)), Role::Context};
        }
        auto attributes =
            node(key + "/attributes", Kind::Array, std::move(indexes), std::move(all));
        // A thin legacy feature need not declare the native Geometry vocabulary. Its open
        // reference is still lookup-only and never proves that an actual member is absent.
        auto geometry = pool_.findSchema("mapget:Geometry");
        if (!geometry)
            geometry = pool_.schemaType(feature).field("geometry").type().id();
        if (!geometry)
            geometry = node("open-geometry", Kind::Object, {}, {}, {}, true);
        auto root = node(
            key,
            Kind::Object,
            {{"$pointIndex", {integer_, Role::Data, true}},
             {"$geometryOrdinal", {integer_, Role::Data, true}},
             {"$feature", {feature, Role::Context, true}},
             {"$geometry", {geometry, Role::Context, true}},
             {"$attributes", {attributes, Role::Data, true}}});
        auto featureId = graph_.dictionary.get(name);
        if (!featureId)
            invalid("missing feature dictionary key");
        graph_.pointRoots.emplace(std::pair{size_t(layer.addr().index()), featureId}, root);
    }

    /** Marker candidates are lookup-only on the selected attribute query context, not
     * members of each public validity. Keep native singleton/array validity compatibility.
     */
    void markerOverlays()
    {
        auto builtin = [&](std::string_view key, MapSchema::Builtin shape)
        {
            auto id = pool_.findSchema(key);
            if (!id && !verify_)
                id = pool_.builtin(shape);
            if (!id)
                invalid("missing native validity schema for a marker use");
            return id;
        };
        for (auto& [query, ctx] : markers_) {
            auto bind = [&](std::string_view name, Nodes ids, Role role)
            {
                unique(ids);
                if (verify_) {
                    auto field = pool_.schemaType(query).field(name);
                    if (!field || field.parentType().id() != query || field.required() != false ||
                        field.multimap() || field.role() != role ||
                        !std::ranges::equal(field.types(), ids))
                        invalid("invalid marker overlay field: " + std::string(name));
                }
                else {
                    for (auto id : ids)
                        pool_.addFieldSchema(query, name, id);
                    pool_.setFieldRole(query, name, role);
                    pool_.setFieldRequired(query, name, false);
                }
            };
            bind("$samples", ctx.samples, Role::Context);
            if (ctx.append)
                bind(
                    "validity",
                    {builtin("mapget:Validities", MapSchema::Builtin::Validities)},
                    Role::Data);
            if (ctx.separate) {
                auto item = builtin("mapget:Validity", MapSchema::Builtin::Validity);
                bind(
                    "pointValidities",
                    {node("marker-validities", Kind::Array, {}, {item})},
                    Role::Data);
            }
        }
    }

    void registerName(std::string_view name)
    {
        if (!name.empty() && !graph_.dictionary.get(name)) {
            if (verify_)
                invalid("missing point dictionary key: " + std::string(name));
            graph_.intern(name);
        }
    }

    /** Validate namespaces before allocating point types; builders can declare codecs in any order.
     */
    void declarations(size_t index, point_schema::Declarations& entries)
    {
        if (LayerType(graph_.layerAt(index).type) != LayerType::Features)
            invalid("only feature layers can own point declarations");
        for (auto const& [name, codec] : entries.codecs) {
            checkCodec(name, codec);
            registerName(name);
        }
        for (auto const& row : entries.series)
            checkSeries(row);  // Includes nonempty fields, required by the canonical sort key.
        if (!verify_)
            std::ranges::sort(entries.series, {}, seriesKey);
        if (!std::ranges::is_sorted(entries.series, {}, seriesKey))
            invalid("noncanonical series order");
        std::set<std::tuple<std::string, std::string, std::string, StringId>> outputNames;
        for (auto const& row : entries.series) {
            if (!std::ranges::is_sorted(row.fields_, {}, &PointSeries::Field::name_))
                invalid("noncanonical field order");
            for (auto key : {&row.featureType_, &row.attributeLayer_, &row.attribute_})
                registerName(*key);
            if (row.geometryName_)
                registerName(*row.geometryName_);
            auto path = [&](Path const& names)
            {
                for (auto const& name : names)
                    registerName(name);
            };
            for (auto p :
                 {&row.records_,
                  &row.position_.field_,
                  &row.position_.bits_,
                  &row.position_.bitsFromAttribute_,
                  &row.position_.vertexMeasures_,
                  &row.position_.end_,
                  &row.branches_,
                  &row.branchTarget_,
                  &row.branchRecords_,
                  &row.hostTarget_,
                  &row.alternative_})
                path(*p);
            for (auto const& f : row.fields_) {
                path(f.path_);
                registerName(f.name_);
                if (!outputNames
                         .emplace(
                             row.featureType_,
                             row.attributeLayer_,
                             row.attribute_,
                             graph_.dictionary.get(f.name_))
                         .second)
                    invalid("duplicate output " + f.name_ + " on " + row.attribute_);
            }
        }
    }

public:
    PointBuilder(Impl& graph, bool verify) : graph_(graph), pool_(graph.pool), verify_(verify) {}

    void run()
    {
        if (verify_)
            for (size_t i = 1; i < graph_.types.size(); ++i)
                if (graph_.types[i].flags & PointQueryFlag) {
                    auto key = graph_.text(graph_.types[i].key);
                    if (key.empty() || !generated_.emplace(key, TypeId(i)).second)
                        invalid("missing or duplicate generated type key");
                }
        // V7 has no declaration section or point-type flag. Preserve its original content identity.
        if (verify_ && graph_.wireVersion == LegacyCodecVersion) {
            if (!generated_.empty())
                invalid("generated point types in a legacy schema");
            return;
        }
        for (auto& [index, rows] : graph_.pointDeclarations)
            declarations(index, rows);
        for (size_t i = 0; i < graph_.layers.size(); ++i) {
            if (LayerType(graph_.layers[i].type) != LayerType::Features)
                continue;
            auto layer = pool_.layer(graph_.name(graph_.layers[i].name));
            std::set<PointSeries const*> bound;
            std::map<std::string_view, std::vector<PointSeries const*>> byFeature;
            for (auto const& row : layer->pointSeries())
                byFeature[row.featureType_].push_back(&row);
            for (auto const& name : layer->featureTypes()) {
                registerName(name);
                if (!integer_) {
                    // The tile dictionary and this schema dictionary are independent. Reuse
                    // protocol IDs where a producer has not already declared the spelling.
                    static StringPool const vocabulary("point-static");
                    for (StringId id = StringPool::MessageStr + 1;
                         id <= StringPool::OverlayGeometryOrdinalStr;
                         ++id) {
                        auto spelling = vocabulary.resolve(id);
                        if (spelling && !graph_.dictionary.get(*spelling)) {
                            if (verify_)
                                invalid("missing point context vocabulary");
                            if (graph_.dictionary.resolve(id))
                                graph_.intern(*spelling);
                            else
                                graph_.dictionary.addStaticKey(id, std::string(*spelling));
                        }
                    }
                    registerName("value");
                    registerName("unit");
                    (void)node("empty-samples", Kind::Object);
                    (void)node("open-geometry", Kind::Object, {}, {}, {}, true);
                    integer_ = node("integer", Kind::Int);
                    number_ = node("number", Kind::Float);
                    string_ = node("string", Kind::String);
                    indices_ = node("indices", Kind::Array, {}, {integer_});
                    path_ = node("path", Kind::Array, {}, {string_, integer_});
                }
                auto id = layer->featureSchema(name);
                auto owners = sites(id, name, layer->name());
                for (auto row : byFeature[name]) {
                    auto group = owners.find(row->attributeLayer_);
                    if (group == owners.end() || !group->second.contains(row->attribute_))
                        invalid(
                            "no matching use site on " + name + "/" + row->attributeLayer_ + "/" +
                            row->attribute_);
                    group->second.at(row->attribute_).series.push_back(row);
                    bound.insert(row);
                }
                feature(*layer, name, id, owners);
            }
            if (bound.size() != layer->pointSeries().size())
                invalid("series owner is not produced by this layer");
        }
        markerOverlays();
        if (used_.size() != generated_.size())
            invalid("orphan generated type");
        auto version = used_.empty() && graph_.pointDeclarations.empty() ?
            LegacyCodecVersion :
            PointCodecVersion;
        if (verify_ && graph_.wireVersion != version)
            invalid("noncanonical point artifact version");
        graph_.wireVersion = version;
    }
};

void MapSchema::Impl::preparePointSchemas(bool verify)
{
    if (!pointsPrepared) {
        PointBuilder(*this, verify).run();
        pointsPrepared = true;
    }
}

void MapSchema::Impl::mergePointDeclarations(size_t index, point_schema::Declarations const& input)
{
    auto layer = pool.layer(name(layerAt(index).name));
    for (auto const& [key, codec] : input.codecs)
        layer->addPointCodec(key, codec);
    for (auto const& series : input.series) {
        auto& rows = pointDeclarations[index].series;
        // Merge additions to a shared series, but do not let the same public
        // field silently acquire another selector, sampler, unit or decoder.
        PointSeries* compatible = nullptr;
        for (auto& row : rows) {
            if (ownerKey(row) != ownerKey(series))
                continue;
            if (equalMembers(row, series, true)) {
                compatible = &row;
                continue;
            }
            for (auto const& field : series.fields_)
                if (std::ranges::find(row.fields_, field.name_, &PointSeries::Field::name_) !=
                    row.fields_.end())
                    invalid("incompatible selector for point field " + field.name_);
        }
        if (!compatible) {
            layer->addPointSeries(series);
            continue;
        }
        for (auto const& field : series.fields_) {
            auto found =
                std::ranges::find(compatible->fields_, field.name_, &PointSeries::Field::name_);
            if (found == compatible->fields_.end())
                compatible->fields_.push_back(field);
            else if (!equalMembers(*found, field))
                invalid("incompatible point field " + field.name_);
        }
        std::ranges::sort(compatible->fields_, {}, &PointSeries::Field::name_);
    }
}

void MapSchema::Impl::pointMemoryUsage(MemoryUsageBreakdown& out) const
{
    simfil::MemoryUsage memory{};
    auto visit = [&](auto&& self, auto const& value) -> void
    {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>)
            memory += stringMemoryUsage(value);
        else if constexpr (requires { value.capacity(); }) {
            memory += vectorMemoryUsage(value);
            for (auto const& item : value)
                self(self, item);
        }
        else if constexpr (requires { value.has_value(); }) {
            if (value)
                self(self, *value);
        }
        else if constexpr (!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
            point_schema::members(value, [&](auto, auto const& member) { self(self, member); });
    };
    auto accountMap = [&](auto const& entries)
    {
        auto bytes = entries.size() * sizeof(typename std::decay_t<decltype(entries)>::value_type);
        memory += simfil::MemoryUsage{bytes, bytes + entries.size() * 3 * sizeof(void*)};
    };
    accountMap(pointRoots);
    accountMap(pointDeclarations);
    for (auto const& [index, entries] : pointDeclarations) {
        visit(visit, entries.series);
        accountMap(entries.codecs);
        for (auto const& [name, codec] : entries.codecs) {
            visit(visit, name);
            visit(visit, codec);
        }
    }
    out.add("point-schemas", memory);
}
}  // namespace mapget
