#include "mapget/model/mapschema.h"
#include "mapschema-builder.h"

#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <future>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <bitsery/adapter/buffer.h>
#include <bitsery/adapter/stream.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <picosha2.h>
#include "simfil/model/schema-model.h"

namespace mapget
{
using schema_detail::Composition;
using schema_detail::hasAffinity;
using schema_detail::Kind;
using schema_detail::matchesKind;
using simfil::ModelNodeAddress;
using simfil::SchemaId;
using simfil::StringId;

/** Canonical graph columns and their borrowed query indexes. The compiler is
 * transient; neither JSON trees nor flattened owning strings survive freeze.
 */
struct MapSchema::Impl
{
    static constexpr uint8_t Domains = simfil::ModelPool::FirstCustomColumnId;
    static constexpr uint8_t Layers = Domains + 1;
    static constexpr uint8_t FieldMaps = Domains + 2;
    static constexpr uint8_t DomainLists = Domains + 3;
    static constexpr uint32_t CodecVersion = 2;
    static constexpr size_t MaxEdges = 4 * 1024 * 1024;

    /** A slice into an owner-wide canonical array, never a per-domain allocation. */
    struct Range
    {
        uint32_t begin = 0, count = 0;
        template <class S>
        void serialize(S& s)
        {
            s.value4b(begin);
            s.value4b(count);
        }
        template <class T>
        std::span<T const> in(std::vector<T> const& data) const
        {
            if (begin > data.size() || count > data.size() - begin)
                throw std::invalid_argument("MapSchema array range exceeds its column.");
            return std::span<T const>(data).subspan(begin, count);
        }
    };
    /** Map-specific metadata indexed by the generic pool's domain identity. */
    struct DomainData
    {
        MODEL_COLUMN_TYPE(40);
        uint32_t flags = 0;
        ModelNodeAddress key, pointer, metaType, zserioType, attributeType, attributeCode,
            annotations;
        Range owners;
    };
    struct OwnerData
    {
        MODEL_COLUMN_TYPE(8);
        StringId feature = 0, layer = 0, name = 0;
        uint16_t reserved = 0;
        template <class S>
        void serialize(S& s)
        {
            s.value2b(feature);
            s.value2b(layer);
            s.value2b(name);
            s.value2b(reserved);
        }
    };
    struct AliasData
    {
        MODEL_COLUMN_TYPE(8);
        ModelNodeAddress key;
        SchemaId id = 0;
        uint16_t layer = 0;  // 0 global, otherwise layer index + 1
        template <class S>
        void serialize(S& s)
        {
            s.value4b(key.value_);
            s.value2b(id);
            s.value2b(layer);
        }
    };
    struct Layer
    {
        std::string name;
        std::string importedFingerprint;
        LayerType type = LayerType::Features;
        SchemaId root = 0;
        std::vector<FeatureTypeInfo> features;
        std::vector<SchemaId> featureRoots;
        std::vector<uint8_t> produced;
        std::map<std::string, SchemaId, std::less<>> buildAliases;
        std::map<std::string_view, SchemaId, std::less<>> aliases;
        bool relationsKnown = false;
        std::vector<LayerSchema::RelationDeclaration> relations;
        std::map<std::string, std::vector<size_t>, std::less<>> relationsByTarget;
    };
    /** Bound concurrent construction and retain only weak published owners. */
    struct BuildCache
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::map<std::string, std::weak_ptr<MapSchema const>, std::less<>> ready;
        std::map<std::string, std::shared_future<ConstPtr>, std::less<>> running;
    };
    /** Initialize worker-local reentrancy state before any capture accesses it. */
    static std::vector<std::string>& activeBuildKeys()
    {
        static thread_local std::vector<std::string> keys;
        return keys;
    }
    /** Only definite-length, bounded CBOR is emitted by our annotation codec. */
    class AnnotationReader : public nlohmann::detail::json_sax_dom_parser<nlohmann::json>
    {
        using Base = nlohmann::detail::json_sax_dom_parser<nlohmann::json>;
        size_t depth_ = 0, nodes_ = 0;
        bool enter(size_t count)
        {
            if (count >= MaxEdges - nodes_ || depth_ == 256)
                return false;
            nodes_ += count + 1;
            ++depth_;
            return true;
        }

    public:
        explicit AnnotationReader(nlohmann::json& result) : Base(result, false) {}
        bool start_object(size_t size) { return enter(size) && Base::start_object(size); }
        bool start_array(size_t size) { return enter(size) && Base::start_array(size); }
        bool end_object()
        {
            --depth_;
            return Base::end_object();
        }
        bool end_array()
        {
            --depth_;
            return Base::end_array();
        }
    };

    explicit Impl(MapSchema& owner, MapFormat f) : pool(owner), format(std::move(f)) {}
    MapSchema& pool;
    MapFormat format;
    std::string identity;
    bool frozen = false;
    std::optional<MemoryUsageBreakdown> memorySummary;
    nlohmann::json statistics;
    std::vector<std::pair<std::string, MapFormat>> constituents;
    std::map<std::string, std::vector<SchemaId>, std::less<>> contributorRemaps;
    class CompositionBuilder;
    std::unique_ptr<schema_detail::SchemaBuilder> builder =
        std::make_unique<schema_detail::SchemaBuilder>();
    std::vector<Layer> layers;
    simfil::ModelColumn<DomainData> domains;
    std::vector<OwnerData> owners;
    std::vector<AliasData> aliases;
    std::map<std::string_view, SchemaId, std::less<>> globalAliases;
    std::map<std::pair<SchemaId, SchemaId>, SchemaId> queryRoots;
    std::vector<LayerSchema::Entry> entries;
    std::unordered_map<std::string, ModelNodeAddress> textIndex;  // discarded at publication
    std::array<StringId, 12> descriptorKeys{};

    StringId intern(std::string_view name)
    {
        auto id = pool.strings()->emplace(name);
        if (!id)
            throw std::length_error(id.error().message);
        return *id;
    }
    ModelNodeAddress text(std::string_view value)
    {
        if (value.empty())
            return {};
        auto [it, inserted] = textIndex.try_emplace(std::string(value));
        if (inserted) {
            auto node = pool.tryNewValue(value);
            if (!node)
                throw std::length_error(node.error().message);
            it->second = (*node)->addr();
        }
        return it->second;
    }
    std::string_view text(ModelNodeAddress address) const
    {
        if (!address)
            return {};
        auto value = pool.resolve(address)->value();
        if (auto v = std::get_if<std::string_view>(&value))
            return *v;
        // ModelPool strings must always be buffer views, never a temporary owning value.
        throw std::invalid_argument("MapSchema text reference is not a string-buffer value.");
    }
    std::string_view name(StringId id) const
    {
        auto value = pool.strings()->resolve(id);
        if (!value)
            throw std::invalid_argument("MapSchema contains an unknown dictionary ID.");
        return *value;
    }
    bool valid(SchemaId id) const
    {
        return id && (builder ? builder->valid(id) : id < domains.size());
    }
    void requireDomain(SchemaId id) const
    {
        if (!valid(id))
            throw std::invalid_argument("MapSchema references an unknown domain.");
    }
    Layer& layerAt(size_t index)
    {
        if (index >= layers.size())
            throw std::invalid_argument("Invalid LayerSchema address.");
        return layers[index];
    }
    Layer const& layerAt(size_t index) const
    {
        if (index >= layers.size())
            throw std::invalid_argument("Invalid LayerSchema address.");
        return layers[index];
    }
    template <class T>
    static Range append(std::vector<T>& target, std::span<T const> values)
    {
        if (values.size() > MaxEdges - target.size())
            throw std::length_error("MapSchema edge budget exceeded.");
        Range range{uint32_t(target.size()), uint32_t(values.size())};
        target.insert(target.end(), values.begin(), values.end());
        return range;
    }
    ModelNodeAddress annotation(nlohmann::json const& value)
    {
        if (value.is_null() || value.empty())
            return {};
        auto bytes = nlohmann::json::to_cbor(value);
        auto node = pool.tryNewValue(simfil::ByteArray{std::string(bytes.begin(), bytes.end())});
        if (!node)
            throw std::length_error(node.error().message);
        return (*node)->addr();
    }
    nlohmann::json annotation(ModelNodeAddress address) const
    {
        if (!address)
            return nlohmann::json::object();
        auto value = pool.resolve(address)->value();
        auto const* bytes = std::get_if<simfil::ByteArray>(&value);
        if (!bytes)
            throw std::invalid_argument("MapSchema annotation reference is not binary.");
        nlohmann::json result;
        AnnotationReader reader(result);
        if (!nlohmann::json::sax_parse(bytes->bytes, &reader, nlohmann::json::input_format_t::cbor))
            throw std::invalid_argument("Malformed or oversized MapSchema annotation.");
        return result;
    }
    void compile();
    void compileScalarMetadata(
        SchemaId,
        schema_detail::SchemaBuilder::LogicalSchema const&,
        nlohmann::json&);
    void index();
    void validate() const;
    void validatePool() const;
    void prepareQueryRoots();
    /** Cache immutable status diagnostics after all indexes and owners have been installed. */
    void summarize();
    template <class S>
    void readWrite(S& archive, size_t budget);
    template <class S, class T, class F>
    static void
    container(S& archive, std::vector<T>& values, size_t limit, size_t& memory, F&& item);
    template <class S>
    static void string(S& archive, std::string& value, size_t limit, size_t& memory);
    void writeBody(std::ostream& output) const;
    void readBody(std::vector<uint8_t> const& input, size_t budget);
    schema_detail::SchemaBuilder exportBuilder() const;
    SchemaId import(schema_detail::SchemaBuilder const&, size_t layerIndex, SchemaId root);
    std::vector<LayerSchema::AttributePathOwner> attributeOwners(SchemaId id) const;
    std::string_view metaType(SchemaId id) const;
    SchemaId childSchema(SchemaId root, std::string_view field, std::optional<Kind> kind) const;
    std::vector<LayerSchema::NamedSchemaPath>
    scalarPaths(SchemaId root, std::string_view attribute) const;
    std::vector<SchemaId> producedRoots(Layer const& layer) const;
};

#include "mapschema-composition.h"

MapSchema::MapSchema(MapFormat format)
    : SchemaPool(std::make_shared<StringPool>("schema")),
      schema_(std::make_unique<Impl>(*this, MapFormat::fromJson(format.toJson())))
{
}
MapSchema::~MapSchema() = default;
void MapSchema::requireMutable() const
{
    if (schema_->frozen || !schema_->builder)
        throw std::logic_error("MapSchema is immutable after finalization.");
}
bool MapSchema::finalized() const
{
    return schema_->frozen;
}
MapFormat const& MapSchema::format() const
{
    return schema_->format;
}
std::string_view MapSchema::schemaId() const
{
    if (!finalized())
        throw std::logic_error("MapSchema identity is unavailable before finalization.");
    return schema_->identity;
}
size_t MapSchema::domainCount() const
{
    return (schema_->builder ? schema_->builder->schemas_.size() : schema_->domains.size()) - 1;
}

LayerSchema::Ptr MapSchema::newLayer(std::string_view name, LayerType type)
{
    requireMutable();
    if (name.empty())
        throw std::invalid_argument("A schema layer requires a nonempty name.");
    if (auto existing = layer(name)) {
        if (existing->layerType() != type)
            throw std::invalid_argument(
                "Schema layer kind conflicts with an existing declaration.");
        return existing;
    }
    if (schema_->layers.size() == UINT16_MAX)
        throw std::length_error("MapSchema layer count exceeds 16-bit namespace.");
    auto index = schema_->layers.size();
    Impl::Layer item;
    item.name = name;
    item.type = type;
    schema_->layers.push_back(std::move(item));
    auto result = LayerSchema::Ptr::make(shared_from_this(), ModelNodeAddress{Impl::Layers, index});
    return result;
}
LayerSchema::Ptr MapSchema::layer(std::string_view name) const
{
    for (size_t i = 0; i < schema_->layers.size(); ++i)
        if (schema_->layers[i].name == name)
            return LayerSchema::Ptr::make(shared_from_this(), ModelNodeAddress{Impl::Layers, i});
    return {};
}
std::vector<std::string> MapSchema::layerNames() const
{
    std::vector<std::string> names;
    for (auto const& layer : schema_->layers)
        names.push_back(layer.name);
    return names;
}

SchemaId MapSchema::addSchema(Kind kind, std::string key, std::string metaType, std::string pointer)
{
    requireMutable();
    if (!key.empty() && schema_->builder->idsByKey_.contains(key))
        throw std::invalid_argument("Duplicate global schema key: " + key);
    return schema_->builder
        ->allocate(kind, std::move(key), std::move(pointer), std::move(metaType));
}
void MapSchema::registerSchemaKey(std::string key, SchemaId id)
{
    requireMutable();
    schema_->requireDomain(id);
    auto [it, inserted] = schema_->builder->idsByKey_.emplace(std::move(key), id);
    if (!inserted && it->second != id)
        throw std::invalid_argument("A global schema key has conflicting declarations.");
}
SchemaId MapSchema::findSchema(std::string_view key) const
{
    if (schema_->builder) {
        auto it = schema_->builder->idsByKey_.find(key);
        return it == schema_->builder->idsByKey_.end() ? 0 : it->second;
    }
    auto it = schema_->globalAliases.find(key);
    return it == schema_->globalAliases.end() ? 0 : it->second;
}
void MapSchema::addFieldSchema(SchemaId parent, std::string field, SchemaId child, bool multimap)
{
    requireMutable();
    schema_->requireDomain(parent);
    if (child)
        schema_->requireDomain(child);
    auto& builder = *schema_->builder;
    builder.addDirectField(parent, field);
    if (child)
        builder.addChild(parent, field, child);
    if (multimap)
        builder.schemas_[parent].multimapFields_.insert(std::move(field));
}
void MapSchema::addElementSchema(SchemaId parent, SchemaId child)
{
    requireMutable();
    schema_->requireDomain(parent);
    schema_->requireDomain(child);
    schema_->builder->addElementSchema(parent, child);
}
void MapSchema::addEnumSymbol(SchemaId id, std::string name)
{
    requireMutable();
    schema_->requireDomain(id);
    schema_->builder->addEnumSymbol(id, name);
}
void MapSchema::addEnumSymbols(SchemaId id, std::span<std::string const> names)
{
    for (auto const& name : names)
        addEnumSymbol(id, name);
}
void MapSchema::addEnumValue(SchemaId id, simfil::ScalarValueType value)
{
    requireMutable();
    schema_->requireDomain(id);
    if (std::holds_alternative<std::string>(value) ||
        std::holds_alternative<std::string_view>(value) ||
        std::holds_alternative<simfil::Bitmask>(value))
        throw std::invalid_argument("Use symbol/named-mask declarations for strings and bitmasks.");
    schema_->builder->schemas_[id].enumValues_.push_back(std::move(value));
}
void MapSchema::setComposition(SchemaId id, Composition op)
{
    requireMutable();
    schema_->requireDomain(id);
    auto& d = schema_->builder->schemas_[id];
    d.composition_ = op;
    if (op != Composition::None)
        d.kind_ = op == Composition::AllOf ?
            Kind::Intersection :
            op == Composition::OneOf ?
            Kind::OneOf :
            Kind::Union;
}
void MapSchema::addAlternative(SchemaId id, SchemaId other)
{
    requireMutable();
    schema_->requireDomain(id);
    schema_->requireDomain(other);
    auto& ids = schema_->builder->schemas_[id].alternatives_;
    if (std::ranges::find(ids, other) == ids.end())
        ids.push_back(other);
}
void MapSchema::setTypeName(SchemaId id, std::string name)
{
    requireMutable();
    schema_->requireDomain(id);
    schema_->builder->schemas_[id].typeName_ = std::move(name);
}
void MapSchema::setOpen(SchemaId id, bool open)
{
    requireMutable();
    schema_->requireDomain(id);
    schema_->builder->schemas_[id].open_ = open;
}
void MapSchema::setNullable(SchemaId id, std::optional<bool> value)
{
    requireMutable();
    schema_->requireDomain(id);
    schema_->builder->schemas_[id].nullable_ = value;
}
void MapSchema::setFieldRequired(SchemaId id, std::string field, std::optional<bool> required)
{
    requireMutable();
    schema_->requireDomain(id);
    auto& values = schema_->builder->schemas_[id].required_;
    if (required)
        values[std::move(field)] = *required;
    else
        values.erase(field);
}
void MapSchema::setZserioType(SchemaId id, std::string name)
{
    requireMutable();
    schema_->requireDomain(id);
    auto& d = schema_->builder->schemas_[id];
    d.zserioType_ = std::move(name);
    if (d.typeName_.empty())
        d.typeName_ = d.zserioType_;
}

