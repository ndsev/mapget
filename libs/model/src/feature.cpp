#include "feature.h"
#include "featureid.h"
#include "featurelayer.h"
#include "geometry.h"
#include "relation.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "stringpool.h"
#include "tl/expected.hpp"

#include <stdexcept>

namespace mapget
{

uint32_t RelationArrayView::localMergedSize() const
{
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    if (auto rel = feature->relationsOrNull()) {
        return rel->size();
    }
    return 0;
}

simfil::ModelNode::Ptr RelationArrayView::localMergedAt(int64_t i) const
{
    if (i < 0) {
        return {};
    }
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    auto rel = feature->relationsOrNull();
    if (!rel || i >= static_cast<int64_t>(rel->size())) {
        return {};
    }
    return rel->at(i);
}

bool RelationArrayView::localMergedIterate(simfil::ModelNode::IterCallback const& cb) const
{
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    if (auto rel = feature->relationsOrNull()) {
        return rel->iterate(cb);
    }
    return true;
}

Feature::Feature(Feature::Data& d,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(l), a, key),
      data_(&d)
{
    updateFields();
}

model_ptr<FeatureId> Feature::id() const
{
    return model().resolve<FeatureId>(data_->id_);
}

std::string_view mapget::Feature::typeId() const
{
    return model().resolve<FeatureId>(data_->id_)->typeId();
}

model_ptr<GeometryCollection> Feature::geom()
{
    if (!data_->geom_) {
        auto result = model().newGeometryCollection();
        data_->geom_ = result->addr();
        updateFields();
        return result;
    }
    materializeGeometryCollection();
    return const_cast<const Feature*>(this)->geomOrNull();
}

model_ptr<GeometryCollection> Feature::geomOrNull() const
{
    model_ptr<GeometryCollection> local;
    if (data_->geom_) {
        local = model().resolve<GeometryCollection>(data_->geom_);
    }

    auto extFeature = extension();
    auto ext = extFeature ? extFeature->geomOrNull() : model_ptr<GeometryCollection>{};
    if (!local) {
        return ext;
    }
    local->setExtension(ext);
    return local;
}

model_ptr<AttributeLayerList> Feature::attributeLayers()
{
    if (!data_->attrLayers_) {
        auto result = model().newAttributeLayers();
        data_->attrLayers_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->attributeLayersOrNull();
}

model_ptr<AttributeLayerList> Feature::attributeLayersOrNull() const
{
    model_ptr<AttributeLayerList> local;
    if (data_->attrLayers_) {
        local = model().resolve<AttributeLayerList>(data_->attrLayers_);
    }

    auto extFeature = extension();
    auto ext = extFeature ? extFeature->attributeLayersOrNull() : model_ptr<AttributeLayerList>{};
    if (!local) {
        return ext;
    }
    local->setExtension(ext);
    return local;
}

model_ptr<Object> Feature::attributes()
{
    if (!data_->attrs_) {
        auto result = model().newObject(8);
        data_->attrs_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->attributesOrNull();
}

model_ptr<Object> Feature::attributesOrNull() const
{
    if (!data_->attrs_)
        return {};
    return model().resolve<simfil::Object>(data_->attrs_);
}

model_ptr<Array> Feature::relations()
{
    if (!data_->relations_) {
        auto result = model().newArray(8);
        data_->relations_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->relationsOrNull();
}

model_ptr<Array> Feature::relationsOrNull() const
{
    if (!data_->relations_)
        return {};
    return model().resolve<simfil::Array>(data_->relations_);
}

model_ptr<RelationArrayView> Feature::mergedRelationsOrNull() const
{
    auto extFeature = extension();
    auto ext = extFeature ? extFeature->mergedRelationsOrNull() : model_ptr<RelationArrayView>{};
    if (!data_->relations_ && !ext) {
        return {};
    }
    auto result = model_ptr<RelationArrayView>::make(
        model_,
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::FeatureRelationsView, addr().index()});
    result->setExtension(ext);
    return result;
}

tl::expected<std::vector<simfil::Value>, simfil::Error>
Feature::evaluateAll(const std::string_view& expression)
{
    // Note: Here we rely on the assertion that the root_ column
    // contains only references to feature nodes, in the order
    // of the feature node column. We could think about protected inheritance
    // of the ModelPool to safeguard this.
    auto result = model().evaluate(expression, *this, false);
    if (!result)
        tl::unexpected<simfil::Error>(std::move(result.error()));

    return result->values;
}

tl::expected<simfil::Value, simfil::Error> Feature::evaluate(const std::string_view& expression)
{
    auto results = evaluateAll(expression);
    if (!results)
        return tl::unexpected<simfil::Error>(std::move(results.error()));

    if (results->empty())
        return simfil::Value::null();

    return std::move((*results)[0]);
}

simfil::ValueType Feature::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Feature::at(int64_t i) const
{
    if (data_->sourceData_) {
        if (i == 0)
            return get(StringPool::SourceDataStr);
        i -= 1;
    }
    if (i < fields_.size())
        return fields_[i].second;
    return {};
}

uint32_t Feature::size() const
{
    return fields_.size() + (data_->sourceData_ ? 1 : 0);
}

simfil::ModelNode::Ptr Feature::get(const simfil::StringId& f) const
{
    if (f == StringPool::SourceDataStr)
        return model().resolve(data_->sourceData_);

    for (auto const& [fieldName, fieldValue] : fields_)
        if (fieldName == f)
            return fieldValue;

    return {};
}

simfil::StringId Feature::keyAt(int64_t i) const
{
    if (data_->sourceData_) {
        if (i == 0)
            return StringPool::SourceDataStr;
        i -= 1;
    }
    if (i < fields_.size())
        return fields_[i].first;
    return {};
}

bool Feature::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;

    return true;
}

void Feature::updateFields() {
    fields_.clear();

    // Add type field
    fields_.emplace_back(
        StringPool::TypeStr,
        simfil::model_ptr<simfil::ValueNode>::make(std::string_view("Feature"), model_));

    // Add id field
    fields_.emplace_back(StringPool::IdStr, Ptr::make(model_, data_->id_));
    auto idNode = model().resolve<FeatureId>(*fields_.back().second);

    // Add type id field
    fields_.emplace_back(
        StringPool::TypeIdStr,
        model_ptr<simfil::ValueNode>::make(idNode->typeId(), model_));

    // Add map and layer ids.
    fields_.emplace_back(
        StringPool::MapIdStr,
        model_ptr<simfil::ValueNode>::make(model().mapId(), model_));
    fields_.emplace_back(
        StringPool::LayerIdStr,
        model_ptr<simfil::ValueNode>::make(model().layerInfo()->layerId_, model_));

    // Add common id-part fields
    if (auto idPrefix = model().getIdPrefix()) {
        for (auto const& [idPartName, value] : idPrefix->fields()) {
            fields_.emplace_back(idPartName, value);
        }
    }

    // Add feature-specific id-part fields
    for (auto const& [idPartName, value] : idNode->fields()) {
        fields_.emplace_back(idPartName, value);
    }

    // Add other fields
    if (auto geomNode = geomOrNull()) {
        fields_.emplace_back(StringPool::GeometryStr, geomNode);
    }
    bool hasExtensionProperties = false;
    if (auto extFeature = extension()) {
        hasExtensionProperties = extFeature->data_->attrLayers_ || extFeature->data_->attrs_;
    }
    if (data_->attrLayers_ || data_->attrs_ || hasExtensionProperties)
        fields_.emplace_back(
            StringPool::PropertiesStr,
            Ptr::make(
                model_,
                simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::FeatureProperties, addr().index()}));
    if (auto rel = mergedRelationsOrNull()) {
        fields_.emplace_back(StringPool::RelationsStr, rel);
    }
}

nlohmann::json Feature::toJson() const
{
    return simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>::toJson();
}

void Feature::addPoint(const Point& p) {
    auto newGeom = appendGeometry(GeomType::Points, 0);
    newGeom->append(p);
}

void Feature::addPoints(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Points, points.size() - 1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addLine(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Line, points.size() - 1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addMesh(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Mesh, points.size() - 1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addPoly(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Polygon, points.size() - 1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::materializeGeometryCollection()
{
    if (!data_->geom_ || data_->geom_.column() != TileFeatureLayer::ColumnId::Geometries) {
        return;
    }
    auto existingGeometry = model().resolve<Geometry>(data_->geom_);
    auto collection = model().newGeometryCollection(2);
    collection->addGeometry(existingGeometry);
    data_->geom_ = collection->addr();
    updateFields();
}

model_ptr<Geometry> Feature::appendGeometry(GeomType type, size_t initialCapacity)
{
    if (!data_->geom_) {
        auto geom = model().newGeometry(type, initialCapacity);
        data_->geom_ = geom->addr();
        updateFields();
        return geom;
    }

    materializeGeometryCollection();
    if (data_->geom_.column() != TileFeatureLayer::ColumnId::GeometryCollections) {
        simfil::raise<std::runtime_error>(
            "Feature geometry reference is neither Geometry nor GeometryCollection.");
    }

    auto collection = model().resolve<GeometryCollection>(data_->geom_);
    auto geom = model().newGeometry(type, initialCapacity);
    collection->addGeometry(geom);
    return geom;
}

model_ptr<Relation> Feature::addRelation(
    const std::string_view& name,
    const std::string_view& targetType,
    const KeyValueViewPairs& targetIdParts)
{
    return addRelation(name, model().newFeatureId(targetType, targetIdParts));
}

model_ptr<Relation> Feature::addRelation(const std::string_view& name, const model_ptr<FeatureId>& target)
{
    return addRelation(model().newRelation(name, target));
}

model_ptr<Relation> Feature::addRelation(const model_ptr<Relation>& relation)
{
    relations()->append(relation);
    return relation;
}

uint32_t Feature::numRelations() const
{
    auto localCount = data_->relations_ ? relationsOrNull()->size() : 0U;
    if (auto extFeature = extension()) {
        localCount += extFeature->numRelations();
    }
    return localCount;
}

model_ptr<Relation> Feature::getRelation(uint32_t index) const
{
    if (data_->relations_) {
        auto localRelations = relationsOrNull();
        auto localCount = localRelations->size();
        if (index < localCount) {
            return model().resolve<Relation>(*localRelations->at(index));
        }
        index -= localCount;
    }

    if (auto extFeature = extension()) {
        return extFeature->getRelation(index);
    }
    return {};
}

bool Feature::forEachRelation(std::function<bool(const model_ptr<Relation>&)> const& callback) const
{
    if (!callback)
        return true;

    if (data_->relations_) {
        auto relationsPtr = relationsOrNull();
        for (auto const& relation : *relationsPtr) {
            if (!callback(model().resolve<Relation>(*relation)))
                return false;
        }
    }

    if (auto extFeature = extension()) {
        if (!extFeature->forEachRelation(callback)) {
            return false;
        }
    }
    return true;
}

SelfContainedGeometry Feature::firstGeometry() const
{
    model_ptr<Geometry> result;
    if (auto geometryCollection = geomOrNull()) {
        geometryCollection->forEachGeometry(
            [&result](auto&& geometry)
            {
                result = geometry;
                return false;
            });
    }
    if (result)
        return result->toSelfContained();
    return {};
}

std::optional<std::vector<model_ptr<Relation>>>
Feature::filterRelations(const std::string_view& name) const
{
    std::vector<model_ptr<Relation>> result;
    result.reserve(numRelations());

    forEachRelation([&name, &result](auto&& rel){
        if (rel->name() == name)
            result.push_back(rel);
        return true;
    });

    if (result.empty())
        return {};
    return result;
}

model_ptr<SourceDataReferenceCollection> Feature::sourceDataReferences() const
{
    if (data_->sourceData_)
        return model().resolve<SourceDataReferenceCollection>(
            *model_ptr<simfil::ModelNode>::make(model_, data_->sourceData_));
    return {};
}

void Feature::setSourceDataReferences(simfil::ModelNode::Ptr const& addresses)
{
    data_->sourceData_ = addresses->addr();
}

model_ptr<Feature> Feature::extension() const
{
    if (!extensionModel_ || !extensionAddress_) {
        return {};
    }
    return extensionModel_->resolve<Feature>(extensionAddress_);
}

void Feature::setExtension(model_ptr<Feature> extension)
{
    if (!extension) {
        extensionModel_ = nullptr;
        extensionAddress_ = {};
        updateFields();
        return;
    }
    extensionModel_ = &extension->model();
    extensionAddress_ = extension->addr();
    updateFields();
}

//////////////////////////////////////////

Feature::FeaturePropertyView::FeaturePropertyView(
    model_ptr<Feature> feature,
    simfil::detail::mp_key key
)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
        feature->model().shared_from_this(),
        feature->addr(),
        key),
      data_(feature->data_)
{
    if (data_->attrs_)
        attrs_ = feature->attributesOrNull();
}

simfil::ValueType Feature::FeaturePropertyView::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::at(int64_t i) const
{
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    auto mergedLayers = feature->attributeLayersOrNull();
    if (mergedLayers) {
        if (i == 0)
            return mergedLayers;
        i -= 1;
    }
    if (attrs_)
        return attrs_->at(i);
    return {};
}

uint32_t Feature::FeaturePropertyView::size() const
{
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    return (feature->attributeLayersOrNull() ? 1 : 0) + (attrs_ ? attrs_->size() : 0);
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::get(const simfil::StringId& f) const
{
    if (f == StringPool::LayerStr) {
        auto feature = model().resolve<Feature>(
            simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
        auto mergedLayers = feature->attributeLayersOrNull();
        if (mergedLayers) {
            return mergedLayers;
        }
    }
    if (attrs_)
        return attrs_->get(f);
    return {};
}

simfil::StringId Feature::FeaturePropertyView::keyAt(int64_t i) const
{
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    if (feature->attributeLayersOrNull()) {
        if (i == 0)
            return StringPool::LayerStr;
        i -= 1;
    }
    if (attrs_)
        return attrs_->keyAt(i);
    return {};
}

bool Feature::FeaturePropertyView::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    auto feature = model().resolve<Feature>(
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::Features, addr().index()});
    if (auto mergedLayers = feature->attributeLayersOrNull()) {
        if (!cb(*mergedLayers))
            return false;
    }
    if (attrs_)
        return attrs_->iterate(cb);
    return true;
}

}
