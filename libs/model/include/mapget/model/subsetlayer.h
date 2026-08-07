#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "featureid.h"
#include "featuremodellayer.h"
#include "geometry.h"
#include "validity-data.h"
#include "simfil/diagnostics.h"
#include "simfil/environment.h"

namespace mapget
{

class TileFeatureLayer;
class TileSubsetLayer;
class TileSubsetChannel;
class FeatureEntry;
class AttributeValidityEntry;
class RelationEntry;
class GroupEntry;

/** Concrete terminal scope of one returned subset channel. */
enum class Scope : uint8_t {
    Feature,
    Attribute,
    Relation,
    Group,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    Scope,
    {
        {Scope::Feature, "feature"},
        {Scope::Attribute, "attribute"},
        {Scope::Relation, "relation"},
        {Scope::Group, "group"},
    })

/** Direction of the directed relation descriptor represented by a relation row. */
enum class RelationDirection : uint8_t {
    Forward,
    Reverse,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    RelationDirection,
    {
        {RelationDirection::Forward, "forward"},
        {RelationDirection::Reverse, "reverse"},
    })

/** One complete source tile consulted while producing an output subset. */
struct TileSubsetDependency
{
    MapTileKey sourceTileKey_;
    uint32_t sourceFeatureCount_ = 0;

    bool operator==(TileSubsetDependency const&) const = default;
};

/** Structured, self-contained issue retained on a delivered subset. */
struct FilterIssue
{
    std::string channelId_;
    std::string expression_;
    Scope scope_ = Scope::Feature;
    std::string message_;
    uint64_t occurrenceCount_ = 1;

    bool operator==(FilterIssue const&) const = default;
};

/** Transport identity readable immediately after the ordinary TileLayer bytes. */
struct FilterIdentity
{
    std::string filterId_;
    uint64_t generation_ = 0;

    bool operator==(FilterIdentity const&) const = default;
};

/**
 * Cheap subset prelude which can be read without deserializing model columns.
 *
 * The ordinary TileLayer metadata precedes this value on the wire. Keeping
 * dependencies and issues in the prelude lets transport consumers route and
 * account for a subset without retaining a second complete ModelPool.
 */
struct TileSubsetLayerMetadata
{
    FilterIdentity identity_;
    std::vector<TileSubsetDependency> dependencies_;
    std::vector<FilterIssue> issues_;
    std::optional<std::string> glbAttachmentName_;

    bool operator==(TileSubsetLayerMetadata const&) const = default;
};

/** Typed SIMFIL trace aggregate captured while producing a subset layer. */
class FilterTrace : public simfil::ProceduralObject<4, FilterTrace, TileSubsetLayer>
{
public:
    friend class TileSubsetLayer;

    struct Data
    {
        MODEL_COLUMN_TYPE(24);

        simfil::ModelNodeAddress name_{};
        simfil::ModelNodeAddress values_{};
        uint64_t calls_ = 0;
        int64_t totalUs_ = 0;
    };

    [[nodiscard]] std::string name() const;
    [[nodiscard]] uint64_t calls() const;
    [[nodiscard]] std::chrono::microseconds totalUs() const;
    [[nodiscard]] model_ptr<Array> values() const;
    [[nodiscard]] nlohmann::json toJson() const override;

    explicit FilterTrace(simfil::detail::mp_key key)
        : simfil::ProceduralObject<4, FilterTrace, TileSubsetLayer>(key) {}
    FilterTrace(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    FilterTrace() = delete;

private:
    Data* data_ = nullptr;
};

/** Compact feature row used as a terminal feature or a relation endpoint. */
class FeatureEntry : public simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>
{
public:
    friend class TileSubsetLayer;

    struct Data
    {
        MODEL_COLUMN_TYPE(12);

        simfil::ModelNodeAddress featureId_{};
        simfil::ModelNodeAddress geometry_{};
        simfil::ModelNodeAddress values_{};
    };

