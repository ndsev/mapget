#include "feature.h"
#include "featureid.h"
#include "featurelayer.h"
#include "geometry.h"
#include "mapget/log.h"
#include "relation.h"
#include "simfil/model/nodes.h"
#include "simfil/model/string-pool.h"
#include "stringpool.h"
#include "tl/expected.hpp"

#include <algorithm>
#include <stdexcept>

namespace mapget
{

namespace
{
model_ptr<Feature> resolveFeatureByRootIndex(TileFeatureLayer const& model, uint32_t index)
{
    auto rootResult = model.root(index);
    if (!rootResult || !*rootResult) {
        return {};
    }
    return model.resolve<Feature>(**rootResult);
}
}

RelationArrayView::ExtensionPtr RelationArrayView::mergedExtension() const
{
    return {};
}

uint32_t RelationArrayView::localMergedSize() const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return 0;
    }
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
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return {};
    }
    auto rel = feature->relationsOrNull();
    if (!rel || i >= static_cast<int64_t>(rel->size())) {
        return {};
    }
    return rel->at(i);
}

bool RelationArrayView::localMergedIterate(simfil::ModelNode::IterCallback const& cb) const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return true;
    }
    if (auto rel = feature->relationsOrNull()) {
        return rel->iterate(cb);
    }
    return true;
}

Feature::Feature(Feature::BasicData& d,
    Feature::ComplexData* c,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(l), a, key),
      basicData_(&d),
      complexData_(c)
{
    fieldsDirty_ = true;
}

model_ptr<FeatureId> Feature::id() const
{
    auto featureIdAddress = featureIdNodeAddress();
    if (featureIdAddress) {
        return model().resolve<FeatureId>(featureIdAddress);
    }
    return {};
}

std::string_view mapget::Feature::typeId() const
{
    if (basicData_) {
        if (auto s = model().strings()->resolve(basicData_->typeId_.typeId_))
            return *s;
    }
    return id()->typeId();
}

model_ptr<GeometryCollection> Feature::geom()
{
    auto& geomAddress = geometryNodeAddress();
    if (!geomAddress) {
        auto result = model().newGeometryCollection();
        geomAddress = result->addr();
        fieldsDirty_ = true;
        return result;
    }
    materializeGeometryCollection();
    return model().resolve<GeometryCollection>(geometryNodeAddress());
}

model_ptr<GeometryCollection> Feature::geomOrNull() const
{
    if (!geometryNodeAddress()) {
        return {};
    }
    return model().resolve<GeometryCollection>(geometryNodeAddress());
}

model_ptr<AttributeLayerList> Feature::attributeLayers()
{
    if (!attributeLayerNodeAddress()) {
        auto result = model().newAttributeLayers();
        if (auto schemaId = model().attributeLayerMapSchemaId(typeId());
            schemaId != simfil::NoSchemaId) {
            if (auto schemaResult = result->setObjectSchema(schemaId); !schemaResult) {
                log().warn("Failed to set attribute-layer-list schema: {}", schemaResult.error().message);
            }
        }
        attributeLayerNodeAddress() = result->addr();
        fieldsDirty_ = true;
        return result;
    }
    return model().resolve<AttributeLayerList>(attributeLayerNodeAddress());
}

model_ptr<AttributeLayerList> Feature::attributeLayersOrNull() const
{
    if (!attributeLayerNodeAddress()) {
        return {};
    }
    return model().resolve<AttributeLayerList>(attributeLayerNodeAddress());
}

model_ptr<Object> Feature::attributes()
{
    if (!attributeNodeAddress()) {
        auto result = model().newObject(8);
        model().applyObjectSchema(*result, model().featurePropertiesSchemaId(typeId()));
        attributeNodeAddress() = result->addr();
        fieldsDirty_ = true;
        return result;
    }
    return attributesOrNull();
}

model_ptr<Object> Feature::attributesOrNull() const
{
    auto localAddress = attributeNodeAddress();
    if (!localAddress)
        return {};
    return model().resolve<simfil::Object>(localAddress);
}

model_ptr<Feature::MergedBasicAttributesView> Feature::mergedAttributesOrNull() const
{
    if (!attributeNodeAddress()) {
        return {};
    }
    return model_ptr<MergedBasicAttributesView>::make(model_, addr());
}

model_ptr<Array> Feature::relations()
{
    if (!relationNodeAddress()) {
        auto result = model().newArray(8);
        model().applyArraySchema(
            *result,
            model().childSchemaId(
                schema(),
                StringPool::RelationsStr,
                simfil::Schema::Kind::Array));
        relationNodeAddress() = result->addr();
        fieldsDirty_ = true;
        return result;
    }
    return const_cast<const Feature*>(this)->relationsOrNull();
}

