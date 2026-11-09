#pragma once

#include "mapget/model/mapschema.h"
#include "mapschema-json.h"
#include "simfil/model/schema-type.h"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <set>
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
    /** Only actual model-node views receive custom address columns. */
    enum Column : uint8_t {
        Layers = simfil::ModelPool::FirstCustomColumnId,
        AttributeOwner,
        AttributeOwners
    };
    static constexpr uint32_t CodecVersion = 7;
    static constexpr size_t MaxEdges = 4 * 1024 * 1024;
    static constexpr size_t GlobalLayer = std::numeric_limits<size_t>::max();
    static constexpr uint32_t QueryRootFlag = 8;

    /** Map-specific metadata indexed by the generic pool's type identity. */
    struct TypeData
    {
        MODEL_COLUMN_TYPE(36);
        uint32_t flags = 0;
        ModelNodeAddress key, pointer, metaType, zserioType, attributeType, attributeCode,
            annotations;
        simfil::ArrayIndex owners = 0;
    };

    /** One map-wide identity; composition order is canonical, independent of production. */
    struct IdentityData
    {
        MODEL_COLUMN_TYPE(8);
        StringId name = 0;
        uint16_t reserved = 0;
        simfil::ArrayIndex compositions = 0;
    };

    /** One immutable ID part; prose stays in the owner's value buffer, not the name dictionary. */
    struct IdentityPartData
    {
        MODEL_COLUMN_TYPE(8);
        StringId name = 0;
        uint8_t datatype = 0, flags = 0;
        ModelNodeAddress description;
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

    /** One generated query context, indexed by its physical feature/attribute pair. */
    struct QueryData
    {
        MODEL_COLUMN_TYPE(8);
        TypeId feature = 0, attribute = 0, root = 0;
        uint16_t reserved = 0;
    };

    /** Authoritative layer identity and memberships, never an owning DTO tree. */
    struct LayerData
    {
        MODEL_COLUMN_TYPE(16);
        StringId name = 0;
        TypeId root = 0;
        uint8_t type = 0, flags = 0;
        uint16_t reserved = 0;
        simfil::ArrayIndex produced = 0, relations = 0;
    };

    /** A layer's produced name is distinct from the map-wide identity inventory. */
    struct ProducedRootData
    {
        MODEL_COLUMN_TYPE(4);
        StringId name = 0;
        TypeId root = 0;
    };

    /** Stable row identity for one layer's forward relation declaration. */
    struct RelationData
    {
        MODEL_COLUMN_TYPE(12);
        StringId owner = 0, name = 0;
        simfil::ArrayIndex targets = 0;
        uint16_t layer = 0;
        uint8_t flags = 0, locality = 0;
    };

    /** Derived incoming lookup; the canonical declaration remains in relationRows. */
    struct RelationTargetData
    {
        MODEL_COLUMN_TYPE(8);
        StringId target = 0;
        uint16_t layer = 0;
        uint32_t relation = 0;
    };

    /** Construction-only aliases and legacy import idempotency, discarded before publication. */
    struct LayerBuildData
    {
        std::map<std::string, TypeId, std::less<>> aliases;
        std::string fingerprint;
    };
    static constexpr uint8_t RelationsKnownFlag = 1, RelationsDeclaredFlag = 2;

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

    explicit Impl(MapSchema& owner, MapFormat f)
        : pool(owner), dictionary(*owner.strings()), format(std::move(f))
    {
    }
    MapSchema& pool;
    simfil::StringPool& dictionary;  // Borrowed from the base owner for this entire lifetime.
    MapFormat format;
    bool frozen = false;
    bool indexed = false;
    std::optional<MemoryUsageBreakdown> memorySummary;
    nlohmann::json statistics;
    std::vector<std::pair<std::string, MapFormat>> constituents;
    std::map<std::string, std::vector<TypeId>, std::less<>> contributorRemaps;
    class CompositionBuilder;
    class JsonCompiler;
    class OwnerDescriptor;
    simfil::ModelColumn<LayerData> layers;
    std::vector<LayerBuildData> layerBuild;
    simfil::ArrayArena<ProducedRootData, 256, 256> producedFeatures;
    simfil::ModelColumn<RelationData> relationRows;
    simfil::ArrayArena<uint32_t, 256, 256> relationLists;
    simfil::ArrayArena<StringId, 256, 256> relationTargets;
    simfil::ModelColumn<RelationTargetData> relationTargetIndex;
    simfil::ModelColumn<IdentityData> identities;
    simfil::ArrayArena<simfil::ArrayIndex, 256, 256> identityCompositions;
    simfil::ArrayArena<IdentityPartData, 256, 256> identityParts;
    simfil::ModelColumn<uint16_t> identityIndex;  // derived, rebuilt once before publication

    simfil::ModelColumn<TypeData> types;
    simfil::ModelColumn<OwnerData> ownerRows;
    simfil::ArrayArena<uint32_t, 256, 256> owners;
    simfil::ModelColumn<AliasData> aliases;
    std::map<std::string, TypeId, std::less<>> buildAliases;
    simfil::ModelColumn<QueryData> queryRoots;
    std::unordered_map<std::string, ModelNodeAddress> textIndex;  // discarded at publication
    std::vector<std::string> appendedTextKeys;
    size_t metadataAppendDepth = 0;
    // Construction guards/caches contain no structural fields or child edges.
    std::map<TypeId, std::set<std::string, std::less<>>> reservedFields;
    std::vector<TypeId> appendedPolicyTypes;
    std::map<TypeId, TypeId> arrayTypes, nullableTypes;
    std::array<StringId, 21> descriptorKeys{};

    StringId intern(std::string_view name)
    {
        auto id = dictionary.emplace(name);
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
            auto journalSize = appendedTextKeys.size();
            try {
                if (metadataAppendDepth)
                    appendedTextKeys.push_back(it->first);
                auto node = pool.tryNewValue(value);
                if (!node)
                    throw std::length_error(node.error().message);
                it->second = (*node)->addr();
            }
            catch (...) {
                appendedTextKeys.resize(journalSize);
                textIndex.erase(it);
                throw;
            }
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
        auto value = dictionary.resolve(id);
        if (!value)
            throw std::invalid_argument("MapSchema contains an unknown dictionary ID.");
        return *value;
    }

    bool valid(TypeId id) const { return id && id <= pool.simfil::SchemaPool::typeCount(); }

    /** Read the canonical sorted context column without allocating a lookup mirror. */
    TypeId findQueryRoot(TypeId feature, TypeId attribute) const
    {
        auto key = std::pair{feature, attribute};
        auto found = std::lower_bound(
            queryRoots.begin(),
            queryRoots.end(),
            key,
            [](QueryData const& row, auto const& wanted) {
                return std::pair{row.feature, row.attribute} < wanted;
            });
        return found != queryRoots.end() && found->feature == feature &&
                found->attribute == attribute ?
            found->root :
            0;
    }

    void requireType(TypeId id) const
    {
        if (!valid(id))
            throw std::invalid_argument("MapSchema references an unknown type.");
    }

    /** Translate an optional layer scope to its canonical alias-column discriminator. */
    uint16_t aliasScope(size_t layerIndex) const
    {
        if (layerIndex == GlobalLayer)
            return 0;
        if (layerIndex >= layers.size())
            throw std::invalid_argument("Invalid schema alias scope.");
        return uint16_t(layerIndex + 1);
    }

    /** Borrow alias meanings directly; no retained tree duplicates the column. */
    template <class F>
    void forEachAlias(size_t layerIndex, F&& visit) const
    {
        auto scope = aliasScope(layerIndex);
        auto first = aliases.begin();
        if (indexed)
            first = std::lower_bound(
                first,
                aliases.end(),
                scope,
                [](auto const& alias, uint16_t value) { return alias.layer < value; });
        for (; first != aliases.end(); ++first) {
            if (first->layer == scope)
                visit(text(first->key), first->id);
            else if (indexed && first->layer > scope)
                break;
        }
    }

    /** Resolve a scope-local alias without falling back to another namespace. */
    TypeId findAlias(size_t layerIndex, std::string_view key) const;

    /** Borrow map-specific metadata in the canonical generic type namespace. */
    TypeData const& data(TypeId id) const
    {
        if (!id || id > pool.simfil::SchemaPool::typeCount())
            throw std::invalid_argument("MapSchema type metadata is unavailable.");
        if (id >= types.size()) {
            static TypeData const empty{};
            return empty;
        }
        return types[id];
    }

    /** Extend only the metadata column after the generic owner reserves new type IDs. */
    TypeData& data(TypeId id)
    {
        auto count = pool.simfil::SchemaPool::typeCount();
        if (!id || id > count)
            throw std::invalid_argument("MapSchema references an unknown generic type.");
        if (types.size() <= count)
            types.resize(count + 1);
        return types[id];
    }

    LayerData& layerAt(size_t index)
    {
        if (index >= layers.size())
            throw std::invalid_argument("Invalid LayerSchema address.");
        return layers[index];
    }

    LayerData const& layerAt(size_t index) const
    {
        if (index >= layers.size())
            throw std::invalid_argument("Invalid LayerSchema address.");
        return layers[index];
    }

    /** Borrow the canonical produced-name/root pairs, preserving stored order. */
    auto featureRoots(LayerData const& layer) const
    {
        return producedFeatures.range(layer.produced);
    }

    /** Borrow relation row IDs; direct descriptor addresses use these same stable IDs. */
    auto relationIds(LayerData const& layer) const { return relationLists.range(layer.relations); }

    /** Borrow forward declarations without constructing public transport DTOs. */
    auto relationsOf(LayerData const& layer) const
    {
        auto ids = relationIds(layer);
        return std::views::iota(size_t{0}, ids.size()) |
            std::views::transform(
                   [this, ids](size_t index) -> RelationData const&
                   { return relationRows[ids[index]]; });
    }

    /** Borrow owner sites while keeping their stable rows separate from memberships. */
    auto ownerSites(simfil::ArrayIndex handle) const
    {
        auto ids = owners.range(handle);
        return std::views::iota(size_t{0}, ids.size()) |
            std::views::transform(
                   [this, ids](size_t index) -> OwnerData const& { return ownerRows[ids[index]]; });
    }

    bool relationComplete(RelationData const& relation) const { return relation.flags & 1; }

    bool relationsKnown(LayerData const& layer) const { return layer.flags & RelationsKnownFlag; }

    bool relationsDeclared(LayerData const& layer) const
    {
        return layer.flags & RelationsDeclaredFlag;
    }

    /** Check the arena builder's capacity precondition before appending untrusted metadata. */
    template <class Arena>
    static simfil::ArrayIndex allocateMetadataArray(Arena& arena, size_t count, bool fixed = false)
    {
        if (!count)
            return 0;
        if (!arena.check_capacity(count, fixed))
            throw std::length_error("Schema metadata arena capacity exceeded.");
        return arena.new_array(count, fixed);
    }

    /** Permit retired empty arrays, but never hidden live payload or a malformed empty head. */
    template <class Arena>
    static void requireOwnedArrays(Arena const& arena, std::set<simfil::ArrayIndex> const& used)
    {
        if (!arena.valid(0) || arena.size(0))
            throw std::invalid_argument("Invalid empty metadata arena head.");
        for (simfil::ArrayIndex id = 1; id < arena.size(); ++id)
            if (arena.size(id) && !used.contains(id))
                throw std::invalid_argument("Unreferenced metadata array payload.");
        for (simfil::ArrayIndex index = 0; index < arena.singleton_handle_count(); ++index) {
            auto id = simfil::SingletonArrayHandleMask | index;
            if (arena.size(id) && !used.contains(id))
                throw std::invalid_argument("Unreferenced metadata singleton payload.");
        }
    }

    /** Append a private owner-membership list with independently addressable rows. */
    simfil::ArrayIndex newOwners(std::span<OwnerData const> sites);

    /** Publish one produced root without copying the feature identity declaration. */
    void bindProduced(size_t layer, std::string_view name, TypeId root);

    /** Replace a forward inventory atomically; retired rows are zeroed and arrays emptied. */
    void setRelations(
        size_t layer,
        std::vector<LayerSchema::RelationDeclaration> declarations,
        bool known = true);

    /** Materialize an explicit public input/output DTO at the API boundary only. */
    LayerSchema::RelationDeclaration relationDeclaration(RelationData const& relation) const;

    /** Normalize legacy relation metadata before the generic schema is prepared. */
    void normalizeRelations();

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

    /** Append an immutable identity from DTO parts or borrowed canonical parts. */
    template <class Compositions, class Parts>
    void appendIdentity(std::string_view name, Compositions const& compositions, Parts&& readParts)
    {
        IdentityData identity;
        identity.name = intern(name);
        identity.compositions =
            allocateMetadataArray(identityCompositions, compositions.size(), true);
        for (auto const& composition : compositions) {
            auto&& parts = readParts(composition);
            auto handle = allocateMetadataArray(identityParts, parts.size(), true);
            for (auto const& part : parts) {
                IdentityPartData data;
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(part)>, IdPart>) {
                    data.name = intern(part.idPartLabel_);
                    data.datatype = uint8_t(part.datatype_);
                    data.flags = uint8_t(part.isSynthetic_) | (uint8_t(part.isOptional_) << 1);
                    data.description = text(part.description_);
                }
                else {
                    data.name = intern(part.name());
                    data.datatype = uint8_t(part.datatype());
                    data.flags = uint8_t(part.synthetic()) | (uint8_t(part.optional()) << 1);
                    data.description = text(part.description());
                }
                identityParts.push_back(handle, data);
            }
            identityCompositions.push_back(identity.compositions, handle);
        }
        identities.push_back(identity);  // publish only the complete definition
    }
    void compile();
    void
    compileScalarMetadata(TypeId, nlohmann::json&, std::function<void()> const& extension = {});

    /** Roll back extension appends before the enclosing generic guard restores its values. */
    void appendMetadata(std::function<void()> const& extension);

    /** Register a use site without mutating the shared physical payload type. */
    void bindAttributeOwner(TypeId id, LayerSchema::AttributePathOwner const& owner);

    /** Stage a named use site without publishing or retiring the previous membership. */
    TypeData prepareAttributeOwner(TypeId id, LayerSchema::AttributePathOwner const& owner);

    /** Restore native mutation policy after importing full shapes; thin legacy roots stay generic.
     */
    void protectNativeType(TypeId id);

    /** Recognize physical Attributes and logical combinations without rewriting generic kinds. */
    bool isAttributeType(TypeId id, bool nativeOnly = false) const;

    /** Adopt complete Attribute trees without changing their logical alternatives or named owners.
     */
    void protectAttributeType(TypeId id);

    /** Journal the first reservation so failed appends do not leave policy for retired IDs. */
    void reserveFields(TypeId id, std::set<std::string, std::less<>> fields);
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
    TypeId import(
        MapSchema const& source,
        size_t layerIndex,
        TypeId root,
        std::span<TypeId const> reuse = {});
    std::vector<LayerSchema::AttributePathOwner> attributeOwners(TypeId id) const;
    std::string_view metaType(TypeId id) const;
    TypeId childSchema(TypeId root, std::string_view field, std::optional<Kind> kind) const;
    std::vector<LayerSchema::NamedSchemaPath>
    scalarPaths(TypeId root, std::string_view attribute) const;
    std::vector<TypeId> producedRoots(LayerData const& layer) const;
};

}  // namespace mapget
