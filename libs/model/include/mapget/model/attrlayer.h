#pragma once

#include "featureid.h"
#include "attr.h"
#include "merged-array-view.h"

namespace mapget
{

class AttributeLayerList;

/**
 * Represents a collection of Attributes which are semantically related.
 * For example, all feature attributes which refer to road rules, such
 * as speed limits, might belong to the same attribute layer.
 * TODO: Convert to use BaseObject
 */
class AttributeLayer : public simfil::BaseObject<TileFeatureLayer, Attribute>
{
    friend class TileFeatureLayer;
    friend class bitsery::Access;

public:
    using BaseObject::addField;
    using BaseObject::get;

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
     * features, it will not be copied.
     */
    void addAttribute(model_ptr<Attribute> a);

    /**
     * Iterate over the stored attributes. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
     * @return True if all attributes were visited, false if the callback ever returned false.
     */
    bool forEachAttribute(std::function<bool(model_ptr<Attribute> const& attr)> const& cb) const;

public:
    explicit AttributeLayer(simfil::detail::mp_key key) : BaseObject(key) {}
    AttributeLayer(simfil::ArrayIndex i,
                   simfil::ModelConstPtr l,
                   simfil::ModelNodeAddress a,
                   simfil::detail::mp_key key);
    AttributeLayer() = delete;
};

/**
 * Collection of attribute layers - this is merely a typed dict which
 * stores (layer-name, layer) pairs.
 * TODO: Convert to use BaseObject
 */
class AttributeLayerList : public MergedArrayView<AttributeLayerList, AttributeLayer>
{
    friend class TileFeatureLayer;
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
     * You can share a single layer between multiple collections, it will not be copied.
     */
    void addLayer(std::string_view const& name, model_ptr<AttributeLayer> l);

    /**
     * Backward-compatible alias for addLayer.
     */
    tl::expected<std::reference_wrapper<AttributeLayerList>, simfil::Error>
    addField(std::string_view const& name, model_ptr<AttributeLayer> l);

    /**
     * Iterate over the stored layers. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
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

    AttributeLayerList(simfil::ArrayIndex i,
                       simfil::ModelConstPtr l,
                       simfil::ModelNodeAddress a,
                       simfil::detail::mp_key key);
    AttributeLayerList() = delete;

    /** Return the object schema assigned to the layer-name map. */
    [[nodiscard]] simfil::SchemaId schema() const override;

    /** Assign the object schema for the layer-name map stored by this view. */
    tl::expected<void, simfil::Error> setObjectSchema(simfil::SchemaId schemaId);

    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] simfil::ModelNode::Ptr at(int64_t i) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] simfil::ModelNode::Ptr get(const simfil::StringId& field) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t i) const override;
    bool iterate(simfil::ModelNode::IterCallback const& cb) const override;

private:
    [[nodiscard]] simfil::model_ptr<simfil::Object> localObject() const;
    [[nodiscard]] uint32_t localMergedSize() const override;
    [[nodiscard]] simfil::ModelNode::Ptr localMergedAt(int64_t i) const override;
    bool localMergedIterate(simfil::ModelNode::IterCallback const& cb) const override;

    simfil::ArrayIndex members_ = simfil::InvalidArrayIndex;
};

}
