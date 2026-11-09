#pragma once

#include "mapget/model/layerschema.h"
#include "simfil/model/schema-pool.h"

#include <array>

namespace mapget
{

/** One shared native schema artifact. Build before publication; finalize to freeze.
 * Layers are views, never independent owning graphs. The schema dictionary is private
 * to this artifact and must not be substituted for a datasource's streamed dictionary.
 */
class MapSchema : public simfil::SchemaPool
{
public:
    using Ptr = std::shared_ptr<MapSchema>;
    using ConstPtr = std::shared_ptr<MapSchema const>;
    using Composition = simfil::SchemaType::Composition;

    /** Mapget-owned runtime shapes. Numeric type IDs remain artifact-local. */
    enum class Builtin {
        SourceDataReference,
        SourceDataReferences,
        Geometry,
        FeatureId,
        Validity,
        Validities,
        Relation,
        RelationReference,
        AttrPoint,
        AttrPointSequence,
        AttrPointIndex,
        AttrPointIndexRange
    };

    /** Begin a mutable artifact with its own dictionary and normalized format identity. */
    explicit MapSchema(MapFormat format = {});
    ~MapSchema() override;

    MapSchema(MapSchema const&) = delete;

    MapSchema& operator=(MapSchema const&) = delete;

    /** Create a layer or reuse the same name/kind; a conflicting kind or frozen owner throws. */
    [[nodiscard]] LayerSchema::Ptr newLayer(std::string_view name, LayerType type);

    /** Retain a named layer view, or return an empty pointer when no layer matches. */
    [[nodiscard]] LayerSchema::Ptr layer(std::string_view name) const;

    /** Copy layer names in declaration order. */
    [[nodiscard]] std::vector<std::string> layerNames() const;

    /** Define an identity once for the map, even if no served layer produces it.
     * Equivalent declarations reuse the first description; conflicting compositions fail. */
    void addFeatureType(FeatureTypeInfo info);

    /** Copy another owner's canonical identity without reconstructing a declaration DTO. */
    void addFeatureType(FeatureIdentity identity);

    /** Borrow canonical identity values; keep the schema alive while traversing. */
    auto featureIdentities() const
    {
        return std::views::iota(size_t{0}, identityCount()) |
            std::views::transform([this](size_t index) { return FeatureIdentity(this, index); });
    }

    /** Borrow a map-wide identity; missing names throw unless throwIfMissing is false. */
    [[nodiscard]] FeatureIdentity
    getTypeInfo(std::string_view name, bool throwIfMissing = true) const;

    /** Import one legacy layer transactionally. Foreign numeric identities are remapped.
     * Existing names may only be re-imported with an identical contract.
     */
    [[nodiscard]] LayerSchema::Ptr importLayer(
        std::string_view name,
        LayerType type,
        nlohmann::json const& schema,
        std::span<FeatureTypeInfo const> features = {});

    /** True once validation and exact-content identity publication have completed. */
    [[nodiscard]] bool finalized() const;

    /** Validate, prepare lookup indexes and freeze. Repeated calls are no-ops; no mutation is
     * allowed afterwards.
     */
    void finalize();

    /** Borrow the descriptive producer format; it need not uniquely identify content. */
    [[nodiscard]] MapFormat const& format() const;

    /** Borrow the content-derived artifact ID; throws before finalize(). */
    [[nodiscard]] std::string_view schemaId() const;

    /** Retained-capacity estimate for this owner, including its lookup indexes. */
    [[nodiscard]] MemoryUsageBreakdown memoryUsage() const;

    /** Return the frozen counts, 16-bit capacities and retained-memory lower bound.
     * The summary is assembled once; status polling never traverses the graph. */
    [[nodiscard]] nlohmann::json statistics() const;

    /** Ordered direct artifact lineage, independent of live datasource registrations. */
    [[nodiscard]] nlohmann::json constituents() const;

    /** Compose compatible immutable contributions into a new effective artifact.
     * Native-to-effective ID translations belong to construction/ingestion only. */
    [[nodiscard]] static ConstPtr compose(MapFormat format, std::span<ConstPtr const> inputs);

    /** Translate one contributor type; this artifact's own valid IDs are unchanged. */
    [[nodiscard]] simfil::TypeId
    remapSchema(std::string_view sourceIdentity, simfil::TypeId id) const;

    /** Borrow a construction-only source-ID table. Decoded artifacts do not retain these tables. */
    [[nodiscard]] std::span<simfil::TypeId const>
    schemaRemap(std::string_view sourceIdentity) const;

    /**
     * Write a finalized artifact with its dictionary, type columns and model payload.
     * An unfinished owner or failed write returns an EncodeDecodeError.
     */
    tl::expected<void, simfil::Error> write(std::ostream& output) override;
    tl::expected<void, simfil::Error> write(std::ostream& output) const;

    /**
     * Decode and retain a frozen artifact, verifying its checksum even on cache hits.
     * maxBytes bounds encoded input and decoder allocations, not process peak memory.
     * Nonempty expectedId must match. Unsupported versions, invalid references,
     * malformed/oversized input and identity mismatches throw.
     */
    [[nodiscard]] static ConstPtr read(
        std::span<uint8_t const> input,
        std::string_view expectedId = {},
        size_t maxBytes = 64 * 1024 * 1024);