model_ptr<Array> Feature::relationsOrNull() const
{
    auto localAddress = relationNodeAddress();
    if (!localAddress)
        return {};
    return model().resolve<simfil::Array>(localAddress);
}

model_ptr<RelationArrayView> Feature::mergedRelationsOrNull() const
{
    if (!relationNodeAddress()) {
        return {};
    }

    return model_ptr<RelationArrayView>::make(
        model_,
        simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::FeatureRelationsView, addr().index()});
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

simfil::SchemaId Feature::schema() const
{
    return model().featureSchemaId(typeId());
}

simfil::ModelNode::Ptr Feature::at(int64_t i) const
{
    ensureFieldsReady();
    auto sourceDataAddress = sourceDataNodeAddress();
    if (sourceDataAddress) {
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
    ensureFieldsReady();
    return fields_.size() + (sourceDataNodeAddress() ? 1 : 0);
}

simfil::ModelNode::Ptr Feature::get(const simfil::StringId& f) const
{
    ensureFieldsReady();
    if (f == StringPool::SourceDataStr) {
        auto sourceDataAddress = sourceDataNodeAddress();
        if (sourceDataAddress) {
            return model().resolve(sourceDataAddress);
        }
    }

    for (auto const& [fieldName, fieldValue] : fields_)
        if (fieldName == f)
            return fieldValue;

    if (f == StringPool::AttributesStr) {
        // Preserve real top-level fields named `attributes`, but otherwise
        // allow query/import callers to use it as an alias for `properties`.
        for (auto const& [fieldName, fieldValue] : fields_)
            if (fieldName == StringPool::PropertiesStr)
                return fieldValue;
    }

    return {};
}

simfil::StringId Feature::keyAt(int64_t i) const
{
    ensureFieldsReady();
    if (sourceDataNodeAddress()) {
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
    ensureFieldsReady();
    for (auto i = 0; i < size(); ++i)
        if (!cb(*at(i)))
            return false;

    return true;
}

simfil::ModelNodeAddress Feature::featureIdNodeAddress() const
{
    using Col = TileFeatureLayer::ColumnId;
    if (!basicData_) {
        return {};
    }
    return {Col::FeatureIds, addr_.index()};
}

simfil::ModelNodeAddress Feature::sourceDataNodeAddress() const
{
    if (!complexData_ && basicData_) {
        complexData_ = model().featureComplexDataOrNull(addr().index());
    }
    if (complexData_) {
        return complexData_->sourceData_;
    }
    return {};
}

simfil::ModelNodeAddress Feature::geometryNodeAddress() const
{
    return basicData_ ? basicData_->geom_ : simfil::ModelNodeAddress{};
}

simfil::ModelNodeAddress& Feature::geometryNodeAddress()
{
    if (!basicData_) {
        throw std::runtime_error("Feature has no mutable geometry storage.");
    }
    return basicData_->geom_;
}

simfil::ModelNodeAddress Feature::attributeLayerNodeAddress() const
{
    if (!complexData_ && basicData_) {
        complexData_ = model().featureComplexDataOrNull(addr().index());
    }
    if (complexData_) {
        return complexData_->attrLayers_;
    }
    return {};
}

simfil::ModelNodeAddress& Feature::attributeLayerNodeAddress()
{
    if (!basicData_) {
        throw std::runtime_error("Feature has no mutable attribute-layer storage.");
    }
    if (!complexData_) {
        complexData_ = &model().ensureFeatureComplexData(addr().index());
    }
    return complexData_->attrLayers_;
}

simfil::ModelNodeAddress Feature::attributeNodeAddress() const
{
    if (!complexData_ && basicData_) {
        complexData_ = model().featureComplexDataOrNull(addr().index());
    }
    if (complexData_) {
        return complexData_->attrs_;
    }
    return {};
}

simfil::ModelNodeAddress& Feature::attributeNodeAddress()
{
    if (!basicData_) {
        throw std::runtime_error("Feature has no mutable attribute storage.");
    }
    if (!complexData_) {
        complexData_ = &model().ensureFeatureComplexData(addr().index());
    }
    return complexData_->attrs_;
}

simfil::ModelNodeAddress Feature::relationNodeAddress() const
{
    if (!complexData_ && basicData_) {
        complexData_ = model().featureComplexDataOrNull(addr().index());
    }
    if (complexData_) {
        return complexData_->relations_;
    }
    return {};
}

simfil::ModelNodeAddress& Feature::relationNodeAddress()
{
    if (!basicData_) {
        throw std::runtime_error("Feature has no mutable relation storage.");
    }
    if (!complexData_) {
        complexData_ = &model().ensureFeatureComplexData(addr().index());
    }
    return complexData_->relations_;
}

void Feature::updateFields() const {
    fields_.clear();

    // Add type field
    fields_.emplace_back(
        StringPool::TypeStr,
        simfil::model_ptr<simfil::ValueNode>::make(std::string_view("Feature"), model_));

    // Add id field
    fields_.emplace_back(StringPool::IdStr, Ptr::make(model_, featureIdNodeAddress()));
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

    // Keep regular feature ids scalar in the node protocol. Feature export still
    // needs the explicit id-part fields, so materialize them from the typed
    // feature-id storage instead of relying on object-style child traversal.
    if (idNode->values_) {
        idNode->ensureVisiblePartLayout();
        auto const limit = std::min(idNode->partNames_.size(), idNode->visibleValueIndices_.size());
        for (size_t i = 0; i < limit; ++i) {
            if (auto value = idNode->values_->at(static_cast<int64_t>(idNode->visibleValueIndices_[i]))) {
                fields_.emplace_back(idNode->partNames_[i], value);
            }
        }
    }

    // Add other fields
    if (auto geomNode = geomOrNull()) {
        fields_.emplace_back(StringPool::GeometryStr, geomNode);
    }
    auto const localAttrLayerAddress = attributeLayerNodeAddress();
    auto const localAttrAddress = attributeNodeAddress();
    if (localAttrLayerAddress || localAttrAddress)
        fields_.emplace_back(
            StringPool::PropertiesStr,
            Ptr::make(
                model_,
                simfil::ModelNodeAddress{TileFeatureLayer::ColumnId::FeatureProperties, addr().index()}));
    if (auto rel = mergedRelationsOrNull()) {
        fields_.emplace_back(StringPool::RelationsStr, rel);
    }
    fieldsDirty_ = false;
}

nlohmann::json Feature::toJson() const
{
    auto json = simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>::toJson();
    if (auto layers = attributeLayersOrNull()) {
        json["properties"]["layer"] = layers->toJson();
    }
    return json;
}

void Feature::addPoint(const Point& p) {
    auto newGeom = appendGeometry(GeomType::Points, 1, true);
    newGeom->append(p);
}

void Feature::addGeometry(const model_ptr<Geometry>& geom)
{
    if (!geom) {
        return;
    }

    auto& geomAddress = geometryNodeAddress();
    if (!geomAddress) {
        geomAddress = geom->addr();
        fieldsDirty_ = true;
        return;
    }

    materializeGeometryCollection();
    if (geometryNodeAddress().column() != TileFeatureLayer::ColumnId::GeometryCollections) {
        simfil::raise<std::runtime_error>(
            "Feature geometry reference is neither Geometry nor GeometryCollection.");
    }

    auto collection = model().resolve<GeometryCollection>(geometryNodeAddress());
    collection->addGeometry(geom);
}

void Feature::addPoints(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Points, points.size());
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addLine(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Line, points.size());
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addMesh(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Mesh, points.size());
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addPoly(const std::vector<Point>& points) {
    auto newGeom = appendGeometry(GeomType::Polygon, points.size());
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::materializeGeometryCollection()
{
    auto const isBaseGeometryColumn = [](uint8_t column) {
        using Col = TileFeatureLayer::ColumnId;
        return column == Col::PointGeometries ||
               column == Col::LineGeometries ||
               column == Col::PolygonGeometries ||
               column == Col::MeshGeometries ||
               column == Col::AabbGeometries ||
               column == Col::GltfNodeIndexGeometries;
    };
    auto currentGeomAddress = geometryNodeAddress();
    if (!currentGeomAddress ||
        (!isBaseGeometryColumn(currentGeomAddress.column()) &&
         currentGeomAddress.column() != TileFeatureLayer::ColumnId::GeometryViews)) {
        return;
    }
    auto existingGeometry = model().resolve<Geometry>(currentGeomAddress);
    auto collection = model().newGeometryCollection(2);
    collection->addGeometry(existingGeometry);
    geometryNodeAddress() = collection->addr();
    fieldsDirty_ = true;
}

model_ptr<Geometry> Feature::appendGeometry(
    GeomType type,
    size_t initialCapacity,
    bool fixedSize)
{
    auto& geomAddress = geometryNodeAddress();
    if (!geomAddress) {
        auto geom = model().newGeometry(type, initialCapacity, fixedSize);
        geomAddress = geom->addr();
        fieldsDirty_ = true;
        return geom;
    }

    materializeGeometryCollection();
    if (geometryNodeAddress().column() != TileFeatureLayer::ColumnId::GeometryCollections) {
        simfil::raise<std::runtime_error>(
            "Feature geometry reference is neither Geometry nor GeometryCollection.");
    }

    auto collection = model().resolve<GeometryCollection>(geometryNodeAddress());
    auto geom = model().newGeometry(type, initialCapacity, fixedSize);
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

model_ptr<Relation> Feature::addRelation(model_ptr<Relation> relation)
{
    if (!relation) {
        raise("Cannot add null relation.");
    }
    if (relation->addr().column() != TileFeatureLayer::ColumnId::Relations) {
        raise("Feature relations must be canonical Relation nodes.");
    }
    if (relation->owningModel().get() != &model()) {
        raise("Feature relations must belong to the same TileFeatureLayer.");
    }

    // Re-resolve by address before mutation because previously returned
    // Relation wrappers can hold stale vector element pointers after growth.
    relation = model().resolve<Relation>(relation->addr());

    auto rels = relations();
    auto const relationIndex = rels->size();
    if (relationIndex >= Relation::InvalidFeatureRelationIndex) {
        raise("Feature relation index exceeds RelationReference JSON range.");
    }
    // RelationReference JSON intentionally uses this feature-local ordinal.
    relation->setFeatureRelationIndex(static_cast<uint16_t>(relationIndex));
    rels->append(relation);
    return relation;
}

uint32_t Feature::numRelations() const
{
    return relationNodeAddress() ? relationsOrNull()->size() : 0U;
}

model_ptr<Relation> Feature::getRelation(uint32_t index) const
{
    if (relationNodeAddress()) {
        auto localRelations = relationsOrNull();
        auto localCount = localRelations->size();
        if (index < localCount) {
            return model().resolve<Relation>(*localRelations->at(index));
        }
        index -= localCount;
    }

    return {};
}

bool Feature::forEachRelation(std::function<bool(const model_ptr<Relation>&)> const& callback) const
{
    if (!callback)
        return true;

    if (relationNodeAddress()) {
        auto relationsPtr = relationsOrNull();
        for (auto const& relation : *relationsPtr) {
            if (!callback(model().resolve<Relation>(*relation)))
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

SelfContainedGeometry Feature::preferredGeometry() const
{
    // Presentation fidelity no longer selects geometry in the model.
    return firstGeometry();
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
    if (auto sourceDataAddress = sourceDataNodeAddress())
        return model().resolve<SourceDataReferenceCollection>(
            *model_ptr<simfil::ModelNode>::make(model_, sourceDataAddress));
    return {};
}

void Feature::setSourceDataReferences(simfil::ModelNode::Ptr const& addresses)
{
    if (!basicData_) {
        throw std::runtime_error("Cannot attach source-data references to a feature without basic storage.");
    }
    if (!complexData_) {
        complexData_ = &model().ensureFeatureComplexData(addr().index());
    }
    complexData_->sourceData_ = addresses->addr();
}

void Feature::ensureFieldsReady() const
{
    if (!fieldsDirty_) {
        return;
    }
    updateFields();
}

//////////////////////////////////////////

Feature::MergedBasicAttributesView::MergedBasicAttributesView(
    simfil::ModelConstPtr model,
    simfil::ModelNodeAddress address,
    simfil::detail::mp_key key)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(model), address, key)
{
}

void Feature::MergedBasicAttributesView::ensureMergedFieldsReady() const
{
    if (!mergedFieldsDirty_) {
        return;
    }
    rebuildMergedFields();
}

void Feature::MergedBasicAttributesView::rebuildMergedFields() const
{
    mergedFields_.clear();

    model_ptr<Feature> feature;
    if (addr().column() == TileFeatureLayer::ColumnId::Features) {
        feature = model().resolve<Feature>(addr());
    }
    else {
        auto rootResult = model().root(addr().index());
        if (rootResult && *rootResult) {
            feature = model().resolve<Feature>(**rootResult);
        }
    }

    if (feature) {
        if (auto attrs = feature->attributesOrNull()) {
            mergedFields_.reserve(attrs->size());
            for (auto i = 0U; i < attrs->size(); ++i) {
                mergedFields_.emplace_back(
                    attrs->keyAt(static_cast<int64_t>(i)),
                    attrs->at(static_cast<int64_t>(i)));
            }
        }

    }

    mergedFieldsDirty_ = false;
}

simfil::ValueType Feature::MergedBasicAttributesView::type() const
{
    return simfil::ValueType::Object;
}

simfil::SchemaId Feature::MergedBasicAttributesView::schema() const
{
    if (auto feature = resolveFeatureByRootIndex(model(), addr().index())) {
        return model().featurePropertiesSchemaId(feature->typeId());
    }
    return simfil::NoSchemaId;
}

simfil::ModelNode::Ptr Feature::MergedBasicAttributesView::at(int64_t i) const
{
    ensureMergedFieldsReady();
    if (i < 0 || i >= static_cast<int64_t>(mergedFields_.size())) {
        return {};
    }
    return mergedFields_[static_cast<size_t>(i)].second;
}

uint32_t Feature::MergedBasicAttributesView::size() const
{
    ensureMergedFieldsReady();
    return static_cast<uint32_t>(mergedFields_.size());
}

simfil::ModelNode::Ptr Feature::MergedBasicAttributesView::get(const simfil::StringId& f) const
{
    ensureMergedFieldsReady();
    auto it = std::find_if(
        mergedFields_.begin(),
        mergedFields_.end(),
        [&](const AttrField& field) { return field.first == f; });
    if (it == mergedFields_.end()) {
        return {};
    }
    return it->second;
}

simfil::StringId Feature::MergedBasicAttributesView::keyAt(int64_t i) const
{
    ensureMergedFieldsReady();
    if (i < 0 || i >= static_cast<int64_t>(mergedFields_.size())) {
        return {};
    }
    return mergedFields_[static_cast<size_t>(i)].first;
}

bool Feature::MergedBasicAttributesView::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    ensureMergedFieldsReady();
    for (auto const& [_, value] : mergedFields_) {
        if (!value || !cb(*value)) {
            return false;
        }
    }
    return true;
}

//////////////////////////////////////////

Feature::FeaturePropertyView::FeaturePropertyView(
    model_ptr<Feature> feature,
    simfil::detail::mp_key key
)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(
        feature->model().shared_from_this(),
        feature->addr(),
        key)
{}

simfil::ValueType Feature::FeaturePropertyView::type() const
{
    return simfil::ValueType::Object;
}

simfil::SchemaId Feature::FeaturePropertyView::schema() const
{
    if (auto feature = resolveFeatureByRootIndex(model(), addr().index())) {
        return model().featurePropertiesSchemaId(feature->typeId());
    }
    return simfil::NoSchemaId;
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::at(int64_t i) const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return {};
    }
    auto mergedLayers = feature->attributeLayersOrNull();
    if (mergedLayers) {
        if (i == 0)
            return mergedLayers;
        i -= 1;
    }
    if (auto mergedAttrs = feature->mergedAttributesOrNull()) {
        return mergedAttrs->at(i);
    }
    return {};
}

uint32_t Feature::FeaturePropertyView::size() const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return 0;
    }
    auto mergedAttrs = feature->mergedAttributesOrNull();
    return (feature->attributeLayersOrNull() ? 1 : 0) + (mergedAttrs ? mergedAttrs->size() : 0U);
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::get(const simfil::StringId& f) const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return {};
    }
    if (f == StringPool::LayerStr) {
        auto mergedLayers = feature->attributeLayersOrNull();
        if (mergedLayers) {
            return mergedLayers;
        }
    }
    if (auto mergedAttrs = feature->mergedAttributesOrNull()) {
        return mergedAttrs->get(f);
    }
    return {};
}

simfil::StringId Feature::FeaturePropertyView::keyAt(int64_t i) const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return {};
    }
    if (feature->attributeLayersOrNull()) {
        if (i == 0)
            return StringPool::LayerStr;
        i -= 1;
    }
    if (auto mergedAttrs = feature->mergedAttributesOrNull()) {
        return mergedAttrs->keyAt(i);
    }
    return {};
}

bool Feature::FeaturePropertyView::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    auto feature = resolveFeatureByRootIndex(model(), addr().index());
    if (!feature) {
        return true;
    }
    if (auto mergedLayers = feature->attributeLayersOrNull()) {
        if (!cb(*mergedLayers))
            return false;
    }
    if (auto mergedAttrs = feature->mergedAttributesOrNull()) {
        if (!mergedAttrs->iterate(cb)) {
            return false;
        }
    }
    return true;
}

}