void MapSchema::setFieldRole(SchemaId id, std::string field, simfil::Schema::FieldRole role)
{
    requireMutable();
    schema_->requireDomain(id);
    auto& fields = schema_->builder->schemas_[id].contextFields_;
    if (role == simfil::Schema::FieldRole::Context)
        fields.insert(std::move(field));
    else
        fields.erase(field);
}
void MapSchema::setAttributeMetadata(
    SchemaId id,
    LayerSchema::AttributePathOwner owner,
    std::string type,
    std::string zserio)
{
    requireMutable();
    schema_->requireDomain(id);
    auto& d = schema_->builder->schemas_[id];
    d.attributeTypeCode_ = owner.attributeName_;
    d.attributeType_ = std::move(type);
    if (!zserio.empty())
        d.zserioType_ = std::move(zserio);
    if (d.typeName_.empty())
        d.typeName_ = d.zserioType_.empty() ? d.attributeType_ : d.zserioType_;
    schema_->builder->registerAttributeOwner(id, std::move(owner));
}
void MapSchema::setJsonSchemaAnnotations(SchemaId id, nlohmann::json annotations)
{
    requireMutable();
    schema_->requireDomain(id);
    if (!annotations.is_object() ||
        schema_detail::SchemaBuilder::annotations(annotations) != annotations)
        throw std::invalid_argument(
            "Schema annotations cannot replace structural domain declarations.");
    schema_->builder->schemas_[id].annotations_ = std::move(annotations);
}

void MapSchema::setNumericDomain(SchemaId id, simfil::Schema::NumericDomain const& value)
{
    requireMutable();
    schema_->requireDomain(id);
    nlohmann::json json{
        {"encoding", value.encoding},
        {"unit", value.unit},
        {"scale", value.scale},
        {"offset", value.offset},
        {"complete", value.complete}};
    auto numeric = [](auto const& number)
    {
        return std::visit([](auto v) { return nlohmann::json(v); }, number);
    };
    if (value.minimum)
        json["minimum"] = numeric(*value.minimum);
    if (value.maximum)
        json["maximum"] = numeric(*value.maximum);
    for (auto const& range : value.ordinaryRanges)
        json["ordinaryRanges"]
            .push_back({{"minimum", numeric(range.min)}, {"maximum", numeric(range.max)}});
    schema_->builder->schemas_[id].annotations_["x-mapget"]["numericDomain"] = std::move(json);
}
void MapSchema::addNamedValue(SchemaId id, simfil::Schema::NamedValue const& value)
{
    requireMutable();
    schema_->requireDomain(id);
    static constexpr std::string_view roles[]{"constant", "special", "flag"};
    static constexpr std::string_view
        meanings[]{"ordinary", "unknown", "variable", "unbounded", "saturated"};
    if (unsigned(value.role) >= std::size(roles) ||
        unsigned(value.meaning) >= std::size(meanings) || value.name.empty())
        throw std::invalid_argument("Invalid named-value declaration.");
    nlohmann::json json{
        {"name", value.name},
        {"value", std::visit([](auto v) { return nlohmann::json(v); }, value.value)},
        {"role", roles[unsigned(value.role)]},
        {"meaning", meanings[unsigned(value.meaning)]},
        {"deprecated", value.deprecated},
        {"removed", value.removed}};
    auto& values = schema_->builder->schemas_[id].annotations_["x-mapget"]["namedValues"];
    if (!values.is_null())
        for (auto const& old : values)
            if (old.at("name") == json.at("name")) {
                if (old != json)
                    throw std::invalid_argument("Conflicting named-value declaration.");
                return;
            }
    values.push_back(std::move(json));
}
void MapSchema::setBitWidth(SchemaId id, uint8_t width)
{
    requireMutable();
    schema_->requireDomain(id);
    if (width == 0 || width > 64)
        throw std::invalid_argument("Bitmask width must be 1..64.");
    schema_->builder->schemas_[id].annotations_["x-mapget"]["bitWidth"] = width;
}
std::optional<simfil::Schema::NumericDomain> MapSchema::numericDomain(SchemaId id) const
{
    auto d = domain(id);
    return d ? d->numericDomain() : std::nullopt;
}
void MapSchema::forEachNamedValue(
    SchemaId id,
    std::function<void(simfil::Schema::NamedValue const&)> const& fn) const
{
    if (auto d = domain(id))
        d->forEachNamedValue(fn);
}
uint8_t MapSchema::bitWidth(SchemaId id) const
{
    auto d = domain(id);
    return d ? d->bitWidth() : 0;
}

SchemaId MapSchema::Impl::import(
    schema_detail::SchemaBuilder const& source,
    size_t layerIndex,
    SchemaId root)
{
    std::vector<SchemaId> remap(source.schemas_.size());
    if (source.schemas_.size() - 1 > simfil::MaxSchemaId + 1u - builder->schemas_.size())
        throw std::length_error("Combined MapSchema exceeds its 16-bit domain namespace.");
    auto start = builder->schemas_.size();
    auto aliasesBefore = builder->idsByKey_;
    auto& layer = layers.at(layerIndex);
    auto localBefore = layer.buildAliases;
    try {
        for (size_t i = 1; i < source.schemas_.size(); ++i) {
            if (!source.valid(SchemaId(i)))
                continue;
            auto const& d = source.schemas_[i];
            // Never reinterpret transported local IDs as graph-global IDs.
            remap[i] = builder->allocate(d.kind_, {}, {}, d.entry_.metaType_);
        }
        auto mapped = [&](SchemaId id)
        {
            if (id == 0)
                return SchemaId{0};
            if (id >= remap.size() || !remap[id])
                throw std::invalid_argument("Dangling domain reference in imported schema.");
            return remap[id];
        };
        for (size_t i = 1; i < source.schemas_.size(); ++i)
            if (remap[i]) {
                auto copy = source.schemas_[i];
                schema_detail::SchemaBuilder::validationSchemas(
                    copy.annotations_,
                    [&](nlohmann::json& value, auto const&)
                    {
                        auto reference = value.at("$ref").get<std::string>();
                        constexpr std::string_view prefix = "#/definitions/";
                        if (!reference.starts_with(prefix))
                            throw std::invalid_argument("Unresolved schema validation reference.");
                        uint32_t id = 0;
                        auto digits = std::string_view(reference).substr(prefix.size());
                        auto [end, error] =
                            std::from_chars(digits.data(), digits.data() + digits.size(), id);
                        if (error != std::errc{} || end != digits.data() + digits.size() ||
                            id > simfil::MaxSchemaId)
                            throw std::invalid_argument("Invalid schema validation reference.");
                        value["$ref"] = "#/definitions/" + std::to_string(mapped(SchemaId(id)));
                    });
                copy.id_ = remap[i];
                copy.entry_.id_ = remap[i];
                for (auto& [_, children] : copy.childSchemas_)
                    for (auto& c : children)
                        c = mapped(c);
                for (auto& c : copy.elementSchemas_)
                    c = mapped(c);
                for (auto& c : copy.alternatives_)
                    c = mapped(c);
                for (auto& owner : copy.attributeOwners_)
                    owner.attributeSchema_ = remap[i];
                builder->entriesById_[remap[i]] = copy.entry_;
                builder->schemas_[remap[i]] = std::move(copy);
            }
        for (auto const& [key, id] : source.idsByKey_) {
            auto [it, inserted] = layer.buildAliases.emplace(key, mapped(id));
            if (!inserted && it->second != mapped(id))
                throw std::invalid_argument("Conflicting layer-local schema key: " + key);
        }
    }
    catch (...) {
        builder->schemas_.resize(start);
        builder->entriesById_.resize(start);
        builder->idsByKey_ = std::move(aliasesBefore);
        layer.buildAliases = std::move(localBefore);
        throw;
    }
    return root ? remap.at(root) : 0;
}

LayerSchema::Ptr MapSchema::importLayer(
    std::string_view name,
    LayerType type,
    nlohmann::json const& json,
    std::span<FeatureTypeInfo const> features)
{
    requireMutable();
    schema_detail::SchemaBuilder::validateInput(json);
    auto inputs = nlohmann::json::array();
    for (auto const& info : features) {
        // Reuse the metadata validator, without persisting its JSON representation.
        inputs.push_back(FeatureTypeInfo::fromJson(info.toJson()).toJson());
    }
    auto fingerprint = picosha2::hash256_hex_string(json.dump() + inputs.dump());
    if (auto found = layer(name)) {
        auto const& old = schema_->layerAt(found->addr().index());
        if (old.type == type && old.importedFingerprint == fingerprint)
            return found;
        throw std::invalid_argument(
            "Conflicting declarations for schema layer '" + std::string(name) + "'.");
    }
    schema_detail::SchemaBuilder imported;
    SchemaId importedRoot = 0;
    if (!json.is_null())
        importedRoot = schema_detail::LayerSchemaCompiler(imported, json).buildAll();
    auto start = schema_->builder->schemas_.size();
    auto result = newLayer(name, type);
    auto index = result->addr().index();
    try {
        auto& target = schema_->layers[index];
        target.importedFingerprint = std::move(fingerprint);
        target.root = schema_->import(imported, index, importedRoot);
        auto emitted = schema_->builder->featureRoots(target.root);
        for (auto const& info : features) {
            auto root = result->schemaId(LayerSchema::featureKey(info.name_));
            bool produced = emitted.empty() || std::ranges::find(emitted, root) != emitted.end();
            if (root) {
                auto const& annotations = schema_->builder->schemas_[root].annotations_;
                if (auto metadata = schema_detail::mapgetMetadata(annotations);
                    metadata && metadata->contains("produced"))
                    produced = produced && metadata->at("produced").get<bool>();
            }
            result->addFeatureType(info, root, produced);
        }
        if (!target.root) {
            target.root = result->addSchema(Kind::Object);
            result->setOpen(target.root, true);
        }
    }
    catch (...) {
        schema_->builder->schemas_.resize(start);
        schema_->builder->entriesById_.resize(start);
        schema_->layers.pop_back();
        // Root registration is deferred to finalization; no dangling root survives rollback.
        throw;
    }
    return result;
}

SchemaId MapSchema::addJsonSchema(nlohmann::json const& json)
{
    requireMutable();
    schema_detail::SchemaBuilder::validateInput(json);
    auto start = schema_->builder->schemas_.size();
    auto aliases = schema_->builder->idsByKey_;
    try {
        return schema_detail::LayerSchemaCompiler(*schema_->builder, json).append();
    }
    catch (...) {
        schema_->builder->schemas_.resize(start);
        schema_->builder->entriesById_.resize(start);
        schema_->builder->idsByKey_ = std::move(aliases);
        throw;
    }
}

