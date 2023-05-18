#include "featurelayer.h"

namespace mapget
{

struct TileFeatureLayer::Impl {
    simfil::ModelNodeAddress featureIdPrefix_;

    sfl::segmented_vector<Feature::Data, simfil::detail::ColumnPageSize> features_;
    sfl::segmented_vector<Attribute::Data, simfil::detail::ColumnPageSize> features_;
};

TileFeatureLayer::TileFeatureLayer(
    TileId tileId,
    std::string const& nodeId,
    std::string const& mapId,
    std::shared_ptr<LayerInfo> const& layerInfo,
    KeyValuePairs const& featureIdPrefix_,
    std::shared_ptr<Fields> fields
) :
    impl_(std::make_unique<Impl>()),
    TileLayer(tileId, nodeId, mapId, layerInfo),
    simfil::ModelPool(std::move(fields))
{
    auto idPrefix = newObject(featureIdPrefix_.size());
    for (auto const& [k, v] : featureIdPrefix_) {
        auto&& kk = k;
        std::visit([&](auto&& x){
            idPrefix->addField(kk, x);
        }, v);
    }
}

TileFeatureLayer::~TileFeatureLayer() {}  // NOLINT

simfil::shared_model_ptr<Feature> TileFeatureLayer::newFeature(
    const std::string_view& typeId,
    const KeyValuePairs& featureId)
{
    return model_ptr<Feature>::make();
}

model_ptr<Object> TileFeatureLayer::featureIdPrefix()
{
    return mapget::model_ptr<Object>();
}

}