    /** Coalesce construction before allocation; completed values are held weakly.
     * The builder must return a finalized graph. Failure is retryable. A cancelled
     * waiter does not cancel another caller's build or poison the cache.
     * Construction may outlive every waiting caller: capture inputs by ownership,
     * never by reference to a source/caller or its cancellation state. Abandoning
     * all waiters currently does not stop a construction already in progress.
     */
    [[nodiscard]] static ConstPtr getOrBuild(
        std::string key,
        std::function<ConstPtr()> builder,
        std::function<bool()> cancel = {});

    /** Retain a schema descriptor in this pool, or return null for an unknown numeric type ID. */
    [[nodiscard]] simfil::ModelNode::Ptr typeNode(simfil::TypeId id) const;
    using ModelPool::resolve;
    tl::expected<void, simfil::Error>
    resolve(simfil::ModelNode const&, ResolveFn const&) const override;
    std::vector<std::string> checkForErrors() const override;

    /** Canonical unqualified scalar type; returned types cannot be modified.
     * Use addSchema for declarations with their own units, constraints or annotations. */
    [[nodiscard]] simfil::TypeId
    primitive(simfil::SchemaType::Kind kind, std::span<std::string const> symbols = {});

    /** Canonical immutable array, retaining a reference to its element type. */
    [[nodiscard]] simfil::TypeId arrayOf(simfil::TypeId element);

    /** Nullable variant of a completed type. Seals the original as well as its variant. */
    [[nodiscard]] simfil::TypeId nullable(simfil::TypeId type);

    /** One immutable definition per runtime shape, shared across every layer. */
    [[nodiscard]] simfil::TypeId builtin(Builtin type);

    /** Create an extensible physical Attribute with mapget's common fields.
     * Register names/owners separately through LayerSchema::bindAttribute. */
    [[nodiscard]] AttributeSchemaType
    newAttribute(std::string key = {}, std::string typeName = {}, bool open = false);

    /** Graph-global declarations, shared by all layers. All borrowed input is copied. */
    [[nodiscard]] simfil::TypeId addSchema(
        simfil::SchemaType::Kind kind,
        std::string key = {},
        std::string metaType = {},
        std::string jsonPointer = {});
    [[nodiscard]] simfil::TypeId addJsonSchema(nlohmann::json const& schema);
    void registerSchemaKey(std::string key, simfil::TypeId id);
    void setJsonSchemaAnnotations(simfil::TypeId id, nlohmann::json annotations);
    void setZserioType(simfil::TypeId id, std::string name);

    /** Discriminator provenance, independent of payload typename and named use sites. */
    void setAttributeType(simfil::TypeId id, std::string name);
    void setAttributeMetadata(
        simfil::TypeId id,
        LayerSchema::AttributePathOwner owner,
        std::string attributeType,
        std::string zserioType = {});

    /** Find a global declaration key; zero means absent, not the artifact's string schemaId. */
    [[nodiscard]] simfil::TypeId findSchema(std::string_view key) const;
    [[nodiscard]] size_t typeCount() const;

protected:
    /** Protect mapget-reserved fields through all generic typed mutation paths. */
    void validateFieldMutation(simfil::TypeId id, std::string_view field) const override;

    /** Prevent kind/base/composition changes from bypassing native envelope contracts. */
    void validateStructureMutation(simfil::TypeId id) const override;

    /** Copy declarations and mutation policy, but never clone use-site ownership. */
    void cloneTypeExtension(simfil::TypeId source, simfil::TypeId target) override;

    /** Bind an attribute-use symbol to this query context's actual runtime name field. */
    std::vector<simfil::SchemaPath>
    symbolEqualityPaths(simfil::TypeId id, simfil::StringId symbol) const override;

    /** Resolve payload shorthand without choosing an arbitrary shared Attribute owner. */
    std::vector<simfil::SchemaPath>
    scalarFieldPathsForSymbol(simfil::TypeId id, simfil::StringId symbol) const override;

    /** Count mapget metadata members directly from the canonical type row. */
    uint32_t descriptorExtensionSize(simfil::TypeId id) const override;

    /** Enumerate only metadata with a value, without constructing descriptor children. */
    simfil::StringId descriptorExtensionKeyAt(simfil::TypeId id, uint32_t position) const override;

    /** Retain a metadata value from this same pool, never an adapter model. */
    simfil::ModelNode::Ptr
    descriptorExtensionGet(simfil::TypeId id, simfil::StringId key) const override;

private:
    friend class LayerSchema;
    friend simfil::model_ptr<LayerSchema>
    resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const&, simfil::ModelNode const&);
    struct Impl;
    friend class FeatureIdentity;
    friend class FeatureIdentity::Composition;
    friend class FeatureIdentity::Part;
    size_t identityCount() const;
    std::unique_ptr<Impl> schema_;
    void requireMutable() const;
    void requireMutableType(simfil::TypeId id) const;

    /** Resolve stable owner rows and their per-type membership containers in this same pool. */
    tl::expected<void, simfil::Error>
    resolveAttributeOwners(simfil::ModelNode const& node, ResolveFn const& callback) const;

    /** Borrow the six optional textual metadata values in descriptor-key order. */
    std::array<simfil::ModelNodeAddress, 6> descriptorMetadata(simfil::TypeId id) const;
};

}  // namespace mapget
