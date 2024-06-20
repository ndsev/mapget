#pragma once

#include "simfil/model/nodes.h"

#include "attr.h"
#include "attrlayer.h"
#include "featureid.h"
#include "tileid.h"
#include "relation.h"
#include "geometry.h"

#include "nlohmann/json.hpp"

#include "sfl/small_vector.hpp"

namespace mapget
{

/**
 * View onto a feature which belongs to a TileFeatureLayer.
 * You can create a feature through the TileFeatureLayer::newFeature function.
 * A Feature object maps to a GeoJSON feature object in the following way:
 *
 *   {
 *     type: "Feature",  # Mandatory for GeoJSON compliance
 *     id: "<type-id>.<part-value-0>...<part-value-n>",
 *     typeId: "<type-id>",
 *     <part-name-n>: <part-value-n>, ...
 *     geometry: <geojson-geometry>,
 *     properties: {
 *       layers: {
 *         <attr-layer-name>: {
 *           <attr-name>: {
 *             <attr-fields> ...,
 *             direction: <attr-direction>,
 *             validity: <attr-validity-geometry>
 *           }
 *         }, ...
 *       },
 *       <non-layer-attr-name>: <non-layer-attr-value>, ...
 *     },
 *     relations: [
 *       {
 *         name: <relation-name>,
 *         target: <target-feature-id>,
 *         targetValidity: <geometry>,
 *         sourceValidity: <geometry>
 *       },
 *       ...
 *     ]
 *   }
 */
class Feature : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class bitsery::Access;
    friend class TileFeatureLayer;
    friend class BoundFeature;
    template<typename> friend struct simfil::shared_model_ptr;

public:
    /** Get the name of this feature's type. */
    [[nodiscard]] std::string_view typeId() const;

    /** Get this feature's ID. */
    [[nodiscard]] model_ptr<FeatureId> id() const;

    /**
     * Get this feature's GeometryCollection. The non-const version adds a
     * GeometryCollection if the feature does not have one yet.
     */
    model_ptr<GeometryCollection> geom();
    [[nodiscard]] model_ptr<GeometryCollection> geom() const;
    [[nodiscard]] model_ptr<Geometry> firstGeometry() const;

    /**
     * Get this feature's Attribute layers. The non-const version adds a
     * AttributeLayerList if the feature does not have one yet.
     */
    model_ptr<AttributeLayerList> attributeLayers();
    [[nodiscard]] model_ptr<AttributeLayerList> attributeLayers() const;

    /**
     * Get this feature's un-layered attributes.The non-const version adds a
     * generic attribute storage if the feature does not have one yet.
     */
    model_ptr<Object> attributes();
    [[nodiscard]] model_ptr<Object> attributes() const;

    /** Add a point to the feature. */
    void addPoint(Point const& p);

    /** Add multiple points to the feature. */
    void addPoints(std::vector<Point> const& points);

    /** Add a line to the feature. */
    void addLine(std::vector<Point> const& points);

    /** Add a mesh to the feature. Points must be a multiple of 3. */
    void addMesh(std::vector<Point> const& points);

    /** Add a polygon to the feature. Will be auto-closed. Must not have holes. */
    void addPoly(std::vector<Point> const& points);

    /**
     * Evaluate a filter expression on this feature, get the first (or Null) result.
     */
    simfil::Value evaluate(std::string_view const& expression);

    /**
     * Evaluate a filter expression on this feature, get all (or no) results.
     */
    std::vector<simfil::Value> evaluateAll(std::string_view const& expression);

    /**
     * Convert the Feature to GeoJSON.
     */
    nlohmann::json toGeoJson();

    /**
     * Expose access to underlying TileFeatureLayer.
     */
    using simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>::model;

    /**
     * Create a new named relation and immediately insert it into the feature.
     * Variants:
     * (1) Creates a new feature id for the target, based on the given ID parts.
     * (2) Use an existing feature id for the target.
     * (3) Use an existing relation.
     */
    void addRelation(std::string_view const& name, std::string_view const& targetType,
        KeyValueViewPairs const& targetIdParts);
    void addRelation(std::string_view const& name, model_ptr<FeatureId> const& target);
    void addRelation(model_ptr<Relation> const& relation);

    /**
     * Visit all added relations. Return false from the callback to abort.
     * Returns false if aborted, true otherwise.
     */
    bool forEachRelation(std::function<bool(model_ptr<Relation> const&)> const& callback) const;

    /** Get all relations with the matching name. Nullopt will be returned instead of an empty vector. */
    [[nodiscard]] std::optional<std::vector<model_ptr<Relation>>> filterRelations(std::string_view const& name) const;

    /** Get the number of added relations. */
    [[nodiscard]] uint32_t numRelations() const;

    /** Get a relation at a specific index. */
    [[nodiscard]] model_ptr<Relation> getRelation(uint32_t index) const;

protected:
    /**
     * Simfil Model-Node Functions
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId&) const override;
    [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

    /**
     * Get this feature's relation list.The non-const version adds a
     * Relation list if the feature does not have one yet.
     * Note: This accessor is private, to ensure that the relations
     * array really only ever contains relations.
     */
    [[nodiscard]] model_ptr<Array> relations();
    [[nodiscard]] model_ptr<Array> relations() const;

    /**
     * Feature Data
     */
    struct Data
    {
        simfil::ModelNodeAddress id_;
        simfil::ModelNodeAddress geom_;
        simfil::ModelNodeAddress attrLayers_;
        simfil::ModelNodeAddress attrs_;
        simfil::ModelNodeAddress relations_;

        template <typename S>
        void serialize(S& s)
        {
            s.object(id_);
            s.object(geom_);
            s.object(attrLayers_);
            s.object(attrs_);
            s.object(relations_);
        }
    };

    Feature(Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
    Feature() = default;

    Data* data_ = nullptr;

    // We keep the fields in a tiny vector on the stack,
    // because their number is dynamic, as a variable number
    // of id-part fields is adopted from the feature id.
    sfl::small_vector<std::pair<simfil::FieldId, simfil::ModelNode::Ptr>, 32> fields_;
    void updateFields();

    nlohmann::json toJsonPrivate(simfil::ModelNode const&);

    struct FeaturePropertyView : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
    {
        [[nodiscard]] simfil::ValueType type() const override;
        [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
        [[nodiscard]] uint32_t size() const override;
        [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId&) const override;
        [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
        [[nodiscard]] bool iterate(IterCallback const& cb) const override;

        FeaturePropertyView(Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);
        FeaturePropertyView() = default;

        Data* data_ = nullptr;
        model_ptr<Object> attrs_;
    };
};

}  // namespace mapget