LayerSchema::LayerSchema(simfil::detail::mp_key key) : ModelNodeBase(key) {}
LayerSchema::LayerSchema(
    simfil::ModelConstPtr owner,
    ModelNodeAddress address,
    simfil::detail::mp_key key)
    : ModelNodeBase(std::move(owner), address, key)
{
}
std::shared_ptr<MapSchema const> LayerSchema::mapSchema() const
{
    return std::static_pointer_cast<MapSchema const>(model_);
}
MapSchema& LayerSchema::mutableOwner()
{
    auto& owner = *const_cast<MapSchema*>(mapSchema().get());
    owner.requireMutable();
    return owner;
}
std::string_view LayerSchema::name() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).name;
}
LayerType LayerSchema::layerType() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).type;
}
void LayerSchema::setRootSchema(SchemaId root)
{
    auto& owner = mutableOwner();
    if (root)
        owner.schema_->requireDomain(root);
    owner.schema_->layerAt(addr_.index()).root = root;
}
SchemaId LayerSchema::rootSchema() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).root;
}
std::span<FeatureTypeInfo const> LayerSchema::featureTypeInfos() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).features;
}
FeatureTypeInfo const* LayerSchema::getTypeInfo(std::string_view name, bool throwIfMissing) const
{
    for (auto const& feature : featureTypeInfos())
        if (feature.name_ == name)
            return &feature;
    if (throwIfMissing)
        throw std::invalid_argument(
            "Unknown feature type '" + std::string(name) + "' in schema layer '" +
            std::string(this->name()) + "'.");
    return nullptr;
}
bool LayerSchema::producesFeatureType(std::string_view name) const
{
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    for (size_t i = 0; i < layer.features.size(); ++i)
        if (layer.features[i].name_ == name)
            return layer.produced[i];
    if (layer.features.empty()) {
        auto types = featureTypes();
        return std::ranges::find(types, name) != types.end();
    }
    return false;
}
std::vector<std::string> LayerSchema::featureTypes() const
{
    std::vector<std::string> result;
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    for (size_t i = 0; i < layer.features.size(); ++i)
        if (layer.produced[i])
            result.push_back(layer.features[i].name_);
    if (layer.features.empty()) {
        auto const& graph = *mapSchema()->schema_;
        auto roots = graph.producedRoots(layer);
        auto appendAliases = [&](auto const& aliases)
        {
            constexpr std::string_view prefix = "Feature:";
            for (auto const& [key, id] : aliases)
                if (key.starts_with(prefix) && key.size() > prefix.size() &&
                    std::ranges::find(roots, id) != roots.end())
                    result.emplace_back(std::string_view(key).substr(prefix.size()));
        };
        // Descriptive type names need not match feature IDs (and may be absent).
        // Only this layer's produced Feature aliases establish those names.
        if (graph.builder)
            appendAliases(layer.buildAliases);
        else
            appendAliases(layer.aliases);
    }
    return result;
}
void LayerSchema::addFeatureType(FeatureTypeInfo info, SchemaId root, bool produced)
{
    auto& owner = mutableOwner();
    info = FeatureTypeInfo::fromJson(info.toJson());
    auto& layer = owner.schema_->layerAt(addr_.index());
    if (!root)
        root = schemaId(featureKey(info.name_));
    if (root)
        owner.schema_->requireDomain(root);
    for (size_t i = 0; i < layer.features.size(); ++i)
        if (layer.features[i].name_ == info.name_) {
            if (layer.features[i].toJson() != info.toJson() || layer.produced[i] != produced ||
                (root && layer.featureRoots[i] != root))
                throw std::invalid_argument(
                    "Conflicting feature identity declaration: " + info.name_);
            return;
        }
    if (!root) {
        root = addSchema(FeatureKind, featureKey(info.name_), "Feature");
        setOpen(root, true);
        setTypeName(root, info.name_);
    }
    registerSchemaKey(featureKey(info.name_), root);
    layer.features.push_back(std::move(info));
    layer.featureRoots.push_back(root);
    layer.produced.push_back(produced);
}
SchemaId
LayerSchema::addSchema(Kind kind, std::string key, std::string metaType, std::string pointer)
{
    auto& owner = mutableOwner();
    auto id = owner.addSchema(kind, {}, std::move(metaType), std::move(pointer));
    owner.schema_->builder->schemas_[id].entry_.key_ = key;
    owner.schema_->builder->entriesById_[id].key_ = key;
    if (!key.empty())
        registerSchemaKey(std::move(key), id);
    return id;
}
void LayerSchema::registerSchemaKey(std::string key, SchemaId id)
{
    auto& owner = mutableOwner();
    owner.schema_->requireDomain(id);
    auto& aliases = owner.schema_->layerAt(addr_.index()).buildAliases;
    auto [it, inserted] = aliases.emplace(std::move(key), id);
    if (!inserted && it->second != id)
        throw std::invalid_argument("Conflicting layer-local schema key: " + it->first);
}
SchemaId LayerSchema::schemaId(std::string_view key) const
{
    auto owner = mapSchema();
    auto const& layer = owner->schema_->layerAt(addr_.index());
    if (owner->schema_->builder) {
        if (auto it = layer.buildAliases.find(key); it != layer.buildAliases.end())
            return it->second;
    }
    else if (auto it = layer.aliases.find(key); it != layer.aliases.end())
        return it->second;
    return owner->findSchema(key);
}
SchemaId LayerSchema::addJsonSchema(nlohmann::json const& json)
{
    auto& owner = mutableOwner();
    auto& graph = *owner.schema_;
    // Compiler resolves converter-shared keys globally, with layer aliases taking precedence.
    auto globals = std::move(graph.builder->idsByKey_);
    graph.builder->idsByKey_ = globals;
    for (auto const& [key, id] : graph.layerAt(addr_.index()).buildAliases)
        graph.builder->idsByKey_[key] = id;
    try {
        auto id = owner.addJsonSchema(json);
        auto& aliases = graph.layerAt(addr_.index()).buildAliases;
        for (auto const& [key, value] : graph.builder->idsByKey_)
            if (!globals.contains(key) || globals.at(key) != value)
                aliases[key] = value;
        graph.builder->idsByKey_ = std::move(globals);
        return id;
    }
    catch (...) {
        graph.builder->idsByKey_ = std::move(globals);
        throw;
    }
}
void LayerSchema::finalize()
{
    if (!mapSchema()->finalized())
        mutableOwner().finalize();
}
LayerSchema::Ptr LayerSchema::fromJsonSchema(nlohmann::json json)
{
    if (json.is_null())
        return {};
    auto owner = std::make_shared<MapSchema>();
    auto layer = owner->importLayer("legacy", LayerType::Features, json);
    owner->finalize();
    return layer;
}

std::vector<SchemaId> MapSchema::Impl::producedRoots(Layer const& layer) const
{
    std::vector<SchemaId> result;
    if (!layer.features.empty()) {
        for (size_t i = 0; i < layer.features.size(); ++i)
            if (layer.produced[i])
                result.push_back(layer.featureRoots[i]);
        return result;
    }
    // Schema-only readers may discover produced roots without inventing feature ID
    // compositions. This does not make such a graph a valid feature creation contract.
    std::vector<SchemaId> pending{layer.root};
    std::vector<bool> seen(builder ? builder->schemas_.size() : domains.size());
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!valid(id) || seen[id])
            continue;
        seen[id] = true;
        if (metaType(id) == "Feature" ||
            (builder ? builder->schemas_[id].kind_ : pool.domain(id)->kind()) ==
                LayerSchema::FeatureKind)
        {
            auto extra = builder ?
                builder->schemas_[id].annotations_ :
                annotation(domains[id].annotations);
            auto metadata = schema_detail::mapgetMetadata(extra);
            if (!metadata || metadata->value("produced", true))
                result.push_back(id);
            continue;
        }
        if (builder) {
            auto const& domain = builder->schemas_[id];
            pending.insert(pending.end(), domain.alternatives_.begin(), domain.alternatives_.end());
            pending.insert(
                pending.end(),
                domain.elementSchemas_.begin(),
                domain.elementSchemas_.end());
        }
        else {
            auto const& domain = *pool.domain(id);
            auto alternatives = domain.alternatives();
            pending.insert(pending.end(), alternatives.begin(), alternatives.end());
            domain.forEachElementSchema([&](auto child) { pending.push_back(child); });
        }
    }
    std::ranges::sort(result);
    return result;
}

void MapSchema::Impl::prepareQueryRoots()
{
    std::set<std::pair<SchemaId, SchemaId>> contexts;
    for (auto const& layer : layers)
        for (auto feature : producedRoots(layer)) {
            std::vector<SchemaId> pending{feature};
            std::vector<bool> seen(builder->schemas_.size());
            while (!pending.empty()) {
                auto id = pending.back();
                pending.pop_back();
                if (!builder->valid(id) || seen[id])
                    continue;
                seen[id] = true;
                auto const& d = builder->schemas_[id];
                if (d.entry_.metaType_ == "Attribute")
                    contexts.emplace(feature, id);
                for (auto const& [_, children] : d.childSchemas_)
                    pending.insert(pending.end(), children.begin(), children.end());
                pending.insert(pending.end(), d.elementSchemas_.begin(), d.elementSchemas_.end());
                pending.insert(pending.end(), d.alternatives_.begin(), d.alternatives_.end());
            }
        }
    if (contexts.empty())
        return;
    for (auto const& [context, _] : queryRoots)
        contexts.erase(context);
    if (contexts.empty())
        return;
    auto string = builder->allocate(Kind::String, {}, {}, {});
    auto integer = builder->allocate(Kind::Int, {}, {}, {});
    auto boolean = builder->allocate(Kind::Bool, {}, {}, {});
    for (auto id : {string, integer, boolean}) {
        builder->schemas_[id].queryRoot_ = true;
        builder->schemas_[id].nullable_ = false;
    }
    for (auto [feature, attribute] : contexts) {
        auto copy = builder->schemas_[attribute];
        auto id = builder->allocate(LayerSchema::AttributeKind, {}, {}, "Attribute");
        copy.id_ = id;
        copy.entry_ = builder->entriesById_[id];
        copy.queryRoot_ = true;
        // Only overlay edges are stored; the payload is referenced by DomainData::base.
        copy.directFields_.clear();
        copy.childSchemas_.clear();
        copy.required_.clear();
        copy.contextFields_.clear();
        copy.directEnumSymbols_.clear();
        copy.enumValues_.clear();
        copy.elementSchemas_.clear();
        // A named Attribute may describe several physical branches. Its query
        // context must retain those alternatives rather than hide their fields.
        builder->schemas_[id] = std::move(copy);
        auto overlay = [&](std::string_view name, SchemaId child)
        {
            builder->addDirectField(id, name);
            builder->addChild(id, name, child);
            builder->schemas_[id].required_[std::string(name)] = true;
            builder->schemas_[id].contextFields_.emplace(name);
        };
        overlay("$name", string);
        overlay("$layer", string);
        overlay("$feature", feature);
        overlay("$attributeIndex", integer);
        overlay("$validityIndex", integer);
        overlay("$validityCount", integer);
        overlay("$hasValidity", boolean);
        queryRoots.emplace(std::pair{feature, attribute}, id);
    }
}

void MapSchema::Impl::compileScalarMetadata(
    SchemaId id,
    schema_detail::SchemaBuilder::LogicalSchema const& source,
    nlohmann::json& extra)
{
    auto& generic = static_cast<simfil::SchemaPool&>(pool);
    auto kindName = simfil::Schema::kindNameId(source.kind_);
    auto explicitMetadata = schema_detail::mapgetMetadata(extra);
    if (source.composition_ == Composition::None &&
        (kindName == simfil::Schema::kindNameId(Kind::Int) ||
         kindName == simfil::Schema::kindNameId(Kind::Float)) &&
        (!explicitMetadata || !explicitMetadata->contains("numericDomain")))
    {
        auto inferred = nlohmann::json::object();
        for (auto key : {"minimum", "maximum"})
            if (auto bound = extra.find(key); bound != extra.end() && bound->is_number())
                inferred[key] = *bound;
        if (!inferred.empty())
            extra["x-mapget"]["numericDomain"] = std::move(inferred);
    }
    auto bits = simfil::Schema::affinities(source.kind_);
    auto mask = simfil::valueTypeAffinity(simfil::ValueType::Bitmask);
    auto isMask = source.composition_ == Composition::None && (bits & mask) &&
        !(bits & ~(mask | simfil::valueTypeAffinity(simfil::ValueType::Null)));
    auto metadata = extra.find("x-mapget");
    if (metadata == extra.end()) {
        if (isMask)
            generic.setBitWidth(id, 64);
        return;
    }
    if (auto declarations = metadata->find("relationDeclarations"); declarations != metadata->end())
    {
        if (!declarations->is_array())
            throw std::invalid_argument("relationDeclarations must be an array.");
        for (auto& relation : *declarations) {
            if (!relation.contains("complete") && relation.contains("coverage"))
                relation["complete"] = relation.at("coverage") == "complete";
            relation.erase("coverage");
            relation.erase("incoming");
        }
    }
    if (metadata->contains("bitWidth") && !isMask)
        throw std::invalid_argument("bitWidth requires a concrete raw-bitmask domain.");
    auto width = metadata->value("bitWidth", isMask ? 64u : 0u);
    if ((isMask && !width) || width > 64)
        throw std::invalid_argument("Bitmask width must be 1..64.");
    if (isMask)
        generic.setBitWidth(id, uint8_t(width));
    metadata->erase("bitWidth");
    auto numericValue = [](nlohmann::json const& value) -> simfil::Schema::NumericValue
    {
        if (value.is_number_unsigned())
            return value.get<uint64_t>();
        if (value.is_number_integer())
            return value.get<int64_t>();
        if (value.is_number_float())
            return value.get<double>();
        throw std::invalid_argument("Numeric domain metadata must be numeric.");
    };
    if (auto values = metadata->find("namedValues"); values != metadata->end()) {
        if (!values->is_array())
            throw std::invalid_argument("namedValues must be an array.");
        std::set<std::string> seen;
        for (auto const& value : *values) {
            simfil::Schema::NamedValue named;
            auto name = value.at("name").get<std::string>();
            if (name.empty() || !seen.insert(name).second)
                throw std::invalid_argument("Duplicate or empty named-value name.");
            named.name = name;
            auto const& scalar = value.at("value");
            if (scalar.is_boolean())
                named.value = scalar.get<bool>();
            else if (scalar.is_string())
                named.value = std::string_view(scalar.get_ref<std::string const&>());
            else
                std::visit([&](auto number) { named.value = number; }, numericValue(scalar));
            auto role = value.value("role", "constant");
            if (role != "constant" && role != "special" && role != "flag")
                throw std::invalid_argument("Invalid named-value role.");
            named.role = role == "flag" ?
                simfil::Schema::NamedValue::Role::Flag :
                role == "special" ?
                simfil::Schema::NamedValue::Role::Special :
                simfil::Schema::NamedValue::Role::Constant;
            std::array<std::string_view, 5>
                meanings{"ordinary", "unknown", "variable", "unbounded", "saturated"};
            auto meaning = value.value("meaning", "ordinary");
            auto found = std::ranges::find(meanings, meaning);
            if (found == meanings.end())
                throw std::invalid_argument("Invalid named-value meaning.");
            named.meaning = simfil::Schema::NamedValue::Meaning(found - meanings.begin());
            named.deprecated = value.value("deprecated", false);
            named.removed = value.value("removed", false);
            generic.addNamedValue(id, named);
        }
        metadata->erase("namedValues");
    }
    if (auto numeric = metadata->find("numericDomain"); numeric != metadata->end()) {
        simfil::Schema::NumericDomain domain;
        auto encoding = numeric->value("encoding", ""), unit = numeric->value("unit", "");
        domain.encoding = encoding;
        domain.unit = unit;
        if (auto minimum = numeric->find("minimum"); minimum != numeric->end())
            domain.minimum = numericValue(*minimum);
        if (auto maximum = numeric->find("maximum"); maximum != numeric->end())
            domain.maximum = numericValue(*maximum);
        domain.complete = numeric->value("complete", false);
        domain.scale = numeric->value("scale", 1.);
        domain.offset = numeric->value("offset", 0.);
        std::vector<simfil::Schema::NumericRange> ranges;
        if (auto input = numeric->find("ordinaryRanges"); input != numeric->end())
            for (auto const& range : *input)
                ranges.push_back(
                    {numericValue(range.at("minimum")), numericValue(range.at("maximum"))});
        domain.ordinaryRanges = ranges;
        generic.setNumericDomain(id, domain);
        metadata->erase("numericDomain");
    }
    if (metadata->empty())
        extra.erase("x-mapget");
}