    [[nodiscard]] model_ptr<FeatureId> featureId() const;
    [[nodiscard]] model_ptr<GeometryCollection> geometry() const;
    [[nodiscard]] model_ptr<Array> values() const;
    [[nodiscard]] nlohmann::json toJson() const override;

protected:
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit FeatureEntry(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(key) {}
    FeatureEntry(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    FeatureEntry() = delete;

private:
    Data* data_ = nullptr;
};

/** One expanded attribute-validity candidate with host and entry projections. */
class AttributeValidityEntry : public simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>
{
public:
    friend class TileSubsetLayer;

    /** Sentinel used when an attribute candidate has no source-array index. */
    static constexpr uint32_t InvalidAttributeIndex = std::numeric_limits<uint32_t>::max();
    /** Sentinel used when geometry has no semantic transition split point. */
    static constexpr uint32_t InvalidTransitionPivotIndex = std::numeric_limits<uint32_t>::max();
    /** Semantic validity geometry representation retained by the subset entry. */
    using GeometryDescriptionType = ValidityData::GeometryDescriptionType;
    /** Connected endpoint of a feature participating in a transition. */
    using TransitionEnd = ValidityData::TransitionEnd;

    struct Data
    {
        MODEL_COLUMN_TYPE(52);

        simfil::ModelNodeAddress featureId_{};
        simfil::ModelNodeAddress geometry_{};
        simfil::ModelNodeAddress hostValues_{};
        simfil::ModelNodeAddress values_{};
        simfil::ModelNodeAddress attributeLayer_{};
        simfil::ModelNodeAddress attributeName_{};
        simfil::ModelNodeAddress transitionFromFeatureId_{};
        simfil::ModelNodeAddress transitionToFeatureId_{};
        uint32_t attributeIndex_ = InvalidAttributeIndex;
        uint32_t validityIndex_ = 0;
        uint32_t validityCount_ = 1;
        uint32_t transitionPivotIndex_ = InvalidTransitionPivotIndex;
        GeometryDescriptionType geometryDescriptionType_ = ValidityData::NoGeometry;
        uint8_t transitionConnectedEnds_ = 0;
        bool hasValidity_ = false;
    };

    [[nodiscard]] model_ptr<FeatureId> featureId() const;
    [[nodiscard]] model_ptr<GeometryCollection> geometry() const;
    [[nodiscard]] model_ptr<Array> hostValues() const;
    [[nodiscard]] model_ptr<Array> values() const;
    [[nodiscard]] std::optional<std::string> attributeLayer() const;
    [[nodiscard]] std::optional<std::string> attributeName() const;
    [[nodiscard]] std::optional<uint32_t> attributeIndex() const;
    [[nodiscard]] bool hasValidity() const;
    [[nodiscard]] uint32_t validityIndex() const;
    [[nodiscard]] uint32_t validityCount() const;
    /** Return the semantic kind represented by the materialized geometry. */
    [[nodiscard]] GeometryDescriptionType geometryDescriptionType() const;
    /** Return whether this entry carries complete feature-transition metadata. */
    [[nodiscard]] bool isFeatureTransition() const;
    /** Return the incoming feature ID for a semantic transition, if present. */
    [[nodiscard]] model_ptr<FeatureId> transitionFromFeatureId() const;
    /** Return the outgoing feature ID for a semantic transition, if present. */
    [[nodiscard]] model_ptr<FeatureId> transitionToFeatureId() const;
    /** Return the connected endpoint on the incoming transition feature. */
    [[nodiscard]] std::optional<TransitionEnd> transitionFromConnectedEnd() const;
    /** Return the connected endpoint on the outgoing transition feature. */
    [[nodiscard]] std::optional<TransitionEnd> transitionToConnectedEnd() const;
    /** Return the line-point index separating incoming and outgoing geometry. */
    [[nodiscard]] std::optional<uint32_t> transitionPivotIndex() const;
    [[nodiscard]] nlohmann::json toJson() const override;

protected:
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit AttributeValidityEntry(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(key) {}
    AttributeValidityEntry(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    AttributeValidityEntry() = delete;

private:
    Data* data_ = nullptr;
};

/** One resolved relation row referencing supporting feature rows in its channel. */
class RelationEntry : public simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>
{
public:
    friend class TileSubsetLayer;

    struct Data
    {
        MODEL_COLUMN_TYPE(36);

        simfil::ModelNodeAddress relationId_{};
        simfil::ModelNodeAddress name_{};
        simfil::ModelNodeAddress provenance_{};
        simfil::ModelNodeAddress source_{};
        simfil::ModelNodeAddress target_{};
        simfil::ModelNodeAddress sourceGeometry_{};
        simfil::ModelNodeAddress targetGeometry_{};
        simfil::ModelNodeAddress values_{};
        RelationDirection direction_ = RelationDirection::Forward;
        bool twoway_ = false;
    };

    [[nodiscard]] std::string relationId() const;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] std::string provenance() const;
    [[nodiscard]] RelationDirection direction() const;
    [[nodiscard]] bool twoway() const;
    [[nodiscard]] model_ptr<FeatureEntry> source() const;
    [[nodiscard]] model_ptr<FeatureEntry> target() const;
    [[nodiscard]] model_ptr<GeometryCollection> sourceGeometry() const;
    [[nodiscard]] model_ptr<GeometryCollection> targetGeometry() const;
    [[nodiscard]] model_ptr<Array> values() const;
    [[nodiscard]] nlohmann::json toJson() const override;

protected:
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit RelationEntry(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(key) {}
    RelationEntry(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    RelationEntry() = delete;

private:
    Data* data_ = nullptr;
};

/** One completed group with representative output and all member identities. */
class GroupEntry : public simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>
{
public:
    friend class TileSubsetLayer;

    struct Data
    {
        MODEL_COLUMN_TYPE(20);

        simfil::ModelNodeAddress groupKey_{};
        simfil::ModelNodeAddress representativeFeatureId_{};
        simfil::ModelNodeAddress geometry_{};
        simfil::ModelNodeAddress values_{};
        simfil::ModelNodeAddress memberFeatureIds_{};
    };

    [[nodiscard]] simfil::ModelNode::Ptr groupKey() const;
    [[nodiscard]] model_ptr<FeatureId> representativeFeatureId() const;
    [[nodiscard]] model_ptr<GeometryCollection> geometry() const;
    [[nodiscard]] model_ptr<Array> values() const;
    [[nodiscard]] model_ptr<Array> memberFeatureIds() const;
    [[nodiscard]] nlohmann::json toJson() const override;

protected:
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit GroupEntry(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(key) {}
    GroupEntry(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    GroupEntry() = delete;

private:
    Data* data_ = nullptr;
};

/** One ordered result schema and its typed aggregate entry arrays. */
class TileSubsetChannel : public simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>
{
public:
    friend class TileSubsetLayer;

    struct Data
    {
        MODEL_COLUMN_TYPE(40);

        simfil::ModelNodeAddress channelId_{};
        simfil::ModelNodeAddress geometryName_{};
        simfil::ArrayIndex featureFields_ = simfil::InvalidArrayIndex;
        simfil::ArrayIndex entryFields_ = simfil::InvalidArrayIndex;
        simfil::ArrayIndex featureEntries_ = simfil::InvalidArrayIndex;
        simfil::ArrayIndex attributeValidityEntries_ = simfil::InvalidArrayIndex;
        simfil::ArrayIndex relationEntries_ = simfil::InvalidArrayIndex;
        simfil::ArrayIndex groupEntries_ = simfil::InvalidArrayIndex;
        uint32_t geometryTypes_ = 0;
        Scope scope_ = Scope::Feature;
    };

    [[nodiscard]] std::string channelId() const;
    [[nodiscard]] Scope scope() const;
    [[nodiscard]] uint32_t geometryTypes() const;
    [[nodiscard]] bool hasWildcardGeometryName() const;
    [[nodiscard]] std::optional<std::string> geometryName() const;

    [[nodiscard]] size_t featureFieldCount() const;
    [[nodiscard]] std::string featureField(size_t index) const;
    [[nodiscard]] std::vector<std::string> featureFields() const;
    [[nodiscard]] size_t entryFieldCount() const;
    [[nodiscard]] std::string entryField(size_t index) const;
    [[nodiscard]] std::vector<std::string> entryFields() const;

    [[nodiscard]] size_t featureEntryCount() const;
    [[nodiscard]] size_t attributeValidityEntryCount() const;
    [[nodiscard]] size_t relationEntryCount() const;
    [[nodiscard]] size_t groupEntryCount() const;
    [[nodiscard]] size_t entryCount() const;

    model_ptr<FeatureEntry> newFeatureEntry(
        model_ptr<FeatureId> const& featureId,
        model_ptr<GeometryCollection> const& geometry,
        std::span<simfil::ModelNode::Ptr const> values = {});
    /** Add one attribute candidate, retaining semantic transition metadata when supplied. */
    model_ptr<AttributeValidityEntry> newAttributeValidityEntry(
        model_ptr<FeatureId> const& featureId,
        model_ptr<GeometryCollection> const& geometry,
        uint32_t attributeIndex,
        bool hasValidity,
        uint32_t validityIndex,
        uint32_t validityCount,
        std::span<simfil::ModelNode::Ptr const> hostValues = {},
        std::span<simfil::ModelNode::Ptr const> values = {},
        std::optional<std::string_view> attributeLayer = std::nullopt,
        std::optional<std::string_view> attributeName = std::nullopt,
        AttributeValidityEntry::GeometryDescriptionType geometryDescriptionType =
            ValidityData::NoGeometry,
        model_ptr<FeatureId> const& transitionFromFeatureId = {},
        AttributeValidityEntry::TransitionEnd transitionFromConnectedEnd =
            ValidityData::Start,
        model_ptr<FeatureId> const& transitionToFeatureId = {},
        AttributeValidityEntry::TransitionEnd transitionToConnectedEnd =
            ValidityData::Start,
        uint32_t transitionPivotIndex =
            AttributeValidityEntry::InvalidTransitionPivotIndex);
    model_ptr<RelationEntry> newRelationEntry(
        std::string_view relationId,
        std::string_view name,
        std::string_view provenance,
        RelationDirection direction,
        bool twoway,
        model_ptr<FeatureEntry> const& source,
        model_ptr<FeatureEntry> const& target,
        model_ptr<GeometryCollection> const& sourceGeometry,
        model_ptr<GeometryCollection> const& targetGeometry,
        std::span<simfil::ModelNode::Ptr const> values = {});
    model_ptr<GroupEntry> newGroupEntry(
        simfil::ModelNode::Ptr const& groupKey,
        model_ptr<FeatureId> const& representativeFeatureId,
        model_ptr<GeometryCollection> const& geometry,
        std::span<simfil::ModelNode::Ptr const> values,
        std::span<model_ptr<FeatureId> const> memberFeatureIds);

    bool forEachFeatureEntry(
        std::function<bool(model_ptr<FeatureEntry> const&)> const& callback) const;
    bool forEachAttributeValidityEntry(
        std::function<bool(model_ptr<AttributeValidityEntry> const&)> const& callback) const;
    bool forEachRelationEntry(
        std::function<bool(model_ptr<RelationEntry> const&)> const& callback) const;
    bool forEachGroupEntry(
        std::function<bool(model_ptr<GroupEntry> const&)> const& callback) const;

    [[nodiscard]] nlohmann::json toJson() const override;

protected:
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t index) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(simfil::StringId const& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t index) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

public:
    explicit TileSubsetChannel(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileSubsetLayer>(key) {}
    TileSubsetChannel(
        Data* data,
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key);
    TileSubsetChannel() = delete;

private:
    Data* data_ = nullptr;
};

/**
 * Immutable-on-publication server-evaluated subset for one output tile.
 *
 * During request execution one single writer may construct this value. Channel
 * roots are the only public root enumeration; all typed entries remain in
 * layer-owned columns and are reached through their channel aggregate arrays.
 */
class TileSubsetLayer : public TileFeatureModelLayerBase
{
    friend class FilterTrace;
    friend class FeatureEntry;
    friend class AttributeValidityEntry;
    friend class RelationEntry;
    friend class GroupEntry;
    friend class TileSubsetChannel;
    template<typename Target>
    friend model_ptr<Target> resolveInternal(
        simfil::res::tag<Target>,
        TileSubsetLayer const&,
        simfil::ModelNode const&);

public:
    using TileFeatureModelLayerBase::resolve;
    using Ptr = std::shared_ptr<TileSubsetLayer>;
    using ColumnId = TileFeatureModelLayerBase::ColumnId;

    TileSubsetLayer(
        TileId tileId,
        std::string const& stringPoolId,
        std::string const& mapId,
        std::shared_ptr<LayerInfo> const& layerInfo,
        std::shared_ptr<simfil::StringPool> const& strings,
        std::string filterId = {},
        uint64_t generation = 0);

    TileSubsetLayer(
        std::vector<uint8_t> const& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        StringPoolResolveFun const& stringPoolGetter);

    ~TileSubsetLayer() override;

    [[nodiscard]] static FilterIdentity readFilterIdentity(
        std::vector<uint8_t> const& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        size_t* bytesRead = nullptr);

    /** Read the complete transport prelude without deserializing model columns. */
    [[nodiscard]] static TileSubsetLayerMetadata readMetadata(
        std::vector<uint8_t> const& input,
        LayerInfoResolveFun const& layerInfoResolveFun,
        size_t* bytesRead = nullptr);

    [[nodiscard]] std::string const& filterId() const;
    [[nodiscard]] uint64_t generation() const;

    void adoptSourceInfo(TileFeatureLayer const& source);
    void setDependencies(std::vector<TileSubsetDependency> dependencies);
    void addDependency(MapTileKey sourceTileKey, uint32_t sourceFeatureCount);
    [[nodiscard]] std::vector<TileSubsetDependency> const& dependencies() const;
    [[nodiscard]] std::optional<uint32_t> localSourceFeatureCount() const;

    void addIssue(FilterIssue issue);
    [[nodiscard]] std::vector<FilterIssue> const& issues() const;

    void setDiagnostics(simfil::Diagnostics const& diagnostics);
    [[nodiscard]] simfil::Diagnostics const& diagnostics() const;
    void setTraces(std::map<std::string, simfil::Trace> traces);
    [[nodiscard]] size_t traceCount() const;
    [[nodiscard]] model_ptr<FilterTrace> traceAt(size_t index) const;

    void setGlbAttachmentName(std::optional<std::string> name);
    [[nodiscard]] std::optional<std::string> const& glbAttachmentName() const;

    [[nodiscard]] simfil::ModelNode::Ptr materializeValue(simfil::Value const& value);

    model_ptr<TileSubsetChannel> newChannel(
        std::string_view channelId,
        Scope scope,
        uint32_t geometryTypes,
        std::optional<std::string_view> geometryName,
        std::span<std::string const> featureFields = {},
        std::span<std::string const> entryFields = {});
    model_ptr<FeatureEntry> newFeatureEntry(
        model_ptr<FeatureId> const& featureId,
        model_ptr<GeometryCollection> const& geometry,
        std::span<simfil::ModelNode::Ptr const> values = {});
    /** Add one attribute candidate, retaining semantic transition metadata when supplied. */
    model_ptr<AttributeValidityEntry> newAttributeValidityEntry(
        model_ptr<FeatureId> const& featureId,
        model_ptr<GeometryCollection> const& geometry,
        uint32_t attributeIndex,
        bool hasValidity,
        uint32_t validityIndex,
        uint32_t validityCount,
        std::span<simfil::ModelNode::Ptr const> hostValues = {},
        std::span<simfil::ModelNode::Ptr const> values = {},
        std::optional<std::string_view> attributeLayer = std::nullopt,
        std::optional<std::string_view> attributeName = std::nullopt,
        AttributeValidityEntry::GeometryDescriptionType geometryDescriptionType =
            ValidityData::NoGeometry,
        model_ptr<FeatureId> const& transitionFromFeatureId = {},
        AttributeValidityEntry::TransitionEnd transitionFromConnectedEnd =
            ValidityData::Start,
        model_ptr<FeatureId> const& transitionToFeatureId = {},
        AttributeValidityEntry::TransitionEnd transitionToConnectedEnd =
            ValidityData::Start,
        uint32_t transitionPivotIndex =
            AttributeValidityEntry::InvalidTransitionPivotIndex);
    model_ptr<RelationEntry> newRelationEntry(
        std::string_view relationId,
        std::string_view name,
        std::string_view provenance,
        RelationDirection direction,
        bool twoway,
        model_ptr<FeatureEntry> const& source,
        model_ptr<FeatureEntry> const& target,
        model_ptr<GeometryCollection> const& sourceGeometry,
        model_ptr<GeometryCollection> const& targetGeometry,
        std::span<simfil::ModelNode::Ptr const> values = {});
    model_ptr<GroupEntry> newGroupEntry(
        simfil::ModelNode::Ptr const& groupKey,
        model_ptr<FeatureId> const& representativeFeatureId,
        model_ptr<GeometryCollection> const& geometry,
        std::span<simfil::ModelNode::Ptr const> values,
        std::span<model_ptr<FeatureId> const> memberFeatureIds);

    model_ptr<FeatureId> newFeatureId(
        std::string_view const& typeId,
        KeyValueViewPairs const& featureIdParts,
        std::optional<std::string_view> externalMapId = std::nullopt) override;
    model_ptr<GeometryCollection> newGeometryCollection(
        size_t initialCapacity = 2,
        bool fixedSize = false) override;
    model_ptr<Geometry> newGeometry(
        GeomType geomType,
        size_t initialCapacity = 2,
        bool fixedSize = false) override;
    model_ptr<Geometry> newGeometryView(
        GeomType geomType,
        uint32_t offset,
        uint32_t size,
        model_ptr<Geometry> const& base) override;
    model_ptr<SourceDataReferenceCollection> newSourceDataReferenceCollection(
        std::span<QualifiedSourceDataReference> list) override;

    tl::expected<void, simfil::Error> write(std::ostream& outputStream) override;
    [[nodiscard]] nlohmann::json toJson() const override;

    /** Report retained subset-model, diagnostics, issue, and dependency capacity. */
    [[nodiscard]] MemoryUsageBreakdown memoryUsage() const override;

    [[nodiscard]] size_t size() const;
    [[nodiscard]] model_ptr<TileSubsetChannel> at(size_t index) const;
    bool forEachChannel(
        std::function<bool(model_ptr<TileSubsetChannel> const&)> const& callback) const;

    /** Access the number of geometry vertices stored in this subset. */
    [[nodiscard]] uint64_t numVertices() const;

    [[nodiscard]] Point geometryAnchor() const override;
    void setGeometryAnchor(Point const& anchor);

protected:
    tl::expected<void, simfil::Error> resolve(
        simfil::ModelNode const& node,
        ResolveFn const& callback) const override;

private:
    [[nodiscard]] model_ptr<Array> newValueArray(
        std::span<simfil::ModelNode::Ptr const> values);
    [[nodiscard]] model_ptr<Array> newStringArray(std::span<std::string const> values);
    [[nodiscard]] static std::string nodeString(simfil::ModelNode::Ptr const& node);
    void validateOwnedNode(simfil::ModelNode::Ptr const& node, std::string_view role) const;
    void updateEntryStatistics();

    Point geometryAnchor_{};
    std::string filterId_;
    uint64_t generation_ = 0;
    std::vector<TileSubsetDependency> dependencies_;
    std::vector<FilterIssue> issues_;
    std::optional<std::string> glbAttachmentName_;
    simfil::Diagnostics diagnostics_;

    simfil::ModelColumn<TileSubsetChannel::Data, simfil::detail::ColumnPageSize / 2> channels_;
    simfil::ModelColumn<FeatureEntry::Data, simfil::detail::ColumnPageSize / 2> featureEntries_;
    simfil::ModelColumn<AttributeValidityEntry::Data, simfil::detail::ColumnPageSize / 2>
        attributeValidityEntries_;
    simfil::ModelColumn<RelationEntry::Data, simfil::detail::ColumnPageSize / 2> relationEntries_;
    simfil::ModelColumn<GroupEntry::Data, simfil::detail::ColumnPageSize / 2> groupEntries_;
    simfil::ModelColumn<FilterTrace::Data, simfil::detail::ColumnPageSize / 2> traces_;
};

template<typename Target>
simfil::model_ptr<Target> resolveInternal(
    simfil::res::tag<Target>,
    TileSubsetLayer const& model,
    simfil::ModelNode const& node);

} // namespace mapget
