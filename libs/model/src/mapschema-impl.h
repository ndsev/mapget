#pragma once

#include "mapget/model/mapschema.h"
#include "mapschema-builder.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <unordered_map>

namespace mapget
{
using schema_detail::Composition;
using schema_detail::Kind;
using simfil::ModelNodeAddress;
using simfil::StringId;
using simfil::TypeId;

/** Canonical graph columns and their borrowed query indexes. The compiler is
 * transient; neither JSON trees nor flattened owning strings survive freeze.
 */
struct MapSchema::Impl
{
    static constexpr uint8_t Types = simfil::ModelPool::FirstCustomColumnId;
    static constexpr uint8_t Layers = Types + 1;
    static constexpr uint8_t FieldMaps = Types + 2;
    static constexpr uint8_t TypeLists = Types + 3;
    static constexpr uint32_t CodecVersion = 3;
    static constexpr size_t MaxEdges = 4 * 1024 * 1024;

    /** A slice into an owner-wide canonical array, never a per-type allocation. */
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
    /** Map-specific metadata indexed by the generic pool's type identity. */
    struct TypeData
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
        StringId featureLayer = 0;
        template <class S>
        void serialize(S& s)
        {
            s.value2b(feature);
            s.value2b(layer);
            s.value2b(name);
            s.value2b(featureLayer);
        }
    };
    struct AliasData
    {
        MODEL_COLUMN_TYPE(8);
        ModelNodeAddress key;
        TypeId id = 0;
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
        TypeId root = 0;
        std::vector<FeatureTypeInfo> features;
        std::vector<TypeId> featureRoots;
        std::vector<uint8_t> produced;
        std::map<std::string, TypeId, std::less<>> buildAliases;
        std::map<std::string_view, TypeId, std::less<>> aliases;
        bool relationsKnown = false;
        bool relationsDeclared = false;  // Native declaration or decoded canonical inventory.
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
    std::map<std::string, std::vector<TypeId>, std::less<>> contributorRemaps;
    class CompositionBuilder;
    std::unique_ptr<schema_detail::SchemaBuilder> builder =
        std::make_unique<schema_detail::SchemaBuilder>();
    std::vector<Layer> layers;
    simfil::ModelColumn<TypeData> types;
    std::vector<OwnerData> owners;
    std::vector<AliasData> aliases;
    std::map<std::string_view, TypeId, std::less<>> globalAliases;
    std::map<std::pair<TypeId, TypeId>, TypeId> queryRoots;
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
    bool valid(TypeId id) const { return id && (builder ? builder->valid(id) : id < types.size()); }
    void requireType(TypeId id) const
    {
        if (!valid(id))
            throw std::invalid_argument("MapSchema references an unknown type.");
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
        TypeId,
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
    TypeId import(schema_detail::SchemaBuilder const&, size_t layerIndex, TypeId root);
    std::vector<LayerSchema::AttributePathOwner> attributeOwners(TypeId id) const;
    std::string_view metaType(TypeId id) const;
    TypeId childSchema(TypeId root, std::string_view field, std::optional<Kind> kind) const;
    std::vector<LayerSchema::NamedSchemaPath>
    scalarPaths(TypeId root, std::string_view attribute) const;
    std::vector<TypeId> producedRoots(Layer const& layer) const;
};

}  // namespace mapget