void MapSchema::Impl::compile()
{
    for (auto& layer : layers)
        if (!layer.root) {
            std::vector<SchemaId> roots;
            for (size_t i = 0; i < layer.featureRoots.size(); ++i)
                if (layer.produced[i])
                    roots.push_back(layer.featureRoots[i]);
            if (roots.size() == 1)
                layer.root = roots.front();
            else {
                layer.root =
                    builder->allocate(roots.empty() ? Kind::Unknown : Kind::Union, {}, {}, {});
                if (!roots.empty()) {
                    builder->schemas_[layer.root].composition_ = Composition::AnyOf;
                    builder->schemas_[layer.root].alternatives_ = std::move(roots);
                }
            }
        }
    prepareQueryRoots();
    auto& generic = static_cast<simfil::SchemaPool&>(pool);
    domains.resize(builder->schemas_.size());
    for (size_t id = 1; id < builder->schemas_.size(); ++id) {
        if (!builder->valid(SchemaId(id)) || generic.addSchema(builder->schemas_[id].kind_) != id)
            throw std::invalid_argument("MapSchema has a hole in its domain namespace.");
    }
    for (size_t id = 1; id < builder->schemas_.size(); ++id) {
        auto const& source = builder->schemas_[id];
        DomainData row{};
        row.flags = uint32_t(source.queryRoot_) << 3;
        row.key = text(source.entry_.key_);
        row.pointer = text(source.entry_.jsonPointer_);
        row.metaType = text(source.entry_.metaType_);
        row.zserioType = text(source.zserioType_);
        row.attributeType = text(source.attributeType_);
        row.attributeCode = text(source.attributeTypeCode_);
        generic.setTypeName(SchemaId(id), source.typeName_);
        generic.setOpen(SchemaId(id), source.open_);
        generic.setNullable(SchemaId(id), source.nullable_);
        generic.setComposition(SchemaId(id), source.composition_);
        for (auto [context, query] : queryRoots)
            if (query == id)
                generic.setBaseSchema(SchemaId(id), context.second);
        for (auto const& name : source.directFields_) {
            auto children = source.childSchemas_.find(name);
            if (children == source.childSchemas_.end() || children->second.empty())
                generic.addFieldSchema(
                    SchemaId(id),
                    name,
                    simfil::NoSchemaId,
                    source.multimapFields_.contains(name));
            else
                for (auto child : children->second)
                    generic.addFieldSchema(
                        SchemaId(id),
                        name,
                        child,
                        source.multimapFields_.contains(name));
            if (auto it = source.required_.find(name); it != source.required_.end())
                generic.setFieldRequired(SchemaId(id), name, it->second);
            if (source.contextFields_.contains(name))
                generic.setFieldRole(SchemaId(id), name, simfil::Schema::FieldRole::Context);
        }
        // Legacy imports also keep the implicit alias in childSchemas_, but only
        // an actual declared field may override the native lookup-only alias.
        if (simfil::Schema::kindNameId(source.kind_) ==
                simfil::Schema::kindNameId(LayerSchema::FeatureKind) &&
            source.childSchemas_.contains("properties") &&
            std::ranges::find(source.directFields_, "attributes") == source.directFields_.end())
            generic.addFieldAlias(SchemaId(id), "attributes", "properties");
        for (auto child : source.elementSchemas_)
            generic.addElementSchema(SchemaId(id), child);
        for (auto child : source.alternatives_)
            generic.addAlternative(SchemaId(id), child);
        for (auto const& symbol : source.directEnumSymbols_)
            generic.addEnumSymbol(SchemaId(id), symbol);
        for (auto const& value : source.enumValues_)
            generic.addEnumValue(SchemaId(id), value);
        row.owners.begin = owners.size();
        for (auto const& owner : source.attributeOwners_)
            owners.push_back(
                {intern(owner.featureType_),
                 intern(owner.attributeLayerName_),
                 intern(owner.attributeName_),
                 0});
        row.owners.count = owners.size() - row.owners.begin;
        auto extra = source.annotations_.is_null() ? nlohmann::json::object() : source.annotations_;
        compileScalarMetadata(SchemaId(id), source, extra);
        row.annotations = annotation(extra);
        domains[id] = row;
    }
    for (auto const& [key, id] : builder->idsByKey_)
        aliases.push_back({text(key), id, 0});
    for (size_t i = 0; i < layers.size(); ++i) {
        for (auto const& [key, id] : layers[i].buildAliases)
            aliases.push_back({text(key), id, uint16_t(i + 1)});
        layers[i].buildAliases.clear();
        layers[i].importedFingerprint.clear();
    }
    for (size_t i = 0; i < descriptorKeys.size(); ++i)
        descriptorKeys[i] = intern(std::array<std::string_view, 12>{
            "name",
            "layerType",
            "root",
            "featureTypes",
            "id",
            "kind",
            "typename",
            "fields",
            "items",
            "alternatives",
            "enum",
            "open"}[i]);
    builder.reset();
    pool.finalizeSchema();
    textIndex.clear();
    textIndex.rehash(0);
}

void MapSchema::Impl::index()
{
    entries.clear();
    globalAliases.clear();
    for (size_t i = 0; i < descriptorKeys.size(); ++i) {
        descriptorKeys[i] = pool.strings()->get(std::array<std::string_view, 12>{
            "name",
            "layerType",
            "root",
            "featureTypes",
            "id",
            "kind",
            "typename",
            "fields",
            "items",
            "alternatives",
            "enum",
            "open"}[i]);
        if (!descriptorKeys[i])
            throw std::invalid_argument("Missing schema descriptor vocabulary.");
    }
    for (auto& layer : layers)
        layer.aliases.clear();
    entries.resize(domains.size());
    for (auto const& alias : aliases) {
        auto& index = alias.layer ? layers.at(alias.layer - 1).aliases : globalAliases;
        auto [it, inserted] = index.emplace(text(alias.key), alias.id);
        if (!inserted && it->second != alias.id)
            throw std::invalid_argument("Conflicting stored schema aliases.");
    }
    for (size_t id = 1; id < domains.size(); ++id) {
        auto const& row = domains[id];
        entries[id] = {SchemaId(id), text(row.key), text(row.pointer), text(row.metaType)};
    }
    // Resolve forward contracts once for each produced layer. Referenced-only
    // identity types do not make that layer a source of incoming relations.
    size_t work = 0;
    for (auto& layer : layers) {
        layer.relations.clear();
        layer.relationsByTarget.clear();
        layer.relationsKnown = false;
        std::vector<SchemaId> pending{layer.root};
        auto produced = producedRoots(layer);
        auto hasInventory = [&](SchemaId id)
        {
            if (!valid(id))
                return false;
            auto value = annotation(domains[id].annotations);
            auto meta = schema_detail::mapgetMetadata(value);
            return meta && meta->contains("relationDeclarations") &&
                meta->value("relationDeclarationsKnown", true);
        };
        bool layerWide = valid(layer.root) && metaType(layer.root) != "Feature" &&
            simfil::Schema::kindNameId(pool.domain(layer.root)->kind()) !=
                simfil::Schema::kindNameId(LayerSchema::FeatureKind) &&
            hasInventory(layer.root);
        layer.relationsKnown = layerWide ||
            (!produced.empty() && std::ranges::all_of(produced, hasInventory));
        pending.insert(pending.end(), produced.begin(), produced.end());
        std::vector<bool> seen(domains.size());
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!valid(id) || seen[id])
                continue;
            seen[id] = true;
            ++work;
            if (work > 64 * 1024 * 1024)
                throw std::length_error("MapSchema relation-index work budget exceeded.");
            auto annotations = annotation(domains[id].annotations);
            auto meta = annotations.find("x-mapget");
            if (meta != annotations.end())
                if (auto declarations = meta->find("relationDeclarations");
                    declarations != meta->end())
                {
                    if (!declarations->is_array())
                        throw std::invalid_argument("relationDeclarations must be an array.");
                    for (auto const& input : *declarations) {
                        LayerSchema::RelationDeclaration value{
                            input.at("ownerType").get<std::string>(),
                            input.at("name").get<std::string>(),
                            input.at("targetTypes").get<std::vector<std::string>>(),
                            input.value("complete", false)};
                        auto locality = input.value("locality", "unknown");
                        using Locality = LayerSchema::RelationDeclaration::Locality;
                        if (locality == "same-container")
                            value.locality_ = Locality::SameContainer;
                        else if (locality == "may-cross-container")
                            value.locality_ = Locality::MayCrossContainer;
                        else if (locality != "unknown")
                            throw std::invalid_argument("Invalid relation locality.");
                        if (!pool.layer(layer.name)->producesFeatureType(value.ownerType_))
                            continue;
                        if (value.name_.empty() || value.targetTypes_.empty())
                            throw std::invalid_argument("Incomplete forward relation declaration.");
                        auto old = std::ranges::find_if(
                            layer.relations,
                            [&](auto const& d)
                            { return d.ownerType_ == value.ownerType_ && d.name_ == value.name_; });
                        if (old != layer.relations.end()) {
                            if (old->targetTypes_ != value.targetTypes_ ||
                                old->complete_ != value.complete_ ||
                                old->locality_ != value.locality_)
                                throw std::invalid_argument(
                                    "Conflicting forward relation declaration.");
                            continue;
                        }
                        auto index = layer.relations.size();
                        for (auto const& target : value.targetTypes_)
                            layer.relationsByTarget[target].push_back(index);
                        layer.relations.push_back(std::move(value));
                    }
                }
            auto append = [&](SchemaId child)
            {
                if (++work > 64 * 1024 * 1024)
                    throw std::length_error("MapSchema relation-index work budget exceeded.");
                pending.push_back(child);
            };
            auto const& domain = *pool.domain(id);
            domain.forEachEnumerableField(
                [&](auto, auto children)
                {
                    for (auto child : children)
                        append(child);
                });
            domain.forEachElementSchema(append);
            for (auto child : domain.alternatives())
                append(child);
        }
    }
}

void MapSchema::Impl::validate() const
{
    (void)MapFormat::fromJson(format.toJson());
    std::set<std::string_view> constituentIds;
    for (auto const& [id, format] : constituents) {
        constexpr std::string_view prefix = "mapget-schema:sha256:";
        if (!id.starts_with(prefix) || id.size() != prefix.size() + 64 ||
            id.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos ||
            !constituentIds.insert(id).second)
            throw std::invalid_argument("Invalid or duplicate constituent schema identity.");
        (void)MapFormat::fromJson(format.toJson());
    }
    if (domains.size() != pool.simfil::SchemaPool::domainCount() + 1)
        throw std::invalid_argument("Map metadata and generic domain namespaces disagree.");
    auto domain = [&](SchemaId id)
    {
        if (!id || id >= domains.size() || !pool.domain(id))
            throw std::invalid_argument("Dangling MapSchema domain reference.");
    };
    size_t ownerEnd = 0;
    for (size_t id = 1; id < domains.size(); ++id) {
        auto const& d = domains[id];
        if (d.flags & ~8u)
            throw std::invalid_argument("Invalid map-domain flags.");
        for (auto address :
             {d.key, d.pointer, d.metaType, d.zserioType, d.attributeType, d.attributeCode})
            text(address);
        if (d.owners.count && d.owners.begin != ownerEnd)
            throw std::invalid_argument("Attribute owner slices must be canonical.");
        for (auto const& owner : d.owners.in(owners)) {
            name(owner.feature);
            name(owner.layer);
            name(owner.name);
            if (owner.reserved)
                throw std::invalid_argument("Invalid attribute owner flags.");
        }
        ownerEnd += d.owners.count;
        if (d.annotations)
            (void)annotation(d.annotations);
        if (auto base = pool.baseSchema(SchemaId(id))) {
            if (base >= id || !(d.flags & 8) || pool.baseSchema(base) ||
                (domains[base].flags & 8) ||
                simfil::Schema::kindNameId(pool.domain(base)->kind()) !=
                    simfil::Schema::kindNameId(LayerSchema::AttributeKind))
                throw std::invalid_argument(
                    "Query overlays must refer directly to a non-overlay attribute payload.");
        }
    }
    if (ownerEnd != owners.size())
        throw std::invalid_argument("Unreferenced attribute ownership storage.");
    for (auto const& alias : aliases) {
        domain(alias.id);
        text(alias.key);
        if (alias.layer > layers.size())
            throw std::invalid_argument("Unknown schema alias layer.");
    }
    std::set<SchemaId> overlays;
    for (auto const& [context, root] : queryRoots) {
        domain(context.first);
        domain(context.second);
        domain(root);
        if (pool.baseSchema(root) != context.second || !overlays.insert(root).second ||
            simfil::Schema::kindNameId(pool.domain(context.first)->kind()) !=
                simfil::Schema::kindNameId(LayerSchema::FeatureKind))
            throw std::invalid_argument("Invalid query overlay binding.");
    }
    for (size_t id = 1; id < domains.size(); ++id)
        if (bool(pool.baseSchema(SchemaId(id))) != overlays.contains(SchemaId(id)))
            throw std::invalid_argument(
                "Query overlay bindings must cover every overlay exactly once.");
    std::set<std::string> names;
    for (auto const& layer : layers) {
        if (layer.name.empty() || !names.insert(layer.name).second ||
            unsigned(layer.type) > unsigned(LayerType::SourceData))
            throw std::invalid_argument("Invalid or duplicate schema layer.");
        if (layer.root)
            domain(layer.root);
        if (layer.features.size() != layer.featureRoots.size() ||
            layer.features.size() != layer.produced.size())
            throw std::invalid_argument("Invalid feature identity column lengths.");
        std::set<std::string> types;
        for (size_t i = 0; i < layer.features.size(); ++i) {
            auto const& feature = layer.features[i];
            (void)FeatureTypeInfo::fromJson(feature.toJson());
            if (!types.insert(feature.name_).second || layer.produced[i] > 1)
                throw std::invalid_argument("Invalid or duplicate feature declaration.");
            domain(layer.featureRoots[i]);
        }
    }
}

