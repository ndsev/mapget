#pragma once

#include "featureid.h"
#include "attr.h"
#include "merged-array-view.h"

#include "nlohmann/json.hpp"

#include <optional>

namespace mapget
{

class AttributeLayerList;
class Feature;

/**
 * Represents a collection of Attributes which are semantically related.
 * For example, all feature attributes which refer to road rules, such
 * as speed limits, might belong to the same attribute layer.
 */
class AttributeLayer : public simfil::BaseObject<PartitionFeatureLayer, Attribute>
{
    friend class PartitionFeatureLayer;
    friend class bitsery::Access;

public:
    static constexpr std::string_view InstanceIdField = "id";

    using BaseObject::addField;
    using BaseObject::get;

    /**
     * Attach a numeric source-layer instance id to this attribute layer.
     * IDs must fit in int64_t: the model stores a signed value, and id()
     * treats negative stored values as absent.
     *
     * AttributeLayerList may contain multiple entries with the same public
     * layer name. The instance id lets GeoJSON/inspection users distinguish
     * those otherwise identical layer objects without changing query/style
     * paths such as `properties.layer.RoadRulesLayer`.
     */
    void setId(uint64_t id);
    [[nodiscard]] std::optional<uint64_t> id() const;
    [[nodiscard]] nlohmann::json toJson() const override;

    /**
     * Create a new attribute and immediately insert it into the layer.
     */
    model_ptr<Attribute> newAttribute(
        std::string_view const& name,
        size_t initialCapacity=8,
        bool fixedSize=false);

    /**
     * Add an attribute to the layer which was created before - note:
     * the same attribute can be added to multiple layers for different
     * features in the same owning PartitionFeatureLayer; it is not copied.
     */
    void addAttribute(model_ptr<Attribute> a);

    /**
     * Iterate over the stored attributes. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
     * Non-attribute fields are skipped. An empty callback returns false.
     * @return True if all attributes were visited, false if the callback ever returned false.
     */
    bool forEachAttribute(std::function<bool(model_ptr<Attribute> const& attr)> const& cb) const;

public:
    explicit AttributeLayer(simfil::detail::mp_key key) : BaseObject(key) {}
    AttributeLayer(
        simfil::ArrayIndex i,
        simfil::ModelConstPtr l,
        simfil::ModelNodeAddress a,
        simfil::detail::mp_key key);

    AttributeLayer() = delete;
};

/**
 * Named attribute-layer instances belonging to a feature. Repeated names
 * remain separate instances; query paths such as properties.layer.RoadRules
 * can therefore yield more than one layer. Nodes share this feature layer's pool.
 */
class AttributeLayerList : public MergedArrayView<AttributeLayerList, AttributeLayer>
{
    friend class PartitionFeatureLayer;
    friend class bitsery::Access;
    friend class Feature;

public:
    /**
     * Create a new named layer and immediately insert it into the collection.
     */
    model_ptr<AttributeLayer> newLayer(
        std::string_view const& name,
        size_t initialCapacity=8,
        bool fixedSize=false);

    /**
     * Add an attribute layer to the collection which was previously created.
     * Collections in the same owning PartitionFeatureLayer can share a layer;
     * the operation stores its address rather than copying its fields.
     */
    void addLayer(std::string_view const& name, model_ptr<AttributeLayer> l);

    /**
     * Insert a layer through the underlying object API and return its error.
     * Unlike addLayer(), this does not assign the child schema. Feature-scoped
     * views reject insertion; populate the owning concrete collection instead.
     */
    tl::expected<std::reference_wrapper<AttributeLayerList>, simfil::Error>
    addField(std::string_view const& name, model_ptr<AttributeLayer> l);

    /**
     * Iterate over the stored layers. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
     * Non-layer fields are skipped. An empty callback returns false.
     * @return True if all layers were visited, false if the callback ever returned false.
     */
    bool forEachLayer(
        std::function<bool(std::string_view, model_ptr<AttributeLayer> const& layer)> const& cb
    ) const;

public:
    explicit AttributeLayerList(simfil::detail::mp_key key)
        : MergedArrayView<AttributeLayerList, AttributeLayer>(key)
    {
    }

    AttributeLayerList(
        simfil::ArrayIndex i,
        simfil::ModelConstPtr l,
        simfil::ModelNodeAddress a,
        simfil::detail::mp_key key);

    AttributeLayerList() = delete;

    /** Return the object schema assigned to the layer-name map. */
    [[nodiscard]] simfil::TypeId schema() const override;

    /** Assign the object schema for the layer-name map stored by this view. */
    tl::expected<void, simfil::Error> setObjectSchema(simfil::TypeId schemaId);

    [[nodiscard]] nlohmann::json toJson() const override;
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t i) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(const simfil::StringId& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t i) const override;
    bool iterate(simfil::ModelNode::IterCallback const& cb) const override;

private:
    [[nodiscard]] simfil::model_ptr<simfil::Object> localObject() const;
    [[nodiscard]] simfil::model_ptr<AttributeLayerList> localConcreteList() const;
    [[nodiscard]] bool isFeatureScopedView() const;
    [[nodiscard]] model_ptr<Feature> featureScopedFeature() const;
    [[nodiscard]] ExtensionPtr mergedExtension() const override;
    [[nodiscard]] uint32_t localMergedSize() const override;
    [[nodiscard]] simfil::ModelNode::Ptr localMergedAt(int64_t i) const override;
    bool localMergedIterate(simfil::ModelNode::IterCallback const& cb) const override;

    simfil::ArrayIndex members_ = simfil::InvalidArrayIndex;
};

}
