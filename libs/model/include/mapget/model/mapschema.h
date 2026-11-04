#pragma once

#include "mapget/model/layerschema.h"
#include "simfil/model/schema-pool.h"

namespace simfil
{
class SchemaBinding;
}

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
    using Composition = simfil::Schema::Composition;

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

    explicit MapSchema(MapFormat format = {});
    ~MapSchema() override;
    MapSchema(MapSchema const&) = delete;
    MapSchema& operator=(MapSchema const&) = delete;

    [[nodiscard]] LayerSchema::Ptr newLayer(std::string_view name, LayerType type);
    [[nodiscard]] LayerSchema::Ptr layer(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> layerNames() const;
    /** Import one legacy layer transactionally. Foreign numeric identities are remapped.
     * Existing names may only be re-imported with an identical contract.
     */
    [[nodiscard]] LayerSchema::Ptr importLayer(
        std::string_view name,
        LayerType type,
        nlohmann::json const& schema,
        std::span<FeatureTypeInfo const> features = {});

    [[nodiscard]] bool finalized() const;
    /** Validate, compact and freeze. Repeated calls are no-ops; no mutation is allowed afterwards.
     */
    void finalize();
    [[nodiscard]] MapFormat const& format() const;
    [[nodiscard]] std::string_view schemaId() const;
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

    /** Canonical binary envelope, dictionary, type columns and ModelPool payload. */
    tl::expected<void, simfil::Error> write(std::ostream& output) override;
    tl::expected<void, simfil::Error> write(std::ostream& output) const;
    /** Reject unsupported versions, invalid references, oversized input and identity mismatch. */
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

    /** Schema-local lookup. The returned binding pins this graph without an owner cycle. */
    [[nodiscard]] std::shared_ptr<simfil::SchemaBinding const> binding() const;
    using SchemaPool::type;
    [[nodiscard]] simfil::Schema const* type(simfil::TypeId id) const;
    [[nodiscard]] simfil::ModelNode::Ptr typeNode(simfil::TypeId id) const;
    using ModelPool::resolve;
    tl::expected<void, simfil::Error>
    resolve(simfil::ModelNode const&, ResolveFn const&) const override;
    std::vector<std::string> checkForErrors() const override;

    /** Canonical unqualified scalar type; returned types cannot be modified.
     * Use addSchema for declarations with their own units, constraints or annotations. */
    [[nodiscard]] simfil::TypeId
    primitive(simfil::Schema::Kind kind, std::span<std::string const> symbols = {});
    /** Canonical immutable array, retaining a reference to its element type. */
    [[nodiscard]] simfil::TypeId arrayOf(simfil::TypeId element);
    /** Nullable variant of a completed type. Seals the original as well as its variant. */
    [[nodiscard]] simfil::TypeId nullable(simfil::TypeId type);
    /** One immutable definition per runtime shape, shared across every layer. */
    [[nodiscard]] simfil::TypeId builtin(Builtin type);
    /** Create an extensible physical Attribute with mapget's common fields.
     * Register names/owners separately through LayerSchema::bindAttribute. */
    [[nodiscard]] simfil::TypeId
    newAttribute(std::string key = {}, std::string typeName = {}, bool open = false);

    /** Graph-global declarations, shared by all layers. All borrowed input is copied. */
    [[nodiscard]] simfil::TypeId addSchema(
        simfil::Schema::Kind kind,
        std::string key = {},
        std::string metaType = {},
        std::string jsonPointer = {});
    [[nodiscard]] simfil::TypeId addJsonSchema(nlohmann::json const& schema);
    void registerSchemaKey(std::string key, simfil::TypeId id);
    void setJsonSchemaAnnotations(simfil::TypeId id, nlohmann::json annotations);
    void addFieldSchema(
        simfil::TypeId parent,
        std::string field,
        simfil::TypeId child = simfil::NoTypeId,
        bool multimap = false);
    void addElementSchema(simfil::TypeId parent, simfil::TypeId child);
    void addEnumSymbol(simfil::TypeId id, std::string name);
    void addEnumSymbols(simfil::TypeId id, std::span<std::string const> names);
    void addEnumValue(simfil::TypeId id, simfil::ScalarValueType value);
    void setComposition(simfil::TypeId id, Composition composition);
    void addAlternative(simfil::TypeId id, simfil::TypeId alternative);
    void setTypeName(simfil::TypeId id, std::string name);
    void setOpen(simfil::TypeId id, bool open);
    void setNullable(simfil::TypeId id, std::optional<bool> nullable);
    void setFieldRequired(simfil::TypeId id, std::string field, std::optional<bool> required);
    /** Mark a declared edge as enumerable data or explicit-lookup-only context. */
    void setFieldRole(simfil::TypeId id, std::string field, simfil::Schema::FieldRole role);
    void setZserioType(simfil::TypeId id, std::string name);
    /** Discriminator provenance, independent of payload typename and named use sites. */
    void setAttributeType(simfil::TypeId id, std::string name);
    void setAttributeMetadata(
        simfil::TypeId id,
        LayerSchema::AttributePathOwner owner,
        std::string attributeType,
        std::string zserioType = {});
    /** Copy exact numeric semantics; input views need not outlive this call. */
    void setNumericDomain(simfil::TypeId id, simfil::Schema::NumericDomain const& domain);
    void addNamedValue(simfil::TypeId id, simfil::Schema::NamedValue const& value);
    void setBitWidth(simfil::TypeId id, uint8_t width);
    [[nodiscard]] std::optional<simfil::Schema::NumericDomain>
    numericDomain(simfil::TypeId id) const;
    void forEachNamedValue(
        simfil::TypeId id,
        std::function<void(simfil::Schema::NamedValue const&)> const& fn) const;
    [[nodiscard]] uint8_t bitWidth(simfil::TypeId id) const;

    [[nodiscard]] simfil::TypeId findSchema(std::string_view key) const;
    [[nodiscard]] size_t typeCount() const;

private:
    friend class LayerSchema;
    friend simfil::model_ptr<LayerSchema>
    resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const&, simfil::ModelNode const&);
    struct Impl;
    std::unique_ptr<Impl> schema_;
    void requireMutable() const;
    void requireMutableType(simfil::TypeId id) const;
};

}  // namespace mapget