void MapSchema::Impl::validatePool() const
{
    // Schema payloads use primitive values only. Never walk arbitrary embedded
    // arrays/objects (including malicious cycles) via ModelPool::checkForErrors.
    auto const& model = std::as_const(pool);
    auto stats = model.memoryUsageStats();
    if (model.objectMemberStorage().size() != size_t(simfil::FirstRegularArrayIndex) ||
        model.arrayMemberStorage().size() != size_t(simfil::FirstRegularArrayIndex) ||
        model.objectMemberStorage().singleton_handle_count() ||
        model.arrayMemberStorage().singleton_handle_count() || stats.objectSchemas.logicalBytes ||
        stats.arraySchemas.logicalBytes || stats.qualifiedScalars.logicalBytes ||
        stats.bitmasks.logicalBytes || stats.schemaBindings.logicalBytes)
        throw std::invalid_argument(
            "MapSchema payload may not contain generic compound/qualified values.");
    if (model.numRoots() != layers.size())
        throw std::invalid_argument("Invalid MapSchema root count.");
    for (size_t i = 0; i < layers.size(); ++i) {
        auto root = model.root(i);
        if (!root || (*root)->addr() != ModelNodeAddress{Layers, i})
            throw std::invalid_argument("MapSchema roots must exactly identify its layer views.");
    }
}

/** Serialize canonical columns only. Derived sets/views/indices are rebuilt once.
 * Every variable length is bounded independently of the envelope byte limit.
 */
template <class S, class T, class F>
void MapSchema::Impl::container(S& s, std::vector<T>& data, size_t limit, size_t& memory, F&& item)
{
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        auto position = s.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(s.adapter(), count, limit, std::true_type{});
        // Every item here has at least one wire byte. Reject truncated lengths
        // and charge all nested allocations before bitsery resizes the container.
        if (count > memory / sizeof(T) ||
            !simfil::detail::check_bitsery_payload_available(s.adapter(), count))
            throw std::length_error("MapSchema container exceeds its input/allocation budget.");
        memory -= count * sizeof(T);
        s.adapter().currentReadPos(position);
    }
    s.container(data, limit, std::forward<F>(item));
}
template <class S>
void MapSchema::Impl::string(S& s, std::string& value, size_t limit, size_t& memory)
{
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        auto position = s.adapter().currentReadPos();
        size_t count = 0;
        bitsery::details::readSize(s.adapter(), count, limit, std::true_type{});
        if (count > memory || !simfil::detail::check_bitsery_payload_available(s.adapter(), count))
            throw std::length_error("MapSchema text exceeds its input/allocation budget.");
        memory -= count;
        s.adapter().currentReadPos(position);
    }
    s.text1b(value, limit);
}

template <class S>
void MapSchema::Impl::readWrite(S& s, size_t budget)
{
    size_t memory = budget > SIZE_MAX / 4 ? SIZE_MAX : budget * 4;
    auto text = [&](auto& a, std::string& v)
    {
        string(a, v, budget, memory);
    };
    auto vector = [&]<class T>(auto& a, std::vector<T>& values, size_t limit)
    {
        container(
            a,
            values,
            limit,
            memory,
            [](auto& b, T& v)
            {
                if constexpr (std::is_same_v<T, uint8_t>)
                    b.value1b(v);
                else if constexpr (std::is_same_v<T, SchemaId>)
                    b.value2b(v);
                else
                    b.object(v);
            });
    };
    auto mapFormat = [&](auto& archive, MapFormat& value)
    {
        text(archive, value.name_);
        text(archive, value.version_);
        std::vector<std::pair<std::string, std::string>> subsystems;
        if constexpr (!simfil::detail::bitsery_input_archive<S>)
            for (auto const& pair : value.subsystems_)
                subsystems.push_back(pair);
        container(
            archive,
            subsystems,
            UINT16_MAX,
            memory,
            [&](auto& a, auto& pair)
            {
                text(a, pair.first);
                text(a, pair.second);
            });
        if constexpr (simfil::detail::bitsery_input_archive<S>)
            for (auto const& pair : subsystems)
                if (!value.subsystems_.emplace(pair).second)
                    throw std::invalid_argument("Duplicate format subsystem.");
    };
    mapFormat(s, format);
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        uint64_t length = 0;
        s.value8b(length);
        if (length > (size_t(simfil::MaxSchemaId) + 1) * sizeof(DomainData) || length > budget ||
            length > memory || length % sizeof(DomainData))
            throw std::length_error("MapSchema domain column exceeds its namespace/byte budget.");
        memory -= length;
        if (!domains.read_payload_from_bitsery(s.adapter(), size_t(length)))
            throw std::invalid_argument("Truncated MapSchema domain column.");
    }
    else
        s.object(domains);
    vector(s, owners, std::min(MaxEdges, budget / sizeof(OwnerData)));
    vector(s, aliases, std::min(MaxEdges, budget / sizeof(AliasData)));
    container(
        s,
        layers,
        UINT16_MAX,
        memory,
        [&](auto& a, Layer& layer)
        {
            text(a, layer.name);
            a.value4b(layer.type);
            a.value2b(layer.root);
            container(
                a,
                layer.features,
                UINT16_MAX,
                memory,
                [&](auto& b, FeatureTypeInfo& info)
                {
                    text(b, info.name_);
                    container(
                        b,
                        info.uniqueIdCompositions_,
                        UINT16_MAX,
                        memory,
                        [&](auto& c, auto& composition)
                        {
                            container(
                                c,
                                composition,
                                16,
                                memory,
                                [&](auto& d, IdPart& part)
                                {
                                    text(d, part.idPartLabel_);
                                    text(d, part.description_);
                                    d.value4b(part.datatype_);
                                    d.boolValue(part.isSynthetic_);
                                    d.boolValue(part.isOptional_);
                                });
                        });
                });
            vector(a, layer.featureRoots, UINT16_MAX);
            vector(a, layer.produced, UINT16_MAX);
        });
    std::vector<std::array<SchemaId, 3>> queries;
    if constexpr (!simfil::detail::bitsery_input_archive<S>)
        for (auto [context, root] : queryRoots)
            queries.push_back({context.first, context.second, root});
    container(
        s,
        queries,
        UINT16_MAX,
        memory,
        [](auto& a, auto& v)
        {
            for (auto& id : v)
                a.value2b(id);
        });
    if constexpr (simfil::detail::bitsery_input_archive<S>) {
        for (auto const& v : queries)
            if (!queryRoots.emplace(std::pair{v[0], v[1]}, v[2]).second)
                throw std::invalid_argument("Duplicate query-root binding.");
    }
    container(
        s,
        constituents,
        256,
        memory,
        [&](auto& archive, auto& constituent)
        {
            string(archive, constituent.first, 128, memory);
            mapFormat(archive, constituent.second);
        });
}

