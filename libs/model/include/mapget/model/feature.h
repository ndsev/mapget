#pragma once

#include "simfil/model/nodes.h"

#include "attr.h"
#include "attrlayer.h"
#include "featureid.h"
#include "tileid.h"
#include "relation.h"
#include "geometry.h"
#include "merged-array-view.h"

#include "tl/expected.hpp"
#include "sfl/small_vector.hpp"
#include "nlohmann/json.hpp"
#include <utility>

namespace mapget
{

class RelationArrayView : public MergedArrayView<RelationArrayView, simfil::ModelNode>
{
public:
    explicit RelationArrayView(simfil::detail::mp_key key)
        : MergedArrayView<RelationArrayView, simfil::ModelNode>(key)
    {
    }

    RelationArrayView(
        simfil::ModelConstPtr pool,
        simfil::ModelNodeAddress address,
        simfil::detail::mp_key key)
        : MergedArrayView<RelationArrayView, simfil::ModelNode>(std::move(pool), address, key)
    {
    }

    RelationArrayView() = delete;

private:
    [[nodiscard]] uint32_t localMergedSize() const override;
    [[nodiscard]] simfil::ModelNode::Ptr localMergedAt(int64_t i) const override;
    bool localMergedIterate(simfil::ModelNode::IterCallback const& cb) const override;
};

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
    friend class RelationArrayView;

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
    [[nodiscard]] model_ptr<GeometryCollection> geomOrNull() const;
    [[nodiscard]] SelfContainedGeometry firstGeometry() const;

    /**
     * Get this feature's Attribute layers. The non-const version adds a
     * AttributeLayerList if the feature does not have one yet.
     */
    model_ptr<AttributeLayerList> attributeLayers();
    [[nodiscard]] model_ptr<AttributeLayerList> attributeLayersOrNull() const;

    /**
     * Get this feature's un-layered attributes.The non-const version adds a
     * generic attribute storage if the feature does not have one yet.
     */
    model_ptr<Object> attributes();
    [[nodiscard]] model_ptr<Object> attributesOrNull() const;

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
    tl::expected<simfil::Value, simfil::Error>
    evaluate(std::string_view const& expression);

    /**
     * Evaluate a filter expression on this feature, get all (or no) results.
     */
    tl::expected<std::vector<simfil::Value>, simfil::Error>
    evaluateAll(std::string_view const& expression);

    /**
     * Convert the Feature to (Geo-) JSON.
     */
    nlohmann::json toJson() const override;

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
    model_ptr<Relation> addRelation(std::string_view const& name, std::string_view const& targetType,
        KeyValueViewPairs const& targetIdParts);
    model_ptr<Relation> addRelation(std::string_view const& name, model_ptr<FeatureId> const& target);
    model_ptr<Relation> addRelation(model_ptr<Relation> const& relation);

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

    /**
     * SourceData accessors.
     */
    [[nodiscard]] model_ptr<SourceDataReferenceCollection> sourceDataReferences() const;
    void setSourceDataReferences(simfil::ModelNode::Ptr const& addresses);

    [[nodiscard]] model_ptr<Feature> extension() const;
    void setExtension(model_ptr<Feature> extension);

protected:
    /**
     * Simfil Model-Node Functions
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::StringId&) const override;
    [[nodiscard]] simfil::StringId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

    /**
     * Get this feature's relation list.The non-const version adds a
     * Relation list if the feature does not have one yet.
     * Note: This accessor is private, to ensure that the relations
     * array really only ever contains relations.
     * TODO: Change relations to use a RelationCollection derived from BaseArray
     */
    [[nodiscard]] model_ptr<Array> relations();
    [[nodiscard]] model_ptr<Array> relationsOrNull() const;
    [[nodiscard]] model_ptr<RelationArrayView> mergedRelationsOrNull() const;

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
        simfil::ModelNodeAddress sourceData_;
    };

public:
    explicit Feature(simfil::detail::mp_key key)
        : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key) {}
    Feature(Data& d,
            simfil::ModelConstPtr l,
            simfil::ModelNodeAddress a,
            simfil::detail::mp_key key);
    Feature() = delete;

protected:
    Data* data_ = nullptr;
    TileFeatureLayer const* extensionModel_ = nullptr;
    simfil::ModelNodeAddress extensionAddress_;

    // We keep the fields in a tiny vector on the stack,
    // because their number is dynamic, as a variable number
    // of id-part fields is adopted from the feature id.
    sfl::small_vector<std::pair<simfil::StringId, simfil::ModelNode::Ptr>, 32> fields_;
    void updateFields();
    void materializeGeometryCollection();
    model_ptr<Geometry> appendGeometry(GeomType type, size_t initialCapacity);

    struct FeaturePropertyView : public simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
    {
        [[nodiscard]] simfil::ValueType type() const override;
        [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
        [[nodiscard]] uint32_t size() const override;
        [[nodiscard]] ModelNode::Ptr get(const simfil::StringId&) const override;
        [[nodiscard]] simfil::StringId keyAt(int64_t) const override;
        [[nodiscard]] bool iterate(IterCallback const& cb) const override;

        explicit FeaturePropertyView(simfil::detail::mp_key key)
            : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(key) {}
        FeaturePropertyView(model_ptr<Feature> feature,
                            simfil::detail::mp_key key);
        FeaturePropertyView() = delete;

        Data* data_ = nullptr;
        model_ptr<Object> attrs_;
    };
};

}  // namespace mapget
