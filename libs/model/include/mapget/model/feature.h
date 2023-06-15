#pragma once

#include "featureid.h"
#include "attrlayer.h"
#include "attr.h"

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
 *     children: [<child-id-list>]
 *   }
 */
class Feature : protected simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>
{
    friend class bitsery::Access;
    friend class TileFeatureLayer;
    friend ModelNode::Ptr;

public:
    [[nodiscard]] std::string_view typeId() const;

    [[nodiscard]] model_ptr<FeatureId> id() const;
    [[nodiscard]] model_ptr<GeometryCollection> geom();
    [[nodiscard]] model_ptr<AttributeLayerList> attributeLayers();
    [[nodiscard]] model_ptr<Object> attributes();
    [[nodiscard]] model_ptr<Array> children();

    /**
     * Evaluate a filter expression on this feature.
     */
    std::vector<simfil::Value> evaluateAll(std::string_view const& expression);
    simfil::Value evaluate(std::string_view const& expression);

    /**
     * Convert the Feature to GeoJSON.
     */
    nlohmann::json toGeoJson();


protected:
    /**
     * Simfil Model-Node Functions
     */
    [[nodiscard]] simfil::ValueType type() const override;
    [[nodiscard]] ModelNode::Ptr at(int64_t) const override;
    [[nodiscard]] uint32_t size() const override;
    [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
    [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
    [[nodiscard]] bool iterate(IterCallback const& cb) const override;

    /**
     * Feature Data
     */
    struct Data
    {
        simfil::ModelNodeAddress id_;
        simfil::ModelNodeAddress geom_;
        simfil::ModelNodeAddress attrLayers_;
        simfil::ModelNodeAddress attrs_;
        simfil::ModelNodeAddress children_;

        template<typename S>
        void serialize(S& s) {
            s.value4b(id_.value_);
            s.value4b(geom_.value_);
            s.value4b(attrLayers_.value_);
            s.value4b(attrs_.value_);
            s.value4b(children_.value_);
        }
    };

    Feature(Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

    Data& data_;

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
        [[nodiscard]] ModelNode::Ptr get(const simfil::FieldId &) const override;
        [[nodiscard]] simfil::FieldId keyAt(int64_t) const override;
        [[nodiscard]] bool iterate(IterCallback const& cb) const override;

        FeaturePropertyView(Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a);

        Data& data_;
        std::optional<model_ptr<Object>> attrs_;
    };
};

}