#pragma once

#include "featureid.h"
#include "attr.h"

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
    model_ptr<Attribute> newAttribute(std::string_view const& name, size_t initialCapacity=8);

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
class AttributeLayerList : public simfil::BaseObject<TileFeatureLayer, AttributeLayer>
{
    friend class TileFeatureLayer;
    friend class bitsery::Access;
    friend class Feature;

public:
    using BaseObject::addField;
    using BaseObject::get;

    /**
     * Create a new named layer and immediately insert it into the collection.
     */
    model_ptr<AttributeLayer> newLayer(std::string_view const& name, size_t initialCapacity=8);

    /**
     * Add an attribute layer to the collection which was previously created.
     * You can share a single layer between multiple collections, it will not be copied.
     */
    void addLayer(std::string_view const& name, model_ptr<AttributeLayer> l);

    /**
     * Iterate over the stored layers. The passed lambda must return
     * true to continue iterating, or false to abort iteration.
     * @return True if all layers were visited, false if the callback ever returned false.
     */
    bool forEachLayer(
        std::function<bool(std::string_view, model_ptr<AttributeLayer> const& layer)> const& cb
    ) const;

public:
    explicit AttributeLayerList(simfil::detail::mp_key key) : BaseObject(key) {}
    AttributeLayerList(simfil::ArrayIndex i,
                       simfil::ModelConstPtr l,
                       simfil::ModelNodeAddress a,
                       simfil::detail::mp_key key);
    AttributeLayerList() = delete;
};

}
