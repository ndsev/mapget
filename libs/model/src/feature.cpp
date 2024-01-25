#include "feature.h"
#include "featurelayer.h"

namespace mapget
{

Feature::Feature(Feature::Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(l), a), data_(&d)
{
    updateFields();
}

model_ptr<FeatureId> Feature::id() const
{
    return model().resolveFeatureId(*Ptr::make(model_, data_->id_));
}

std::string_view mapget::Feature::typeId() const
{
    return model().resolveFeatureId(*Ptr::make(model_, data_->id_))->typeId();
}

model_ptr<simfil::GeometryCollection> Feature::geom()
{
    if (!data_->geom_) {
        auto result = model().newGeometryCollection();
        data_->geom_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->geom();
}

model_ptr<GeometryCollection> Feature::geom() const
{
    if (!data_->geom_)
        return {};
    return model().resolveGeometryCollection(Ptr::make(model_, data_->geom_));
}

model_ptr<AttributeLayerList> Feature::attributeLayers()
{
    if (!data_->attrLayers_) {
        auto result = model().newAttributeLayers();
        data_->attrLayers_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->attributeLayers();
}

model_ptr<AttributeLayerList> Feature::attributeLayers() const
{
    if (!data_->attrLayers_)
        return {};
    return model().resolveAttributeLayerList(*Ptr::make(model_, data_->attrLayers_));
}

model_ptr<Object> Feature::attributes()
{
    if (!data_->attrs_) {
        auto result = model().newObject(8);
        data_->attrs_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->attributes();
}

model_ptr<Object> Feature::attributes() const
{
    if (!data_->attrs_)
        return {};
    return model().resolveObject(Ptr::make(model_, data_->attrs_));
}

model_ptr<Array> Feature::relations()
{
    if (!data_->relations_) {
        auto result = model().newArray(8);
        data_->relations_ = result->addr();
        updateFields();
        return result;
    }
    return const_cast<const Feature*>(this)->relations();
}

model_ptr<Array> Feature::relations() const
{
    if (!data_->relations_)
        return {};
    model().resolveArray(Ptr::make(model_, data_->relations_));
}

std::vector<simfil::Value> Feature::evaluateAll(const std::string_view& expression)
{
    // Note: Here we rely on the assertion that the root_ column
    // contains only references to feature nodes, in the order
    // of the feature node column. We could think about protected inheritance
    // of the ModelPool to safeguard this.
    return simfil::eval(
        model().evaluationEnvironment(),
        *model().compiledExpression(expression),
        model(),
        addr().index());
}

simfil::Value Feature::evaluate(const std::string_view& expression) {
    auto results = evaluateAll(expression);
    if (results.empty())
        return simfil::Value::null();
    return std::move(results[0]);
}

simfil::ValueType Feature::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Feature::at(int64_t i) const
{
    if (i < fields_.size())
        return fields_[i].second;
    return {};
}

uint32_t Feature::size() const
{
    return fields_.size();
}

simfil::ModelNode::Ptr Feature::get(const simfil::FieldId& f) const
{
    for (auto const& [fieldName, fieldValue] : fields_)
        if (fieldName == f)
            return fieldValue;
    return {};
}

simfil::FieldId Feature::keyAt(int64_t i) const
{
    if (i < fields_.size())
        return fields_[i].first;
    return {};
}

bool Feature::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    auto& resolver = model();
    auto resolveAndCb = [&cb, &resolver](auto&& nodeNameAndValue){
        bool cont = true;
        resolver.resolve(*nodeNameAndValue.second, simfil::Model::Lambda([&cont, &cb](auto&& resolved){
            cont = cb(resolved);
        }));
        return cont;
    };
    return std::all_of(fields_.begin(), fields_.end(), resolveAndCb);
}

void Feature::updateFields() {
    fields_.clear();

    // Add type field
    fields_.emplace_back(Fields::Type, simfil::ValueNode(std::string_view("Feature"), model_));

    // Add id field
    fields_.emplace_back(Fields::IdStr, Ptr::make(model_, data_->id_));
    auto idNode = model().resolveFeatureId(*fields_.back().second);

    // Add type id field
    fields_.emplace_back(
        Fields::TypeIdStr,
        model_ptr<simfil::ValueNode>::make(idNode->typeId(), model_));

    // Add common id-part fields
    if (model().featureIdPrefix())
        for (auto const& [idPartName, value] : model().featureIdPrefix()->fields()) {
            fields_.emplace_back(idPartName, value);
        }

    // Add feature-specific id-part fields
    for (auto const& [idPartName, value] : idNode->fields()) {
        fields_.emplace_back(idPartName, value);
    }

    // Add other fields
    if (data_->geom_)
        fields_.emplace_back(Fields::Geometry, Ptr::make(model_, data_->geom_));
    if (data_->attrLayers_ || data_->attrs_)
        fields_.emplace_back(
            Fields::PropertiesStr,
            Ptr::make(
                model_,
                simfil::ModelNodeAddress{TileFeatureLayer::FeatureProperties, addr().index()}));
    if (data_->relations_)
        fields_.emplace_back(Fields::RelationsStr, Ptr::make(model_, data_->relations_));
}

nlohmann::json Feature::toGeoJson()
{
    // Ensure that properties and geometry exist
    attributes();
    geom();
    return toJsonPrivate(*this);
}

nlohmann::json Feature::toJsonPrivate(const simfil::ModelNode& n)
{
    if (n.type() == simfil::ValueType::Object) {
        auto j = nlohmann::json::object();
        for (const auto& [fieldId, childNode] : n.fields()) {
            if (auto resolvedField = model().fieldNames()->resolve(fieldId))
                j[*resolvedField] = toJsonPrivate(*childNode);
        }
        return j;
    }
    else if (n.type() == simfil::ValueType::Array) {
        auto j = nlohmann::json::array();
        for (const auto& i : n)
            j.push_back(toJsonPrivate(*i));
        return j;
    }
    else {
        nlohmann::json j;
        std::visit(
            [&j](auto&& v)
            {
                if constexpr (!std::is_same_v<std::decay_t<decltype(v)>, std::monostate>)
                    j = v;
                else
                    j = nullptr;
            },
            n.value());
        return j;
    }
}

void Feature::addPoint(const Point& p) {
    auto newGeom = geom()->newGeometry(GeomType::Points, 0);
    newGeom->append(p);
}

void Feature::addPoints(const std::vector<Point>& points) {
    auto newGeom = geom()->newGeometry(GeomType::Points, points.size()-1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addLine(const std::vector<Point>& points) {
    auto newGeom = geom()->newGeometry(GeomType::Line, points.size()-1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addMesh(const std::vector<Point>& points) {
    auto newGeom = geom()->newGeometry(GeomType::Mesh, points.size()-1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addPoly(const std::vector<Point>& points) {
    auto newGeom = geom()->newGeometry(GeomType::Polygon, points.size()-1);
    for (auto const& p : points)
        newGeom->append(p);
}

void Feature::addRelation(
    const std::string_view& name,
    const std::string_view& targetType,
    const KeyValuePairs& targetIdParts)
{
    addRelation(name, model().newFeatureId(targetType, targetIdParts));
}

void Feature::addRelation(const std::string_view& name, const model_ptr<FeatureId>& target)
{
    addRelation(model().newRelation(name, target));
}

void Feature::addRelation(const model_ptr<Relation>& relation)
{
    relations()->append(relation);
}

uint32_t Feature::numRelations() const
{
    if (data_->relations_)
        return relations()->size();
    return 0;
}

model_ptr<Relation> Feature::getRelation(uint32_t index) const
{
    if (data_->relations_)
        return model().resolveRelation(*relations()->at(index));
    return {};
}

bool Feature::forEachRelation(std::function<bool(const model_ptr<Relation>&)> const& callback) const
{
    auto relationsPtr = relations();
    if (!relationsPtr || !callback)
        return true;
    for (auto const& relation : *relationsPtr) {
        if (!callback(model().resolveRelation(*relation)))
            return false;
    }
    return true;
}

//////////////////////////////////////////

Feature::FeaturePropertyView::FeaturePropertyView(
    Feature::Data& d,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a
)
    : simfil::MandatoryDerivedModelNodeBase<TileFeatureLayer>(std::move(l), a), data_(&d)
{
    if (data_->attrs_)
        attrs_ = model().resolveObject(Ptr::make(model_, data_->attrs_));
}

simfil::ValueType Feature::FeaturePropertyView::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::at(int64_t i) const
{
    if (data_->attrLayers_) {
        if (i == 0)
            return Ptr::make(model_, data_->attrLayers_);
        i -= 1;
    }
    if (attrs_)
        return attrs_->at(i);
    return {};
}

uint32_t Feature::FeaturePropertyView::size() const
{
    return (data_->attrLayers_ ? 1 : 0) + (attrs_ ? attrs_->size() : 0);
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::get(const simfil::FieldId& f) const
{
    if (data_->attrLayers_ && f == Fields::LayerStr)
        return Ptr::make(model_, data_->attrLayers_);
    if (attrs_)
        return attrs_->get(f);
    return {};
}

simfil::FieldId Feature::FeaturePropertyView::keyAt(int64_t i) const
{
    if (data_->attrLayers_) {
        if (i == 0)
            return Fields::LayerStr;
        i -= 1;
    }
    if (attrs_)
        return attrs_->keyAt(i);
    return {};
}

bool Feature::FeaturePropertyView::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    if (data_->attrLayers_) {
        if (!cb(*model().resolveAttributeLayerList(*Ptr::make(model_, data_->attrLayers_))))
            return false;
    }
    if (attrs_)
        return attrs_->iterate(cb);
    return true;
}

}
