#include "feature.h"
#include "featurelayer.h"

namespace mapget
{

Feature::Feature(Feature::Data& d, simfil::ModelConstPtr l, simfil::ModelNodeAddress a)
    : simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>(std::move(l), a), data_(d)
{
    updateFields();
}

model_ptr<FeatureId> Feature::id() const
{
    return pool().resolveFeatureId(*Ptr::make(pool_, data_.id_));
}

std::string_view mapget::Feature::typeId() const
{
    return pool().resolveFeatureId(*Ptr::make(pool_, data_.id_))->typeId();
}

model_ptr<simfil::GeometryCollection> Feature::geom()
{
    if (data_.geom_.value_ == 0) {
        auto result = pool().newGeometryCollection();
        data_.geom_ = result->addr();
        updateFields();
        return result;
    }
    return pool().resolveGeometryCollection(Ptr::make(pool_, data_.geom_));
}

model_ptr<AttributeLayerList> Feature::attributeLayers()
{
    if (data_.attrLayers_.value_ == 0) {
        auto result = pool().newAttributeLayers();
        data_.attrLayers_ = result->addr();
        updateFields();
        return result;
    }
    return pool().resolveAttributeLayerList(*Ptr::make(pool_, data_.attrLayers_));
}

model_ptr<Object> Feature::attributes()
{
    if (data_.attrs_.value_ == 0) {
        auto result = pool().newObject(8);
        data_.attrs_ = result->addr();
        updateFields();
        return result;
    }
    return pool().resolveObject(Ptr::make(pool_, data_.attrs_));
}

model_ptr<Array> Feature::children()
{
    if (data_.children_.value_ == 0) {
        auto result = pool().newArray(8);
        data_.children_ = result->addr();
        updateFields();
        return result;
    }
    return pool().resolveArray(Ptr::make(pool_, data_.children_));
}

std::vector<simfil::Value> Feature::evaluateAll(const std::string_view& expression)
{
    // Note: Here we rely on the assertion that the root_ column
    // contains only references to feature nodes, in the order
    // of the feature node column. We could think about protected inheritance
    // of the ModelPool to safeguard this.
    return simfil::eval(
        pool().evaluationEnvironment(),
        *pool().compiledExpression(expression),
        pool(),
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
    auto& resolver = pool();
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
    fields_.emplace_back(Fields::Type, simfil::ValueNode(std::string_view("Feature"), pool_));

    // Add id field
    fields_.emplace_back(Fields::IdStr, Ptr::make(pool_, data_.id_));
    auto idNode = pool().resolveFeatureId(*fields_.back().second);

    // Add type id field
    fields_.emplace_back(
        Fields::TypeIdStr,
        model_ptr<simfil::ValueNode>::make(idNode->typeId(), pool_));

    // Add common id-part fields
    if (pool().featureIdPrefix())
        for (auto const& [idPartName, value] : (*pool().featureIdPrefix())->fields()) {
            fields_.emplace_back(idPartName, value);
        }

    // Add feature-specific id-part fields
    for (auto const& [idPartName, value] : idNode->fields()) {
        fields_.emplace_back(idPartName, value);
    }

    // Add other fields
    if (data_.geom_.value_)
        fields_.emplace_back(Fields::Geometry, Ptr::make(pool_, data_.geom_));
    if (data_.attrLayers_.value_ || data_.attrs_.value_)
        fields_.emplace_back(
            Fields::PropertiesStr,
            Ptr::make(
                pool_,
                simfil::ModelNodeAddress{TileFeatureLayer::FeatureProperties, addr().index()}));
    if (data_.children_.value_)
        fields_.emplace_back(Fields::ChildrenStr, Ptr::make(pool_, data_.children_));
}

nlohmann::json Feature::toGeoJson()
{
    return toJsonPrivate(*this);
}

nlohmann::json Feature::toJsonPrivate(const simfil::ModelNode& n)
{
    if (n.type() == simfil::ValueType::Object) {
        auto j = nlohmann::json::object();
        for (const auto& [fieldId, childNode] : n.fields()) {
            if (auto resolvedField = pool().fieldNames()->resolve(fieldId))
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

//////////////////////////////////////////

Feature::FeaturePropertyView::FeaturePropertyView(
    Feature::Data& d,
    simfil::ModelConstPtr l,
    simfil::ModelNodeAddress a
)
    : simfil::MandatoryDerivedModelPoolNodeBase<TileFeatureLayer>(std::move(l), a), data_(d)
{
    if (data_.attrs_.value_)
        attrs_ = pool().resolveObject(Ptr::make(pool_, data_.attrs_));
}

simfil::ValueType Feature::FeaturePropertyView::type() const
{
    return simfil::ValueType::Object;
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::at(int64_t i) const
{
    if (data_.attrLayers_.value_) {
        if (i == 0)
            return Ptr::make(pool_, data_.attrLayers_);
        i -= 1;
    }
    if (attrs_)
        return (*attrs_)->at(i);
    return {};
}

uint32_t Feature::FeaturePropertyView::size() const
{
    return (data_.attrLayers_.value_ ? 1 : 0) + (attrs_ ? (*attrs_)->size() : 0);
}

simfil::ModelNode::Ptr Feature::FeaturePropertyView::get(const simfil::FieldId& f) const
{
    if (data_.attrLayers_.value_ && f == Fields::LayerStr)
        return Ptr::make(pool_, data_.attrLayers_);
    if (attrs_)
        return (*attrs_)->get(f);
    return {};
}

simfil::FieldId Feature::FeaturePropertyView::keyAt(int64_t i) const
{
    if (data_.attrLayers_.value_) {
        if (i == 0)
            return Fields::LayerStr;
        i -= 1;
    }
    if (attrs_)
        return (*attrs_)->keyAt(i);
    return {};
}

bool Feature::FeaturePropertyView::iterate(const simfil::ModelNode::IterCallback& cb) const
{
    if (data_.attrLayers_.value_) {
        if (!cb(*pool().resolveAttributeLayerList(*Ptr::make(pool_, data_.attrLayers_))))
            return false;
    }
    if (attrs_)
        return (*attrs_)->iterate(cb);
    return true;
}

}