void MapSchema::Impl::writeBody(std::ostream& output) const
{
    bitsery::Serializer<bitsery::OutputStreamAdapter> s(output);
    const_cast<Impl*>(this)->readWrite(s, UINT32_MAX);
    std::ostringstream generic(std::ios::binary);
    pool.writeSchemaPayload(generic);
    auto data = generic.str();
    s.text1b(data, UINT32_MAX);
    s.adapter().flush();
    if (!output)
        throw std::runtime_error("Failed to write MapSchema payload.");
}
void MapSchema::Impl::readBody(std::vector<uint8_t> const& input, size_t budget)
{
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
    builder.reset();
    readWrite(s, budget);
    std::string generic;
    size_t memory = budget;
    string(s, generic, budget, memory);
    if (s.adapter().error() != bitsery::ReaderError::NoError ||
        !s.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Truncated, malformed or trailing MapSchema data.");
    pool.readSchemaPayload(
        std::span(reinterpret_cast<uint8_t const*>(generic.data()), generic.size()),
        budget);
}

void MapSchema::finalize()
{
    if (finalized())
        return;
    requireMutable();
    // After packing has started, failures poison only this unpublished builder.
    // Callers retry getOrBuild with a fresh graph, never publish partially frozen state.
    schema_->compile();
    schema_->validate();
    schema_->index();
    for (auto const& name : layerNames())
        addRoot(layer(name));
    schema_->validatePool();
    std::ostringstream body(std::ios::binary);
    schema_->writeBody(body);
    schema_->identity = "mapget-schema:sha256:" + picosha2::hash256_hex_string(body.str());
    ModelPool::freeze();
    schema_->frozen = true;
    schema_->summarize();
}
tl::expected<void, simfil::Error> MapSchema::write(std::ostream& output)
{
    return std::as_const(*this).write(output);
}
tl::expected<void, simfil::Error> MapSchema::write(std::ostream& output) const
{
    try {
        if (!finalized())
            throw std::logic_error("Cannot serialize an unfinished MapSchema.");
        std::ostringstream data(std::ios::binary);
        schema_->writeBody(data);
        auto payload = data.str();
        bitsery::Serializer<bitsery::OutputStreamAdapter> s(output);
        uint32_t magic = 0x4843534d;  // MSCH
        uint32_t version = Impl::CodecVersion;
        s.value4b(magic);
        s.value4b(version);
        auto id = schema_->identity;
        s.text1b(id, 128);
        s.text1b(payload, UINT32_MAX);
        s.adapter().flush();
        if (!output)
            throw std::runtime_error("Failed to write MapSchema envelope.");
        return {};
    }
    catch (std::exception const& e) {
        return tl::unexpected(simfil::Error{simfil::Error::EncodeDecodeError, e.what()});
    }
}
MapSchema::ConstPtr
MapSchema::read(std::span<uint8_t const> bytes, std::string_view expectedId, size_t maxBytes)
{
    if (bytes.size() > maxBytes || bytes.size() < 8)
        throw std::length_error("MapSchema artifact exceeds its byte budget or is truncated.");
    std::vector<uint8_t> input(bytes.begin(), bytes.end());
    using Adapter = bitsery::InputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Deserializer<Adapter> s(Adapter(input.begin(), input.end()));
    uint32_t magic = 0, version = 0;
    s.value4b(magic);
    s.value4b(version);
    if (magic != 0x4843534d || version != Impl::CodecVersion)
        throw std::invalid_argument("Unsupported MapSchema binary format.");
    std::string identity, payload;
    size_t memory = bytes.size();
    Impl::string(s, identity, 128, memory);
    Impl::string(s, payload, maxBytes, memory);
    if (s.adapter().error() != bitsery::ReaderError::NoError ||
        !s.adapter().isCompletedSuccessfully())
        throw std::invalid_argument("Malformed MapSchema envelope.");
    if (!expectedId.empty() && identity != expectedId)
        throw std::invalid_argument("MapSchema identity does not match the requested artifact.");
    if (identity != "mapget-schema:sha256:" + picosha2::hash256_hex_string(payload))
        throw std::invalid_argument("MapSchema content checksum mismatch.");
    // Every input is verified before consulting the cache; a warm artifact must
    // never conceal a truncated body or a wrong expected identity.
    return getOrBuild(
        identity,
        [identity, payload = std::move(payload), maxBytes]
        {
            auto result = std::make_shared<MapSchema>();
            result->schema_
                ->readBody(std::vector<uint8_t>(payload.begin(), payload.end()), maxBytes);
            result->schema_->validate();
            result->schema_->validatePool();
            result->schema_->index();
            // Reject noncanonical identities, hidden data and duplicate dictionary IDs.
            std::ostringstream canonical(std::ios::binary);
            result->schema_->writeBody(canonical);
            if (canonical.str() != payload)
                throw std::invalid_argument("Noncanonical MapSchema payload.");
            result->schema_->identity = identity;
            result->ModelPool::freeze();
            result->schema_->frozen = true;
            result->schema_->summarize();
            return result;
        });
}

MapSchema::ConstPtr MapSchema::getOrBuild(
    std::string key,
    std::function<ConstPtr()> builder,
    std::function<bool()> cancel)
{
    // Four distinct builds may run concurrently; no second unbounded work queue.
    // Workers retain the cache owner so process-static teardown cannot free a
    // mutex beneath detached construction. Completed entries hold only weak owners.
    static auto cache = std::make_shared<Impl::BuildCache>();
    auto& activeKeys = Impl::activeBuildKeys();
    if (key.empty() || !builder)
        throw std::invalid_argument("Schema construction requires a key and builder.");
    if (std::ranges::find(activeKeys, key) != activeKeys.end())
        throw std::logic_error("Reentrant construction of the same schema artifact.");
    bool nested = !activeKeys.empty();
    std::shared_future<ConstPtr> future;
    for (;;) {
        if (cancel && cancel())
            throw std::runtime_error("Schema wait cancelled.");
        std::unique_lock lock(cache->mutex);
        for (auto it = cache->ready.begin(); it != cache->ready.end();)
            if (it->second.expired())
                it = cache->ready.erase(it);
            else
                ++it;
        if (auto found = cache->ready.find(key); found != cache->ready.end())
            if (auto owner = found->second.lock())
                return owner;
        if (auto found = cache->running.find(key); found != cache->running.end()) {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
            throw std::logic_error("Reentrant construction of the same schema artifact.");
#else
            future = found->second;
            break;
#endif
        }
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        if (!nested && cache->running.size() >= 4) {
            cache->changed.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
#endif
        auto promise = std::make_shared<std::promise<ConstPtr>>();
        future = promise->get_future().share();
        cache->running.emplace(key, future);
        auto build = [cache = cache, key, promise, build = std::move(builder)]
        {
            auto& activeKeys = Impl::activeBuildKeys();
            activeKeys.push_back(key);
            auto leave = [](auto* keys)
            {
                keys->pop_back();
            };
            std::unique_ptr<std::vector<std::string>, decltype(leave)> active(&activeKeys, leave);
            try {
                auto owner = build();
                if (!owner || !owner->finalized())
                    throw std::invalid_argument("Schema builder did not return a finalized owner.");
                std::lock_guard lock(cache->mutex);
                if (cache->ready.size() >= 256)
                    cache->ready.erase(cache->ready.begin());
                cache->ready[key] = owner;
                cache->running.erase(key);
                promise->set_value(owner);
            }
            catch (...) {
                std::lock_guard lock(cache->mutex);
                cache->running.erase(key);
                promise->set_exception(std::current_exception());
            }
            cache->changed.notify_all();
        };
        try {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
            // The browser build has no pthread runtime. It still shares the
            // completed immutable artifact but constructs it on this call stack.
            lock.unlock();
            build();
#else
            // A network builder may decode a second, content-addressed key. Reuse
            // this worker instead of waiting for a fifth slot behind four parents.
            if (nested) {
                lock.unlock();
                build();
            }
            else
                std::thread(std::move(build)).detach();
#endif
        }
        catch (...) {
            cache->running.erase(key);
            cache->changed.notify_all();
            throw;
        }
        break;
    }
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
        if (cancel && cancel())
            throw std::runtime_error("Schema wait cancelled.");
    if (cancel && cancel())
        throw std::runtime_error("Schema wait cancelled.");
    return future.get();
}

simfil::Schema const* MapSchema::domain(SchemaId id) const
{
    if (!simfil::SchemaPool::finalized())
        throw std::logic_error("Schema query requires a finalized graph.");
    return simfil::SchemaPool::domain(id);
}
std::shared_ptr<simfil::SchemaBinding const> MapSchema::binding() const
{
    if (!finalized())
        throw std::logic_error("Schema binding requires a finalized graph.");
    return simfil::SchemaPool::binding(std::string(schemaId()));
}
simfil::ModelNode::Ptr MapSchema::domainNode(SchemaId id) const
{
    if (!domain(id))
        return {};
    return simfil::SchemaPool::domainNode(id);
}
std::vector<std::string> MapSchema::checkForErrors() const
{
    std::vector<std::string> errors;
    try {
        schema_->validate();
        schema_->validatePool();
    }
    catch (std::exception const& e) {
        errors.push_back(e.what());
    }
    return errors;
}
tl::expected<void, simfil::Error>
MapSchema::resolve(simfil::ModelNode const& node, ResolveFn const& callback) const
{
    if (node.addr().column() == Impl::Layers) {
        if (node.addr().index() >= schema_->layers.size())
            return tl::unexpected(simfil::Error{
                simfil::Error::IndexOutOfRange,
                "Unknown LayerSchema address."});
        auto view = LayerSchema::Ptr::make(shared_from_this(), node.addr());
        callback(*view);
        return {};
    }
    return ModelPool::resolve(node, callback);
}
LayerSchema::Ptr
resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const& pool, simfil::ModelNode const& node)
{
    if (node.addr().column() != MapSchema::Impl::Layers ||
        node.addr().index() >= pool.schema_->layers.size())
        throw std::invalid_argument("Cannot resolve node as a LayerSchema.");
    return LayerSchema::Ptr::make(pool.shared_from_this(), node.addr());
}

schema_detail::SchemaBuilder MapSchema::Impl::exportBuilder() const
{
    schema_detail::SchemaBuilder result;
    result.schemas_.resize(domains.size());
    result.entriesById_.resize(domains.size());
    auto scalarJson = [](simfil::Schema::NumericValue const& value)
    {
        return std::visit([](auto v) { return nlohmann::json(v); }, value);
    };
    for (size_t i = 1; i < domains.size(); ++i) {
        auto const& row = domains[i];
        auto const& view = *pool.domain(i);
        auto& out = result.schemas_[i];
        out.id_ = SchemaId(i);
        out.kind_ = view.kind();
        out.entry_ = {
            SchemaId(i),
            std::string(text(row.key)),
            std::string(text(row.pointer)),
            std::string(text(row.metaType))};
        result.entriesById_[i] = out.entry_;
        out.typeName_ = view.typeName();
        out.zserioType_ = text(row.zserioType);
        out.attributeType_ = text(row.attributeType);
        out.attributeTypeCode_ = text(row.attributeCode);
        out.queryRoot_ = row.flags & 8;
        out.open_ = view.open();
        out.nullable_ = view.nullable();
        out.composition_ = view.composition();
        for (auto alternative : view.alternatives())
            out.alternatives_.push_back(alternative);
        view.forEachElementSchema([&](SchemaId child) { out.elementSchemas_.push_back(child); });
        view.forEachDirectField(
            [&](StringId field, auto children)
            {
                auto n = std::string(name(field));
                out.directFields_.push_back(n);
                out.childSchemas_[n] = {children.begin(), children.end()};
                // The generic pool represents an unknown declaration as a zero
                // edge; legacy JSON uses unknownEdge rather than a dangling ref 0.
                if (children.size() == 1 && children.front() == simfil::NoSchemaId)
                    out.childSchemas_[n].clear();
                if (pool.fieldMultimap(SchemaId(i), field))
                    out.multimapFields_.insert(n);
                if (auto required = view.fieldRequired(field))
                    out.required_[n] = *required;
                if (view.fieldRole(field) == simfil::Schema::FieldRole::Context)
                    out.contextFields_.insert(n);
            });
        for (auto symbol : view.directEnumSymbols())
            out.directEnumSymbols_.push_back(std::string(name(symbol)));
        out.enumValues_ = {view.enumValues().begin(), view.enumValues().end()};
        out.attributeOwners_ = attributeOwners(SchemaId(i));
        out.annotations_ = annotation(row.annotations);
        if (auto numeric = view.numericDomain()) {
            auto& json = out.annotations_["x-mapget"]["numericDomain"];
            json = {
                {"encoding", numeric->encoding},
                {"unit", numeric->unit},
                {"scale", numeric->scale},
                {"offset", numeric->offset},
                {"complete", numeric->complete}};
            if (numeric->minimum)
                json["minimum"] = scalarJson(*numeric->minimum);
            if (numeric->maximum)
                json["maximum"] = scalarJson(*numeric->maximum);
            for (auto const& range : numeric->ordinaryRanges)
                json["ordinaryRanges"].push_back(
                    {{"minimum", scalarJson(range.min)}, {"maximum", scalarJson(range.max)}});
        }
        view.forEachNamedValue(
            [&](auto const& value)
            {
                static constexpr std::string_view roles[]{"constant", "special", "flag"};
                static constexpr std::string_view
                    meanings[]{"ordinary", "unknown", "variable", "unbounded", "saturated"};
                out.annotations_["x-mapget"]["namedValues"].push_back(
                    {{"name", value.name},
                     {"value", std::visit([](auto v) { return nlohmann::json(v); }, value.value)},
                     {"role", roles[unsigned(value.role)]},
                     {"meaning", meanings[unsigned(value.meaning)]},
                     {"deprecated", value.deprecated},
                     {"removed", value.removed}});
            });
        if (view.isBitmask())
            out.annotations_["x-mapget"]["bitWidth"] = view.bitWidth();
    }
    for (auto const& [key, id] : globalAliases)
        result.idsByKey_.emplace(key, id);
    return result;
}

nlohmann::json LayerSchema::toJsonSchema() const
{
    auto const& graph = *mapSchema()->schema_;
    auto compiled = graph.builder ? *graph.builder : graph.exportBuilder();
    auto const& layer = graph.layerAt(addr_.index());
    if (graph.builder)
        for (auto const& [key, id] : layer.buildAliases)
            compiled.idsByKey_[key] = id;
    else
        for (auto const& [key, id] : layer.aliases)
            compiled.idsByKey_[std::string(key)] = id;
    auto json = compiled.exportDomains();
    json.erase("anyOf");
    if (layer.root)
        json["$ref"] = "#/definitions/" + std::to_string(layer.root);
    else
        for (auto root : graph.producedRoots(layer))
            json["anyOf"].push_back({{"$ref", "#/definitions/" + std::to_string(root)}});
    return json;
}

LayerSchema::Entry const* LayerSchema::getSchema(std::string_view key) const
{
    auto id = schemaId(key);
    if (!id)
        id = featureSchema(key);
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder)
        throw std::logic_error(
            "getSchema() requires finalization; use schemaId() during construction.");
    return id && id < graph.entries.size() ? &graph.entries[id] : nullptr;
}
bool LayerSchema::hasSchema(SchemaId id) const
{
    return mapSchema()->schema_->valid(id);
}
Kind LayerSchema::kind(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder)
        return graph.builder->kind(id);
    return graph.valid(id) ? graph.pool.domain(id)->kind() : Kind::Unknown;
}
std::string_view LayerSchema::typeName(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        std::string_view(graph.builder->schemas_[id].typeName_) :
        graph.pool.domain(id)->typeName();
}
Composition LayerSchema::composition(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return Composition::None;
    return graph.builder ?
        graph.builder->schemas_[id].composition_ :
        graph.pool.domain(id)->composition();
}
std::span<SchemaId const> LayerSchema::alternatives(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        std::span<SchemaId const>(graph.builder->schemas_[id].alternatives_) :
        graph.pool.domain(id)->alternatives();
}
std::span<simfil::ScalarValueType const> LayerSchema::enumValues(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        std::span<simfil::ScalarValueType const>(graph.builder->schemas_[id].enumValues_) :
        graph.pool.domain(id)->enumValues();
}
bool LayerSchema::open(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return true;
    return graph.builder ? graph.builder->schemas_[id].open_ : graph.pool.domain(id)->open();
}
std::optional<bool> LayerSchema::nullable(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    return graph.builder ?
        graph.builder->schemas_[id].nullable_ :
        graph.pool.domain(id)->nullable();
}
std::optional<bool> LayerSchema::fieldRequired(SchemaId id, std::string_view field) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return {};
    if (graph.builder) {
        auto const& required = graph.builder->schemas_[id].required_;
        if (auto it = required.find(field); it != required.end())
            return it->second;
        return {};
    }
    return graph.pool.domain(id)->fieldRequired(graph.pool.strings()->get(field));
}
bool LayerSchema::reachabilityComplete(SchemaId id) const
{
    auto const& graph = *mapSchema()->schema_;
    return !graph.builder && graph.valid(id) && graph.pool.domain(id)->reachabilityComplete();
}

simfil::Schema::FieldRole LayerSchema::fieldRole(SchemaId id, std::string_view field) const
{
    auto const& graph = *mapSchema()->schema_;
    if (!graph.valid(id))
        return simfil::Schema::FieldRole::Data;
    if (graph.builder)
        return graph.builder->schemas_[id].contextFields_.contains(field) ?
            simfil::Schema::FieldRole::Context :
            simfil::Schema::FieldRole::Data;
    return graph.pool.domain(id)->fieldRole(graph.pool.strings()->get(field));
}
bool LayerSchema::canHaveField(SchemaId id, std::string_view field) const
{
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder || !graph.valid(id))
        return true;
    return graph.pool.domain(id)->canHaveField(graph.pool.strings()->get(field));
}
bool LayerSchema::canHaveEnumSymbol(SchemaId id, std::string_view symbol) const
{
    auto const& graph = *mapSchema()->schema_;
    if (graph.builder || !graph.valid(id))
        return false;
    return graph.pool.domain(id)->canHaveEnumSymbol(graph.pool.strings()->get(symbol));
}
std::span<std::string_view const> LayerSchema::directFields(SchemaId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ? g.pool.fieldNames(id) : std::span<std::string_view const>{};
}
std::span<std::string_view const> LayerSchema::nestedFields(SchemaId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ?
        g.pool.fieldNames(id, true) :
        std::span<std::string_view const>{};
}
std::span<std::string_view const> LayerSchema::directEnumSymbols(SchemaId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ? g.pool.enumNames(id) : std::span<std::string_view const>{};
}
std::span<std::string_view const> LayerSchema::nestedEnumSymbols(SchemaId id) const
{
    auto const& g = *mapSchema()->schema_;
    return !g.builder && g.valid(id) ?
        g.pool.enumNames(id, true) :
        std::span<std::string_view const>{};
}
std::string_view LayerSchema::attributeTypeCode(SchemaId id) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return {};
    return g.builder ?
        std::string_view(g.builder->schemas_[id].attributeTypeCode_) :
        g.text(g.domains[id].attributeCode);
}
void LayerSchema::forEachDirectField(
    SchemaId id,
    std::function<void(std::string_view, std::span<SchemaId const>)> const& fn) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return;
    if (g.builder)
        g.builder->forEachDirectField(id, fn);
    else
        g.pool.domain(id)
            ->forEachDirectField([&](StringId name, auto children) { fn(g.name(name), children); });
}
void LayerSchema::forEachElementSchema(SchemaId id, std::function<void(SchemaId)> const& fn) const
{
    auto const& g = *mapSchema()->schema_;
    if (!g.valid(id))
        return;
    if (g.builder)
        g.builder->forEachElementSchema(id, fn);
    else
        g.pool.domain(id)->forEachElementSchema(fn);
}
SchemaId LayerSchema::attributeQuerySchema(std::string_view feature, SchemaId attribute) const
{
    auto const& roots = mapSchema()->schema_->queryRoots;
    auto it = roots.find({featureSchema(feature), attribute});
    return it == roots.end() ? 0 : it->second;
}
SchemaId LayerSchema::featureSchema(std::string_view feature) const
{
    return schemaId(featureKey(feature));
}
SchemaId LayerSchema::featurePropertiesSchema(std::string_view feature) const
{
    return schemaId(featurePropertiesKey(feature));
}
SchemaId LayerSchema::attributeLayerMapSchema(std::string_view feature) const
{
    return schemaId(attributeLayerMapKey(feature));
}

std::string_view MapSchema::Impl::metaType(SchemaId id) const
{
    if (!valid(id))
        return {};
    return builder ?
        std::string_view(builder->entriesById_[id].metaType_) :
        text(domains[id].metaType);
}
std::vector<LayerSchema::AttributePathOwner> MapSchema::Impl::attributeOwners(SchemaId id) const
{
    if (!valid(id))
        return {};
    if (builder)
        return builder->schemas_[id].attributeOwners_;
    std::vector<LayerSchema::AttributePathOwner> result;
    for (auto const& owner : domains[id].owners.in(owners))
        result.push_back(
            {std::string(name(owner.feature)),
             std::string(name(owner.layer)),
             std::string(name(owner.name)),
             id});
    return result;
}
SchemaId
MapSchema::Impl::childSchema(SchemaId root, std::string_view field, std::optional<Kind> preferred)
    const
{
    if (builder)
        return builder->childSchema(root, field, preferred);
    std::vector<SchemaId> pending{root};
    std::vector<bool> seen(domains.size());
    SchemaId fallback = 0;
    auto fieldId = pool.strings()->get(field);
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto const& view = *pool.domain(id);
        for (auto alternate : view.alternatives())
            pending.push_back(alternate);
        if (hasAffinity(view.kind(), simfil::ValueType::Array) &&
            view.composition() == Composition::None)
            view.forEachElementSchema([&](SchemaId item) { pending.push_back(item); });
        SchemaId selected = 0;
        view.forEachDirectField(
            [&](StringId name, auto children)
            {
                if (name != view.canonicalField(fieldId))
                    return;
                for (auto child : children) {
                    if (!child)
                        continue;
                    if (!fallback)
                        fallback = child;
                    if (!selected &&
                        (!preferred || matchesKind(pool.domain(child)->kind(), *preferred)))
                        selected = child;
                }
            });
        if (selected || fallback)
            return selected ? selected : fallback;
    }
    return 0;
}
SchemaId
LayerSchema::childSchema(SchemaId root, std::string_view field, std::optional<Kind> preferred) const
{
    return mapSchema()->schema_->childSchema(root, field, preferred);
}

