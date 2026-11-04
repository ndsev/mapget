#include "mapschema-impl.h"

namespace mapget
{

void MapSchema::requireMutableType(TypeId id) const
{
    requireMutable();
    schema_->requireType(id);
    if (schema_->builder->schemas_[id].immutable_)
        throw std::invalid_argument(
            "Cannot modify a canonical mapget type; declare a distinct type instead.");
}

TypeId MapSchema::primitive(Kind kind, std::span<std::string const> symbols)
{
    requireMutable();
    if (kind != Kind::Unknown && kind != Kind::Any && kind != Kind::Null && kind != Kind::Bool &&
        kind != Kind::Int && kind != Kind::Float && kind != Kind::String && kind != Kind::Bytes &&
        kind != Kind::Value)
        throw std::invalid_argument("primitive() requires a scalar, Any or Unknown kind.");
    if (!symbols.empty() && kind != Kind::String)
        throw std::invalid_argument("String symbols require a string type.");
    auto key = "mapget:primitive:" + std::to_string(uint32_t(kind));
    for (auto const& symbol : symbols)
        key += ":" + std::to_string(symbol.size()) + ":" + symbol;
    if (auto id = findSchema(key))
        return id;
    auto id = addSchema(kind, std::move(key));
    addEnumSymbols(id, symbols);
    if (kind != Kind::Any && kind != Kind::Unknown && kind != Kind::Value)
        setNullable(id, kind == Kind::Null);
    schema_->builder->schemas_[id].immutable_ = true;
    return id;
}

TypeId MapSchema::arrayOf(TypeId element)
{
    requireMutable();
    schema_->requireType(element);
    if (auto found = schema_->builder->arrayTypes_.find(element);
        found != schema_->builder->arrayTypes_.end())
        return found->second;
    auto id = addSchema(Kind::Array);
    addElementSchema(id, element);
    setNullable(id, false);
    schema_->builder->schemas_[id].immutable_ = true;
    schema_->builder->arrayTypes_[element] = id;
    return id;
}

TypeId MapSchema::nullable(TypeId source)
{
    requireMutable();
    schema_->requireType(source);
    if (schema_->builder->schemas_[source].nullable_ == true) {
        schema_->builder->schemas_[source].immutable_ = true;
        return source;
    }
    if (auto found = schema_->builder->nullableTypes_.find(source);
        found != schema_->builder->nullableTypes_.end())
        return found->second;
    auto copy = schema_->builder->schemas_[source];
    auto id = addSchema(copy.kind_, {}, copy.entry_.metaType_);
    copy.id_ = id;
    copy.entry_ = schema_->builder->entriesById_[id];
    copy.nullable_ = true;
    copy.kind_ = simfil::Schema::makeKind(
        simfil::Schema::kindNameId(copy.kind_),
        simfil::Schema::affinities(copy.kind_) |
            simfil::valueTypeAffinity(simfil::ValueType::Null));
    copy.immutable_ = true;
    // Null is admitted beside the completed contract, not by mutating its children.
    if (copy.composition_ != Composition::None ||
        simfil::Schema::kindNameId(copy.kind_) == simfil::Schema::kindNameId(Kind::Never) ||
        copy.annotations_.contains("not") || copy.annotations_.contains("if"))
    {
        copy.composition_ = Composition::AnyOf;
        copy.alternatives_ = {source, primitive(Kind::Null)};
        copy.directFields_.clear();
        copy.childSchemas_.clear();
        copy.elementSchemas_.clear();
        copy.required_.clear();
        copy.contextFields_.clear();
        copy.directEnumSymbols_.clear();
        copy.enumValues_.clear();
        copy.annotations_ = nlohmann::json::object();
    }
    else {
        // Enum/const constrain values independently of the JSON type keyword.
        if (copy.annotations_.contains("const")) {
            auto value = copy.annotations_["const"];
            bool admitted = !copy.annotations_.contains("enum") ||
                std::ranges::find(copy.annotations_["enum"], value) !=
                    copy.annotations_["enum"].end();
            copy.annotations_.erase("const");
            copy.annotations_["enum"] = nlohmann::json::array({nullptr});
            if (admitted && !value.is_null())
                copy.annotations_["enum"].push_back(std::move(value));
        }
        else if (copy.annotations_.contains("enum")) {
            auto& values = copy.annotations_["enum"];
            if (std::ranges::find(values, nullptr) == values.end())
                values.push_back(nullptr);
        }
        if (!copy.directEnumSymbols_.empty() || !copy.enumValues_.empty())
            copy.enumValues_.emplace_back(std::monostate{});
    }
    schema_->builder->schemas_[source].immutable_ = true;
    schema_->builder->schemas_[id] = std::move(copy);
    schema_->builder->nullableTypes_[source] = id;
    return id;
}

TypeId MapSchema::builtin(Builtin type)
{
    requireMutable();
    static constexpr std::string_view names[]{
        "SourceDataReference",
        "SourceDataReferences",
        "Geometry",
        "FeatureId",
        "Validity",
        "Validities",
        "Relation",
        "RelationReference",
        "AttrPoint",
        "AttrPointSequence",
        "AttrPointIndex",
        "AttrPointIndexRange"};
    if (size_t(type) >= std::size(names))
        throw std::invalid_argument("Unknown mapget built-in type.");
    auto key = "mapget:" + std::string(names[size_t(type)]);
    if (auto id = findSchema(key))
        return id;
    if (type == Builtin::SourceDataReferences) {
        auto id = arrayOf(builtin(Builtin::SourceDataReference));
        registerSchemaKey(std::move(key), id);
        return id;
    }

    auto id = addSchema(Kind::Object, std::move(key));
    setTypeName(id, "mapget::" + std::string(names[size_t(type)]));
    setNullable(id, false);
    auto field = [&](std::string name, TypeId child, bool required = false)
    {
        addFieldSchema(id, name, child);
        setFieldRequired(id, std::move(name), required);
    };
    auto string = primitive(Kind::String);
    auto integer = primitive(Kind::Int);
    auto strings = [&](std::initializer_list<std::string> values)
    {
        return primitive(Kind::String, std::vector<std::string>(values));
    };
    auto sourceLinks = [&]
    {
        field("_sourceData", builtin(Builtin::SourceDataReferences));
    };
    // PointNode enumerates three numbers, but also exposes named coordinate lookup.
    auto point = [&]
    {
        if (auto existing = findSchema("mapget:Point"))
            return existing;
        auto result = addSchema(Kind::Array, "mapget:Point");
        setTypeName(result, "mapget::Point");
        setNullable(result, false);
        auto number = primitive(Kind::Float);
        addElementSchema(result, number);
        setJsonSchemaAnnotations(result, {{"minItems", 3}, {"maxItems", 3}});
        for (auto name : {"lon", "lat", "elevation"}) {
            addFieldSchema(result, name, number);
            setFieldRole(result, name, simfil::Schema::FieldRole::Context);
        }
        schema_->builder->schemas_[result].immutable_ = true;
        return result;
    };

    switch (type) {
    case Builtin::SourceDataReference:
        field("layerId", string, true);
        field("qualifier", string, true);
        field("address", integer, true);
        registerSchemaKey("#/definitions/sourceDataReference", id);
        break;
    case Builtin::SourceDataReferences: break;
    case Builtin::FeatureId: {
        // Local IDs are strings; cross-map references retain named, self-describing parts.
        auto foreign = addSchema(Kind::Object);
        auto parts = addSchema(Kind::Object, "mapget:FeatureIdParts");
        setTypeName(parts, "mapget::FeatureIdParts");
        setOpen(parts, true);
        for (auto const& name : {"mapId", "typeId", "id"}) {
            addFieldSchema(foreign, name, std::string_view(name) == "id" ? parts : string);
            setFieldRequired(foreign, name, true);
        }
        setComposition(id, Composition::AnyOf);
        addAlternative(id, string);
        addAlternative(id, foreign);
        schema_->builder->schemas_[foreign].immutable_ = true;
        schema_->builder->schemas_[parts].immutable_ = true;
        break;
    }
    case Builtin::Geometry: {
        field(
            "type",
            strings(
                {"Point",
                 "MultiPoint",
                 "LineString",
                 "MultiLineString",
                 "Polygon",
                 "MultiPolygon",
                 "GeometryCollection"}),
            true);
        // Geometry kinds select one to four array levels. Keeping the union closed
        // lets unrelated wildcard fields skip even very large coordinate buffers.
        auto coordinates = addSchema(Kind::Union, "mapget:Coordinates");
        setComposition(coordinates, Composition::AnyOf);
        auto nested = point();
        for (unsigned depth = 1; depth <= 4; ++depth) {
            addAlternative(coordinates, nested);
            if (depth < 4)
                nested = arrayOf(nested);
        }
        schema_->builder->schemas_[coordinates].immutable_ = true;
        field("coordinates", coordinates);
        field("geometries", arrayOf(id));
        field("geometryName", string);
        auto bounds = addSchema(Kind::Object, "mapget:Bounds");
        setTypeName(bounds, "mapget::Bounds");
        setNullable(bounds, false);
        for (auto name : {"origin", "size"}) {
            addFieldSchema(bounds, name, point());
            setFieldRequired(bounds, name, true);
        }
        schema_->builder->schemas_[bounds].immutable_ = true;
        field("aabb", bounds);
        field("gltfNodeIndex", integer);
        sourceLinks();
        registerSchemaKey("#/definitions/geometry", id);
        break;
    }
    case Builtin::Validity: {
        field("direction", strings({"COMPLETE", "POSITIVE", "NEGATIVE", "EMPTY", "NONE"}));
        field("geometryName", string);
        field("geometry", builtin(Builtin::Geometry));
        field(
            "offsetType",
            strings(
                {"GeoPosOffset", "BufferOffset", "RelativeLengthOffset", "MetricLengthOffset"}));
        // Offsets are numbers or coordinate arrays, depending on offsetType.
        auto position = addSchema(Kind::Union);
        setComposition(position, Composition::AnyOf);
        addAlternative(position, primitive(Kind::Float));
        addAlternative(position, point());
        schema_->builder->schemas_[position].immutable_ = true;
        for (auto const& name : {"point", "start", "end"})
            field(name, position);
        for (auto const& name : {"featureId", "from", "to"})
            field(name, builtin(Builtin::FeatureId));
        field("attrPointIndex", builtin(Builtin::AttrPointIndex));
        field("attrPointIndexRange", builtin(Builtin::AttrPointIndexRange));
        field("transitionNumber", integer);
        field("fromConnectedEnd", strings({"START", "END"}));
        field("toConnectedEnd", strings({"START", "END"}));
        registerSchemaKey("#/definitions/validityEntry", id);
        break;
    }
    case Builtin::Validities:
        setComposition(id, Composition::AnyOf);
        addAlternative(id, builtin(Builtin::Validity));
        addAlternative(id, arrayOf(builtin(Builtin::Validity)));
        registerSchemaKey("#/definitions/validity", id);
        break;
    case Builtin::Relation:
        field("name", string, true);
        field("target", builtin(Builtin::FeatureId));
        field("sourceValidity", builtin(Builtin::Validities));
        field("targetValidity", builtin(Builtin::Validities));
        sourceLinks();
        break;
    case Builtin::RelationReference: {
        auto reference = addSchema(Kind::Object);
        addFieldSchema(reference, "$mapgetRelation", integer);
        setFieldRequired(reference, "$mapgetRelation", true);
        auto relation = builtin(Builtin::Relation);
        // Indexed references enumerate only the index, but retain explicit relation lookup.
        auto fields = schema_->builder->schemas_[relation].childSchemas_;
        for (auto const& [name, children] : fields) {
            for (auto child : children)
                addFieldSchema(reference, name, child);
            setFieldRole(reference, name, simfil::Schema::FieldRole::Context);
        }
        schema_->builder->schemas_[reference].immutable_ = true;
        setComposition(id, Composition::AnyOf);
        addAlternative(id, reference);
        addAlternative(id, relation);
        registerSchemaKey("#/definitions/relationReference", id);
        break;
    }
    case Builtin::AttrPoint: {
        field("index", integer, true);
        // Native inspection is a display string; lossless JSON uses coordinate arrays.
        auto position = addSchema(Kind::Union);
        setComposition(position, Composition::AnyOf);
        addAlternative(position, string);
        addAlternative(position, point());
        schema_->builder->schemas_[position].immutable_ = true;
        field("point", position, true);
        sourceLinks();
        break;
    }
    case Builtin::AttrPointSequence:
        field("featureId", builtin(Builtin::FeatureId), true);
        field("geometryIndex", integer, true);
        field("geometryName", string);
        field("attrPoints", arrayOf(builtin(Builtin::AttrPoint)), true);
        field("positionCount", integer);
        field("id", integer);
        sourceLinks();
        break;
    case Builtin::AttrPointIndex:
    case Builtin::AttrPointIndexRange: {
        auto sequence = findSchema("mapget:AttrPointSequenceReference");
        if (!sequence) {
            sequence = addSchema(Kind::Object, "mapget:AttrPointSequenceReference");
            addFieldSchema(sequence, "$mapgetAttrPointSequence", integer);
            setFieldRequired(sequence, "$mapgetAttrPointSequence", true);
            schema_->builder->schemas_[sequence].immutable_ = true;
        }
        field("sequence", sequence, true);
        if (type == Builtin::AttrPointIndex)
            field("index", integer, true);
        else {
            field("start", integer, true);
            field("end", integer, true);
        }
        break;
    }
    }
    schema_->builder->schemas_[id].immutable_ = true;
    return id;
}

TypeId MapSchema::newAttribute(std::string key, std::string typeName, bool open)
{
    auto refs = builtin(Builtin::SourceDataReferences);
    auto validity = builtin(Builtin::Validities);
    auto id = addSchema(LayerSchema::AttributeKind, std::move(key), "Attribute");
    setTypeName(id, std::move(typeName));
    setOpen(id, open);
    setNullable(id, false);
    addFieldSchema(id, "_sourceData", refs);
    addFieldSchema(id, "validity", validity);
    setFieldRequired(id, "_sourceData", false);
    setFieldRequired(id, "validity", false);
    schema_->builder->schemas_[id].reservedFields_ = {"_sourceData", "validity"};
    return id;
}

void MapSchema::setAttributeType(TypeId id, std::string name)
{
    requireMutableType(id);
    auto& type = schema_->builder->schemas_[id];
    if (type.entry_.metaType_ != "Attribute")
        throw std::invalid_argument("Discriminator provenance requires an Attribute type.");
    if (!type.attributeType_.empty() && type.attributeType_ != name)
        throw std::invalid_argument("Conflicting Attribute discriminator provenance.");
    type.attributeType_ = std::move(name);
}

TypeId LayerSchema::newFeature(
    FeatureTypeInfo info,
    TypeId properties,
    TypeId attributeLayers,
    bool produced)
{
    auto& owner = mutableOwner();
    info = FeatureTypeInfo::fromJson(info.toJson());
    if (schemaId(featureKey(info.name_)))
        throw std::invalid_argument("Feature already declared: " + info.name_);
    auto string = owner.primitive(Kind::String);
    auto geometry = owner.nullable(owner.builtin(MapSchema::Builtin::Geometry));
    auto relations = owner.arrayOf(owner.builtin(MapSchema::Builtin::Relation));
    auto refs = owner.builtin(MapSchema::Builtin::SourceDataReferences);
    if (!properties)
        properties = addSchema(Kind::Object, {}, "FeatureProperties");
    if (!attributeLayers)
        attributeLayers = childSchema(properties, "layer");
    if (!attributeLayers)
        attributeLayers = addSchema(Kind::Object, {}, "AttributeLayerMap");
    for (auto id : {properties, attributeLayers}) {
        owner.requireMutableType(id);
        if (!schema_detail::hasAffinity(kind(id), simfil::ValueType::Object))
            throw std::invalid_argument("Feature properties and layer map must be objects.");
    }
    auto& props = owner.schema_->builder->schemas_[properties];
    if (auto found = props.childSchemas_.find("layer");
        found != props.childSchemas_.end() && found->second != std::vector<TypeId>{attributeLayers})
        throw std::invalid_argument("Feature properties already contain a different layer map.");
    addFieldSchema(properties, "layer", attributeLayers);
    props.reservedFields_.insert("layer");
    props.entry_.metaType_ = "FeatureProperties";
    owner.schema_->builder->schemas_[attributeLayers].entry_.metaType_ = "AttributeLayerMap";
    registerSchemaKey(featurePropertiesKey(info.name_), properties);
    registerSchemaKey(attributeLayerMapKey(info.name_), attributeLayers);
    auto id = addSchema(FeatureKind, featureKey(info.name_), "Feature");
    setTypeName(id, info.name_);
    setNullable(id, false);
    auto field = [&](std::string name, TypeId child, bool required = false)
    {
        addFieldSchema(id, name, child);
        setFieldRequired(id, std::move(name), required);
    };
    field("type", owner.primitive(Kind::String, std::vector<std::string>{"Feature"}), true);
    field("typeId", owner.primitive(Kind::String, std::vector<std::string>{info.name_}), true);
    field("id", string, true);
    field("mapId", string, true);
    field(
        "layerId",
        owner.primitive(Kind::String, std::vector<std::string>{std::string(name())}),
        true);
    field("geometry", geometry);
    field("properties", properties);
    field("relations", relations);
    field("_sourceData", refs);
    auto& root = owner.schema_->builder->schemas_[id];
    root.reservedFields_.insert(root.directFields_.begin(), root.directFields_.end());
    root.reservedFields_.insert("attributes");
    // Only primary composition fields occur on produced Features. Secondary IDs describe
    // references.
    for (auto const& part : info.uniqueIdCompositions_.front()) {
        if (owner.schema_->builder->schemas_[id].reservedFields_.contains(part.idPartLabel_))
            throw std::invalid_argument(
                "Feature ID part conflicts with reserved field: " + part.idPartLabel_);
        auto key = "mapget:id-part:" + part.toJson().dump();
        auto scalar = owner.findSchema(key);
        if (!scalar) {
            bool text = part.datatype_ == IdPartDataType::STR ||
                part.datatype_ == IdPartDataType::UUID128;
            scalar = owner.addSchema(text ? Kind::String : Kind::Int, std::move(key));
            owner.setNullable(scalar, false);
            nlohmann::json annotations{
                {"description", part.description_},
                {"x-mapget", {{"datatype", nlohmann::json(part.datatype_)}}}};
            if (part.datatype_ == IdPartDataType::UUID128)
                annotations.update({{"minLength", 16}, {"maxLength", 16}});
            if (!text) {
                int64_t minimum = INT64_MIN, maximum = INT64_MAX;
                if (part.datatype_ == IdPartDataType::I32) {
                    minimum = INT32_MIN;
                    maximum = INT32_MAX;
                }
                if (part.datatype_ == IdPartDataType::U32) {
                    minimum = 0;
                    maximum = UINT32_MAX;
                }
                // U64 IDs use the full signed int64 projection in the native value model.
                if (part.datatype_ == IdPartDataType::U64)
                    annotations["x-mapget"]["encoding"] = "signed-64-bit-pattern";
                annotations["minimum"] = minimum;
                annotations["maximum"] = maximum;
            }
            owner.setJsonSchemaAnnotations(scalar, std::move(annotations));
            owner.schema_->builder->schemas_[scalar].immutable_ = true;
        }
        field(part.idPartLabel_, scalar, !part.isOptional_);
        owner.schema_->builder->schemas_[id].reservedFields_.insert(part.idPartLabel_);
    }
    addFeatureType(std::move(info), id, produced);
    return id;
}

void LayerSchema::bindAttribute(
    TypeId id,
    std::string_view feature,
    std::string_view layer,
    std::string_view name)
{
    auto& owner = mutableOwner();
    owner.schema_->requireType(id);
    if (feature.empty() || layer.empty() || name.empty())
        throw std::invalid_argument("An Attribute use site requires feature, layer and name.");
    auto& type = owner.schema_->builder->schemas_[id];
    if (type.entry_.metaType_ != "Attribute") {
        if (type.composition_ == Composition::None || type.alternatives_.empty() ||
            std::ranges::any_of(
                type.alternatives_,
                [&](auto child) {
                    return owner.schema_->builder->schemas_[child].entry_.metaType_ != "Attribute";
                }))
            throw std::invalid_argument("Attribute bindings require a native Attribute type.");
        type.entry_.metaType_ = "Attribute";
        // A logical aggregate is still an Attribute payload. Preserve its structural
        // affinities/composition while making native query-overlay validation agree
        // with the same aggregate imported from JSON metadata.
        type.kind_ = simfil::Schema::makeKind(
            simfil::Schema::kindNameId(AttributeKind),
            simfil::Schema::affinities(type.kind_));
    }
    owner.schema_->builder->registerAttributeOwner(
        id,
        {std::string(feature),
         std::string(layer),
         std::string(name),
         id,
         std::string(this->name())});
    auto const& first = type.attributeOwners_.front().attributeName_;
    type.attributeTypeCode_ =
        std::ranges::all_of(
            type.attributeOwners_,
            [&](auto const& site) { return site.attributeName_ == first; }) ?
        first :
        std::string{};
    if (!type.entry_.key_.empty())
        registerSchemaKey(attributeKey(feature, layer, name, type.entry_.key_), id);
}

void LayerSchema::setRelationDeclarations(std::vector<RelationDeclaration> declarations)
{
    auto& owner = mutableOwner();
    auto& layer = owner.schema_->layerAt(addr_.index());
    std::set<std::pair<std::string, std::string>> keys;
    for (auto& declaration : declarations) {
        if (declaration.ownerType_.empty() || declaration.name_.empty() ||
            declaration.targetTypes_.empty() ||
            !keys.emplace(declaration.ownerType_, declaration.name_).second ||
            unsigned(declaration.locality_) >
                unsigned(RelationDeclaration::Locality::MayCrossContainer))
            throw std::invalid_argument("Invalid or duplicate forward relation declaration.");
        std::ranges::sort(declaration.targetTypes_);
        declaration.targetTypes_.erase(
            std::unique(declaration.targetTypes_.begin(), declaration.targetTypes_.end()),
            declaration.targetTypes_.end());
        if (declaration.targetTypes_.front().empty())
            throw std::invalid_argument("A relation target type cannot be empty.");
    }
    layer.relationsDeclared = true;
    layer.relationsKnown = true;
    layer.relations = std::move(declarations);
}

}  // namespace mapget
