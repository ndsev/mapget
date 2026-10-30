#pragma once

#include "mapget/model/layerschema.h"

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
class MapSchema : public simfil::ModelPool
{
public:
    using Ptr = std::shared_ptr<MapSchema>;
    using ConstPtr = std::shared_ptr<MapSchema const>;
    using Composition = simfil::Schema::Composition;

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

    /** Canonical binary envelope, dictionary, domain columns and ModelPool payload. */
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
    [[nodiscard]] simfil::Schema const* domain(simfil::SchemaId id) const;
    [[nodiscard]] simfil::ModelNode::Ptr domainNode(simfil::SchemaId id) const;
    using ModelPool::resolve;
    tl::expected<void, simfil::Error>
    resolve(simfil::ModelNode const&, ResolveFn const&) const override;
    std::vector<std::string> checkForErrors() const override;

    /** Graph-global declarations, shared by all layers. All borrowed input is copied. */
    [[nodiscard]] simfil::SchemaId addSchema(
        simfil::Schema::Kind kind,
        std::string key = {},
        std::string metaType = {},
        std::string jsonPointer = {});
    [[nodiscard]] simfil::SchemaId addJsonSchema(nlohmann::json const& schema);
    void registerSchemaKey(std::string key, simfil::SchemaId id);
    void setJsonSchemaAnnotations(simfil::SchemaId id, nlohmann::json annotations);
    void addFieldSchema(
        simfil::SchemaId parent,
        std::string field,
        simfil::SchemaId child = simfil::NoSchemaId,
        bool multimap = false);
    void addElementSchema(simfil::SchemaId parent, simfil::SchemaId child);
    void addEnumSymbol(simfil::SchemaId id, std::string name);
    void addEnumSymbols(simfil::SchemaId id, std::span<std::string const> names);
    void addEnumValue(simfil::SchemaId id, simfil::ScalarValueType value);
    void setComposition(simfil::SchemaId id, Composition composition);
    void addAlternative(simfil::SchemaId id, simfil::SchemaId alternative);
    void setTypeName(simfil::SchemaId id, std::string name);
    void setOpen(simfil::SchemaId id, bool open);
    void setNullable(simfil::SchemaId id, std::optional<bool> nullable);
    void setFieldRequired(simfil::SchemaId id, std::string field, std::optional<bool> required);
    void setZserioType(simfil::SchemaId id, std::string name);
    void setAttributeMetadata(
        simfil::SchemaId id,
        LayerSchema::AttributePathOwner owner,
        std::string attributeType,
        std::string zserioType = {});
    /** Copy exact numeric semantics; input views need not outlive this call. */
    void setNumericDomain(simfil::SchemaId id, simfil::Schema::NumericDomain const& domain);
    void addNamedValue(simfil::SchemaId id, simfil::Schema::NamedValue const& value);
    void setBitWidth(simfil::SchemaId id, uint8_t width);
    [[nodiscard]] std::optional<simfil::Schema::NumericDomain>
    numericDomain(simfil::SchemaId id) const;
    void forEachNamedValue(
        simfil::SchemaId id,
        std::function<void(simfil::Schema::NamedValue const&)> const& fn) const;
    [[nodiscard]] uint8_t bitWidth(simfil::SchemaId id) const;

    [[nodiscard]] simfil::SchemaId findSchema(std::string_view key) const;
    [[nodiscard]] size_t domainCount() const;

private:
    friend class LayerSchema;
    friend simfil::model_ptr<LayerSchema>
    resolveInternal(simfil::res::tag<LayerSchema>, MapSchema const&, simfil::ModelNode const&);
    struct Impl;
    std::unique_ptr<Impl> schema_;
    void requireMutable() const;
};

}  // namespace mapget