LayerSchema::PathOwner LayerSchema::ownerForPath(
    std::string_view featureType,
    simfil::SchemaId rootSchema,
    std::span<const std::string> fieldPath) const
{
    auto const& graph = *mapSchema()->schema_;
    auto uniqueOwner =
        [&](SchemaId id, std::string_view feature) -> std::optional<AttributePathOwner>
    {
        std::optional<AttributePathOwner> result;
        for (auto const& owner : graph.attributeOwners(id))
            if (owner.featureType_ == feature) {
                if (result)
                    return std::nullopt;
                result = owner;
            }
        return result;
    };
    auto currentSchema = rootSchema;
    if (!graph.valid(currentSchema)) {
        return {};
    }

    auto attributeOwner = uniqueOwner(rootSchema, featureType);
    auto currentLayerName = std::optional<std::string>{};
    auto enteredAttributeBranch = graph.metaType(rootSchema) == "AttributeLayerMap" ||
        graph.metaType(rootSchema) == "AttributeContainer" ||
        graph.metaType(rootSchema) == "Attribute";

    for (auto const& fieldName : fieldPath) {
        if (fieldName.empty()) {
            continue;
        }

        auto const parentSchema = currentSchema;
        auto const parentMetaType = graph.metaType(parentSchema);
        auto pendingAttributeName = std::optional<std::string>{};

        if (parentMetaType == "AttributeLayerMap") {
            // This edge selects the concrete attribute layer, but not yet a
            // concrete attribute within that layer.
            enteredAttributeBranch = true;
            currentLayerName = fieldName;
        }
        else if (parentMetaType == "AttributeContainer") {
            // This edge selects the attribute object inside the active layer.
            enteredAttributeBranch = true;
            pendingAttributeName = fieldName;
        }

        currentSchema = graph.childSchema(currentSchema, fieldName, std::nullopt);
        if (currentSchema == simfil::NoSchemaId) {
            return {};
        }

        auto const currentMetaType = graph.metaType(currentSchema);
        if (currentMetaType == "AttributeLayerMap" || currentMetaType == "AttributeContainer") {
            enteredAttributeBranch = true;
        }
        if (currentMetaType == "Attribute") {
            enteredAttributeBranch = true;
            if (auto owner = uniqueOwner(currentSchema, featureType)) {
                attributeOwner = *owner;
                continue;
            }

            // If the schema id is intentionally shared, the path edge still
            // contains enough context to identify the selected attribute.
            auto attributeName = std::string(attributeTypeCode(currentSchema));
            if (attributeName.empty() && pendingAttributeName) {
                attributeName = *pendingAttributeName;
            }
            if (currentLayerName && !attributeName.empty()) {
                attributeOwner = AttributePathOwner{
                    std::string(featureType),
                    *currentLayerName,
                    std::move(attributeName),
                    currentSchema};
            }
        }
        else if (auto owner = uniqueOwner(currentSchema, featureType)) {
            attributeOwner = *owner;
        }
    }

    if (attributeOwner) {
        return {PathOwnerKind::Attribute, *attributeOwner};
    }

    if (enteredAttributeBranch) {
        return {};
    }

    if (rootSchema == featureSchema(featureType) ||
        rootSchema == featurePropertiesSchema(featureType))
    {
        return {PathOwnerKind::Feature, {}};
    }

    return {};
}

void LayerSchema::setJsonSchemaAnnotations(simfil::SchemaId id, nlohmann::json annotations)
{
    mutableOwner().setJsonSchemaAnnotations(id, std::move(annotations));
}

void LayerSchema::addFieldSchema(
    simfil::SchemaId parent,
    std::string fieldName,
    simfil::SchemaId child,
    bool multimap)
{
    mutableOwner().addFieldSchema(parent, std::move(fieldName), child, multimap);
}

void LayerSchema::addElementSchema(simfil::SchemaId parent, simfil::SchemaId child)
{
    mutableOwner().addElementSchema(parent, child);
}

void LayerSchema::addEnumSymbol(simfil::SchemaId schemaId, std::string symbolName)
{
    mutableOwner().addEnumSymbol(schemaId, std::move(symbolName));
}

void LayerSchema::addEnumSymbols(
    simfil::SchemaId schemaId,
    std::span<const std::string> symbolNames)
{
    mutableOwner().addEnumSymbols(schemaId, symbolNames);
}

void LayerSchema::addEnumValue(simfil::SchemaId schemaId, simfil::ScalarValueType value)
{
    mutableOwner().addEnumValue(schemaId, std::move(value));
}

void LayerSchema::setComposition(simfil::SchemaId schemaId, Composition composition)
{
    mutableOwner().setComposition(schemaId, composition);
}

void LayerSchema::addAlternative(simfil::SchemaId schemaId, simfil::SchemaId alternative)
{
    mutableOwner().addAlternative(schemaId, alternative);
}

void LayerSchema::setTypeName(simfil::SchemaId schemaId, std::string name)
{
    mutableOwner().setTypeName(schemaId, std::move(name));
}

void LayerSchema::setOpen(simfil::SchemaId schemaId, bool open)
{
    mutableOwner().setOpen(schemaId, open);
}

void LayerSchema::setNullable(simfil::SchemaId schemaId, std::optional<bool> nullable)
{
    mutableOwner().setNullable(schemaId, nullable);
}

void LayerSchema::setFieldRequired(
    simfil::SchemaId schemaId,
    std::string field,
    std::optional<bool> required)
{
    mutableOwner().setFieldRequired(schemaId, std::move(field), required);
}

void LayerSchema::setZserioType(simfil::SchemaId schemaId, std::string zserioType)
{
    mutableOwner().setZserioType(schemaId, std::move(zserioType));
}

void LayerSchema::setFieldRole(
    simfil::SchemaId schemaId,
    std::string field,
    simfil::Schema::FieldRole role)
{
    mutableOwner().setFieldRole(schemaId, std::move(field), role);
}

void LayerSchema::setAttributeMetadata(
    simfil::SchemaId schemaId,
    AttributePathOwner owner,
    std::string attributeType,
    std::string zserioType)
{
    mutableOwner().setAttributeMetadata(
        schemaId,
        std::move(owner),
        std::move(attributeType),
        std::move(zserioType));
}

std::string LayerSchema::featureKey(std::string_view featureType)
{
    return "Feature:" + std::string(featureType);
}

std::string LayerSchema::featurePropertiesKey(std::string_view featureType)
{
    return "FeatureProperties:" + std::string(featureType);
}

std::string LayerSchema::attributeLayerMapKey(std::string_view featureType)
{
    return "AttributeLayerMap:" + std::string(featureType);
}

std::string LayerSchema::attributeContainerKey(
    std::string_view featureType,
    std::string_view attributeLayerName)
{
    return "AttributeContainer:" + std::string(featureType) + ":" + std::string(attributeLayerName);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode)
{
    return "Attribute:" + std::string(featureType) + ":" + std::string(attributeLayerName) + ":" +
        std::string(attributeTypeCode);
}

std::string LayerSchema::attributeKey(
    std::string_view featureType,
    std::string_view attributeLayerName,
    std::string_view attributeTypeCode,
    std::string_view sourceDefinitionKey)
{
    std::string result = "AttributeSource:";
    for (auto part : {featureType, attributeLayerName, attributeTypeCode, sourceDefinitionKey}) {
        if (part.empty())
            throw std::invalid_argument("Qualified attribute keys require all four context parts.");
        result += std::to_string(part.size());
        result += ':';
        result += part;
    }
    return result;
}

std::vector<LayerSchema::NamedSchemaPath>
MapSchema::Impl::scalarPaths(SchemaId root, std::string_view attribute) const
{
    if (builder)
        return builder->scalarFieldPathsForAttribute(root, attribute);
    if (!valid(root) ||
        std::ranges::none_of(
            domains,
            [&](auto const& row) { return text(row.attributeCode) == attribute; }))
        return {};

    // Probe each node once before enumerating paths. Bare query operands often
    // are not attribute codes; a shared DAG must not expand exponentially merely
    // to prove that a code (or a selected payload scalar) is absent.
    constexpr uint8_t hasAttribute = 1, hasScalar = 2;
    std::vector<std::vector<SchemaId>> parents(domains.size());
    std::vector<bool> seen(domains.size());
    std::vector<uint8_t> reachable(domains.size());
    std::vector<SchemaId> pending{root};
    seen[root] = true;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        auto const& row = domains[id];
        if (text(row.attributeCode) == attribute)
            reachable[id] |= hasAttribute;
        auto const& domain = *pool.domain(id);
        if (matchesKind(domain.kind(), Kind::Value))
            reachable[id] |= hasScalar;
        auto addChild = [&](SchemaId child)
        {
            if (!valid(child))
                return;
            parents[child].push_back(id);
            if (!seen[child]) {
                seen[child] = true;
                pending.push_back(child);
            }
        };
        domain.forEachEnumerableField(
            [&](auto, auto children)
            {
                for (auto child : children)
                    addChild(child);
            });
        domain.forEachElementSchema(addChild);
        for (auto child : domain.alternatives())
            addChild(child);
    }
    for (size_t id = 1; id < domains.size(); ++id)
        if (reachable[id])
            pending.push_back(SchemaId(id));
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        for (auto parent : parents[id]) {
            auto added = reachable[id] & ~reachable[parent];
            if (added) {
                reachable[parent] |= added;
                pending.push_back(parent);
            }
        }
    }

    std::vector<LayerSchema::NamedSchemaPath> result;
    LayerSchema::NamedSchemaPath path;
    std::vector<bool> active(domains.size());
    size_t work = 0;
    // An attribute shorthand chooses its first payload scalar, not every scalar
    // in the record. Keep this precedence identical to the transient compiler.
    std::function<bool(SchemaId, bool, bool)> visit = [&](SchemaId id, bool selected, bool first)
    {
        if (!valid(id) || active[id])
            return false;
        auto const& row = domains[id];
        auto const& view = *pool.domain(id);
        auto matches = text(row.attributeCode) == attribute;
        selected = selected || matches;
        // Reachability deliberately includes data metadata edges: only the original
        // path-local walk below decides exclusions and first-payload precedence.
        if (!(reachable[id] & hasScalar) || (!selected && !(reachable[id] & hasAttribute)))
            return false;
        if (++work > 100000 || path.size() > 256)
            throw std::length_error("Schema path traversal budget exceeded.");
        if (selected && matchesKind(view.kind(), Kind::Value)) {
            result.push_back(path);
            return true;
        }
        active[id] = true;
        bool found = false;
        for (auto child : view.alternatives()) {
            found = visit(child, selected, first || matches) || found;
            if (selected && found)
                break;
        }
        view.forEachEnumerableField(
            [&](StringId field, auto children)
            {
                if (selected && found)
                    return;
                auto name = this->name(field);
                if ((first || matches) &&
                    schema_detail::isAttributeScalarShorthandMetadataField(name))
                    return;
                path.push_back({simfil::SchemaPathSegment::Kind::Field, std::string(name)});
                for (auto child : children) {
                    found = visit(child, selected, false) || found;
                    if (selected && found)
                        break;
                }
                path.pop_back();
            });
        view.forEachElementSchema(
            [&](SchemaId child)
            {
                if (selected && found)
                    return;
                path.push_back({simfil::SchemaPathSegment::Kind::ArrayElement, {}});
                found = visit(child, selected, false) || found;
                path.pop_back();
            });
        active[id] = false;
        return found;
    };
    visit(root, false, false);
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::vector<LayerSchema::NamedSchemaPath>
LayerSchema::scalarFieldPathsForAttribute(SchemaId root, std::string_view attribute) const
{
    if (attribute.empty())
        return {};
    return mapSchema()->schema_->scalarPaths(root, attribute);
}
std::vector<LayerSchema::AttributePathOwner> LayerSchema::attributeScopes() const
{
    auto const& g = *mapSchema()->schema_;
    std::vector<AttributePathOwner> result;
    std::vector<SchemaId> pending;
    for (auto const& feature : featureTypes())
        pending.push_back(featureSchema(feature));
    std::set<SchemaId> seen;
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!g.valid(id) || !seen.insert(id).second)
            continue;
        for (auto const& owner : g.attributeOwners(id))
            if (producesFeatureType(owner.featureType_))
                result.push_back(owner);
        forEachDirectField(
            id,
            [&](auto field, auto children)
            {
                if (fieldRole(id, field) == simfil::Schema::FieldRole::Data)
                    pending.insert(pending.end(), children.begin(), children.end());
            });
        forEachElementSchema(id, [&](auto child) { pending.push_back(child); });
        auto a = alternatives(id);
        pending.insert(pending.end(), a.begin(), a.end());
    }
    auto key = [](auto const& o)
    {
        return std::tie(
            o.featureType_,
            o.attributeLayerName_,
            o.attributeName_,
            o.attributeSchema_);
    };
    std::ranges::sort(result, {}, key);
    result.erase(
        std::unique(
            result.begin(),
            result.end(),
            [&](auto const& a, auto const& b) { return key(a) == key(b); }),
        result.end());
    return result;
}
std::vector<std::string>
LayerSchema::constantTypeNames(SchemaId root, std::string_view symbol) const
{
    auto const& g = *mapSchema()->schema_;
    if (g.builder)
        return g.builder->constantTypeNames(root, symbol);
    std::vector<std::string> result;
    std::vector<SchemaId> pending{root};
    std::vector<bool> seen(g.domains.size());
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (!g.valid(id) || seen[id])
            continue;
        seen[id] = true;
        auto const& d = g.domains[id];
        if (g.text(d.attributeCode) == symbol && d.attributeType)
            result.emplace_back(g.text(d.attributeType));
        auto typeName = g.pool.domain(id)->typeName();
        if (std::ranges::find(directEnumSymbols(id), symbol) != directEnumSymbols(id).end() &&
            !typeName.empty())
            result.emplace_back(typeName);
        g.pool.domain(id)->forEachNamedValue(
            [&](auto const& value)
            {
                if (value.name == symbol && !typeName.empty())
                    result.emplace_back(typeName);
            });
        forEachDirectField(
            id,
            [&](auto field, auto children)
            {
                if (fieldRole(id, field) == simfil::Schema::FieldRole::Data)
                    pending.insert(pending.end(), children.begin(), children.end());
            });
        forEachElementSchema(id, [&](auto child) { pending.push_back(child); });
        auto a = alternatives(id);
        pending.insert(pending.end(), a.begin(), a.end());
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
void LayerSchema::setNumericDomain(SchemaId id, simfil::Schema::NumericDomain const& d)
{
    mutableOwner().setNumericDomain(id, d);
}
void LayerSchema::addNamedValue(SchemaId id, simfil::Schema::NamedValue const& v)
{
    mutableOwner().addNamedValue(id, v);
}
void LayerSchema::setBitWidth(SchemaId id, uint8_t width)
{
    mutableOwner().setBitWidth(id, width);
}
std::optional<simfil::Schema::NumericDomain> LayerSchema::numericDomain(SchemaId id) const
{
    return mapSchema()->numericDomain(id);
}
void LayerSchema::forEachNamedValue(
    SchemaId id,
    std::function<void(simfil::Schema::NamedValue const&)> const& fn) const
{
    mapSchema()->forEachNamedValue(id, fn);
}
uint8_t LayerSchema::bitWidth(SchemaId id) const
{
    return mapSchema()->bitWidth(id);
}

bool LayerSchema::RelationDeclaration::permitsIncoming(
    std::string_view owner,
    std::string_view name,
    std::string_view target) const
{
    return ownerType_ == owner && name_ == name &&
        std::ranges::find(targetTypes_, target) != targetTypes_.end();
}
nlohmann::json
LayerSchema::featureTypeContract(std::string_view name, std::function<void(size_t)> const& reserve)
    const
{
    auto chargeString = [&](std::string_view value)
    {
        if (!reserve)
            return;
        if (value.size() > (std::numeric_limits<size_t>::max() - 2) / 6)
            throw std::length_error("Feature contract string is too large.");
        reserve(2 + value.size() * 6);  // Quotes and worst-case escaping, before allocating JSON.
    };
    auto const& identity = *getTypeInfo(name);
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    if (reserve)
        reserve(256);
    chargeString(identity.name_);
    auto result = nlohmann::json{
        {"name", identity.name_},
        {"uniqueIdCompositions", nlohmann::json::array()},
        {"produced", producesFeatureType(name)},
        {"schemaId", featureSchema(name)},
        {"forwardRelationsKnown", layer.relationsKnown},
        {"forwardRelations", nlohmann::json::array()}};
    for (auto const& composition : identity.uniqueIdCompositions_) {
        if (reserve)
            reserve(8);
        nlohmann::json parts;
        for (auto const& part : composition) {
            if (reserve)
                reserve(128);
            chargeString(part.idPartLabel_);
            chargeString(part.description_);
            parts.push_back(part.toJson());
        }
        result["uniqueIdCompositions"].push_back(std::move(parts));
    }
    // Read the frozen relation index directly. A layer-wide vector copy would allocate
    // unrelated declarations before the caller can stop a focused inspection.
    for (auto const& declaration : layer.relations) {
        if (reserve)
            reserve(0);
        if (declaration.ownerType_ != name)
            continue;
        if (reserve)
            reserve(128);
        chargeString(declaration.name_);
        auto targets = nlohmann::json::array();
        for (auto const& target : declaration.targetTypes_) {
            chargeString(target);
            targets.push_back(target);
        }
        auto locality = declaration.locality_ == RelationDeclaration::Locality::SameContainer ?
            "same-container" :
            declaration.locality_ == RelationDeclaration::Locality::MayCrossContainer ?
            "may-cross-container" :
            "unknown";
        result["forwardRelations"].push_back(
            {{"name", declaration.name_},
             {"targetTypes", std::move(targets)},
             {"complete", declaration.complete_},
             {"locality", locality}});
    }
    return result;
}

std::vector<LayerSchema::RelationDeclaration> LayerSchema::relationDeclarations() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).relations;
}
std::vector<LayerSchema::RelationDeclaration>
LayerSchema::relationsTo(std::string_view target) const
{
    auto const& layer = mapSchema()->schema_->layerAt(addr_.index());
    std::vector<RelationDeclaration> result;
    if (auto found = layer.relationsByTarget.find(target); found != layer.relationsByTarget.end())
        for (auto i : found->second)
            result.push_back(layer.relations[i]);
    return result;
}
bool LayerSchema::hasRelationDeclarations() const
{
    return mapSchema()->schema_->layerAt(addr_.index()).relationsKnown;
}

/** A layer remains a small native object even without feature roots (e.g. an
 * open SourceData decoder). Domain descriptors share the graph through SchemaModel.
 */
simfil::ValueType LayerSchema::type() const
{
    return simfil::ValueType::Object;
}
uint32_t LayerSchema::size() const
{
    return 3;
}
StringId LayerSchema::keyAt(int64_t i) const
{
    if (i < 0 || i >= size())
        return 0;
    return mapSchema()->schema_->descriptorKeys[size_t(i)];
}
simfil::ModelNode::Ptr LayerSchema::at(int64_t i) const
{
    if (i == 0)
        return simfil::model_ptr<simfil::ValueNode>::make(simfil::ScalarValueType{name()}, model_);
    if (i == 1)
        return simfil::model_ptr<simfil::ValueNode>::make(
            simfil::ScalarValueType{int64_t(layerType())},
            model_);
    if (i == 2)
        return rootSchema() ? mapSchema()->domainNode(rootSchema()) : simfil::ModelNode::Ptr{};
    return {};
}
simfil::ModelNode::Ptr LayerSchema::get(StringId const& field) const
{
    for (size_t i = 0; i < size(); ++i)
        if (keyAt(i) == field)
            return at(i);
    return {};
}
bool LayerSchema::iterate(IterCallback const& callback) const
{
    for (size_t i = 0; i < size(); ++i)
        if (auto node = at(i); node && !callback(*node))
            return false;
    return true;
}

MemoryUsageBreakdown MapSchema::memoryUsage() const
{
    if (schema_->memorySummary)
        return *schema_->memorySummary;
    MemoryUsageBreakdown result;
    result.add("identity", stringMemoryUsage(schema_->identity));
    result.add(
        "format",
        stringMemoryUsage(schema_->format.name_) + stringMemoryUsage(schema_->format.version_));
    result.add(
        "subsystems",
        {schema_->format.subsystems_.size() *
             sizeof(decltype(schema_->format.subsystems_)::value_type),
         schema_->format.subsystems_.size() *
             (sizeof(decltype(schema_->format.subsystems_)::value_type) + 3 * sizeof(void*))});
    for (auto const& [name, version] : schema_->format.subsystems_)
        result.add("subsystem-text", stringMemoryUsage(name) + stringMemoryUsage(version));
    result.add(
        "query-root-index",
        {schema_->queryRoots.size() * sizeof(decltype(schema_->queryRoots)::value_type),
         schema_->queryRoots.size() *
             (sizeof(decltype(schema_->queryRoots)::value_type) + 3 * sizeof(void*))});
    result.add("object", {sizeof(*this) + sizeof(Impl), sizeof(*this) + sizeof(Impl)});
    result.add("model-pool", memoryUsageStats().total());
    result.add("dictionary", strings()->memoryUsage());
    result.add("domain-column", schema_->domains.memory_usage());
    result.add("attribute-owners", vectorMemoryUsage(schema_->owners));
    result.add("aliases", vectorMemoryUsage(schema_->aliases));
    result.add("entries", vectorMemoryUsage(schema_->entries));
    result.add("layers", vectorMemoryUsage(schema_->layers));
    result.add("generic-schema", schemaMemoryUsage());
    result.add("constituents", vectorMemoryUsage(schema_->constituents));
    for (auto const& [identity, format] : schema_->constituents) {
        result.add(
            "constituent-text",
            stringMemoryUsage(identity) + stringMemoryUsage(format.name_) +
                stringMemoryUsage(format.version_));
        result.add(
            "constituent-subsystems",
            {format.subsystems_.size() * sizeof(decltype(format.subsystems_)::value_type),
             format.subsystems_.size() *
                 (sizeof(decltype(format.subsystems_)::value_type) + 3 * sizeof(void*))});
        for (auto const& [name, version] : format.subsystems_)
            result.add(
                "constituent-subsystem-text",
                stringMemoryUsage(name) + stringMemoryUsage(version));
    }
    result.add(
        "contributor-remaps",
        {schema_->contributorRemaps.size() *
             sizeof(decltype(schema_->contributorRemaps)::value_type),
         schema_->contributorRemaps.size() *
             (sizeof(decltype(schema_->contributorRemaps)::value_type) + 3 * sizeof(void*))});
    for (auto const& [identity, remap] : schema_->contributorRemaps)
        result
            .add("contributor-remap-data", stringMemoryUsage(identity) + vectorMemoryUsage(remap));
    for (auto const& layer : schema_->layers) {
        result.add("layer-names", stringMemoryUsage(layer.name));
        result.add("import-fingerprint", stringMemoryUsage(layer.importedFingerprint));
        result.add("relations", vectorMemoryUsage(layer.relations));
        for (auto const& relation : layer.relations)
            result.add(
                "relation-text",
                stringMemoryUsage(relation.ownerType_) + stringMemoryUsage(relation.name_) +
                    stringVectorMemoryUsage(relation.targetTypes_));
        result.add(
            "relation-target-index",
            {layer.relationsByTarget.size() * sizeof(decltype(layer.relationsByTarget)::value_type),
             layer.relationsByTarget.size() *
                 (sizeof(decltype(layer.relationsByTarget)::value_type) + 3 * sizeof(void*))});
        for (auto const& [target, indices] : layer.relationsByTarget)
            result.add(
                "relation-target-data",
                stringMemoryUsage(target) + vectorMemoryUsage(indices));
        result.add("feature-definitions", vectorMemoryUsage(layer.features));
        result.add("feature-roots", vectorMemoryUsage(layer.featureRoots));
        result.add("feature-production", vectorMemoryUsage(layer.produced));
        for (auto const& feature : layer.features) {
            result.add("feature-name", stringMemoryUsage(feature.name_));
            result.add("id-compositions", vectorMemoryUsage(feature.uniqueIdCompositions_));
            for (auto const& composition : feature.uniqueIdCompositions_) {
                result.add("id-parts", vectorMemoryUsage(composition));
                for (auto const& part : composition) {
                    result.add("id-part-names", stringMemoryUsage(part.idPartLabel_));
                    result.add("id-part-descriptions", stringMemoryUsage(part.description_));
                }
            }
        }
        result.add(
            "layer-alias-index",
            {layer.aliases.size() * sizeof(decltype(layer.aliases)::value_type),
             layer.aliases.size() *
                 (sizeof(decltype(layer.aliases)::value_type) + 3 * sizeof(void*))});
    }
    result.add(
        "global-alias-index",
        {schema_->globalAliases.size() * sizeof(decltype(schema_->globalAliases)::value_type),
         schema_->globalAliases.size() *
             (sizeof(decltype(schema_->globalAliases)::value_type) + 3 * sizeof(void*))});
    return result;
}

void MapSchema::Impl::summarize()
{
    auto counts = pool.schemaStatistics();
    size_t featureTypes = 0, relations = 0;
    for (auto const& layer : layers) {
        featureTypes += layer.features.size();
        relations += layer.relations.size();
    }
    memorySummary = pool.memoryUsage();
    statistics = {
        {"counts",
         {{"domains", domains.empty() ? 0 : domains.size() - 1},
          {"fields", counts.fields},
          {"fieldEdges", counts.fieldEdges},
          {"contextFields", counts.contextFields},
          {"elementEdges", counts.elementEdges},
          {"alternativeEdges", counts.alternativeEdges},
          {"layers", layers.size()},
          {"featureTypes", featureTypes},
          {"relationDeclarations", relations},
          {"internedNames", pool.strings()->size()}}},
        {"limits",
         {{"domainId", simfil::MaxSchemaId}, {"nameId", std::numeric_limits<StringId>::max()}}},
        {"memory", memorySummary->toJson()},
        {"constituents", pool.constituents()}};
}

nlohmann::json MapSchema::constituents() const
{
    auto result = nlohmann::json::array();
    for (auto const& [identity, format] : schema_->constituents)
        result.push_back({{"schemaId", identity}, {"mapFormat", format.toJson()}});
    return result;
}

nlohmann::json MapSchema::statistics() const
{
    if (!finalized())
        throw std::logic_error("MapSchema statistics require a finalized graph.");
    return schema_->statistics;
}

std::span<SchemaId const> MapSchema::schemaRemap(std::string_view sourceIdentity) const
{
    if (!finalized())
        throw std::logic_error("Schema translations require a finalized artifact.");
    auto found = schema_->contributorRemaps.find(sourceIdentity);
    if (found == schema_->contributorRemaps.end())
        throw std::invalid_argument(
            "No construction-time translation for the source schema artifact.");
    return found->second;
}

MapSchema::ConstPtr MapSchema::compose(MapFormat format, std::span<ConstPtr const> inputs)
{
    format = MapFormat::fromJson(format.toJson());
    if (inputs.empty() || inputs.size() > 256)
        throw std::invalid_argument("Schema composition requires 1..256 immutable constituents.");
    std::vector<ConstPtr> owners;
    std::set<std::string_view> identities;
    for (auto const& owner : inputs) {
        if (!owner || !owner->finalized())
            throw std::invalid_argument("Schema composition requires finalized input owners.");
        if (identities.insert(owner->schemaId()).second)
            owners.push_back(owner);
    }
    if (owners.size() == 1 && owners.front()->format() == format)
        return owners.front();
    auto key = "compose-v1:" + format.toJson().dump();
    for (auto const& owner : owners)
        key += ":" + std::string(owner->schemaId());
    // Do not share this cache key with the native reader: decoded artifacts have
    // the same content identity but deliberately no producer translation tables.
    return getOrBuild(
        std::move(key),
        [format = std::move(format), owners = std::move(owners)]
        {
            auto result = std::make_shared<MapSchema>(format);
            Impl::CompositionBuilder(*result->schema_, owners).build();
            result->finalize();
            return result;
        });
}

SchemaId MapSchema::remapSchema(std::string_view sourceIdentity, SchemaId id) const
{
    if (sourceIdentity == schemaId()) {
        if (id && !schema_->valid(id))
            throw std::invalid_argument("Source domain is absent from the effective artifact.");
        return id;
    }
    auto remap = schemaRemap(sourceIdentity);
    if (id >= remap.size() || (id && !remap[id]))
        throw std::invalid_argument("Source domain is absent from the contributor translation.");
    return remap[id];
}
MemoryUsageBreakdown LayerSchema::memoryUsage() const
{
    // This is a borrowing view. Unique-owner accounting belongs to MapSchema.
    MemoryUsageBreakdown result;
    result.add("view", {sizeof(*this), sizeof(*this)});
    return result;
}

}  // namespace mapget
