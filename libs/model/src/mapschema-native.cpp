#include "mapschema-impl.h"

namespace mapget
{

FeatureSchemaType::FeatureSchemaType(simfil::SchemaType type) : SchemaObjectType(std::move(type))
{
    if (!dynamic_cast<MapSchema const*>(pool()) ||
        SchemaType::kindNameId(kind()) != SchemaType::kindNameId(LayerSchema::FeatureKind))
        throw std::invalid_argument(
            "FeatureSchemaType requires a complete native Feature envelope.");
    for (auto name : {"type", "typeId", "id", "mapId", "layerId"}) {
        auto member = field(name);
        if (!member || member.types().size() != 1 || member.required() != true ||
            member.type().kind() != Kind::String)
            throw std::invalid_argument("Invalid native Feature identity envelope.");
    }
    auto discriminator = field("type").type();
    if (discriminator.directEnumSymbols().size() != 1 ||
        !discriminator.hasDirectEnumSymbol("Feature"))
        throw std::invalid_argument("Invalid native Feature type discriminator.");
    for (auto name : {"geometry", "properties", "relations", "_sourceData"}) {
        auto member = field(name);
        if (!member || member.types().size() != 1 || member.required() != false)
            throw std::invalid_argument("Incomplete native Feature envelope.");
    }
    if (!field("geometry").type().hasAffinity(simfil::ValueType::Object) ||
        !field("properties").type().hasAffinity(simfil::ValueType::Object) ||
        !field("properties").type().field("layer").type().hasAffinity(simfil::ValueType::Object) ||
        !field("relations").type().hasAffinity(simfil::ValueType::Array) ||
        !field("_sourceData").type().hasAffinity(simfil::ValueType::Array))
        throw std::invalid_argument("Invalid native Feature container shapes.");
    if (auto legacy = field("attributes"); legacy && legacy.name() == "attributes")
        throw std::invalid_argument(
            "A separately declared attributes member is a legacy Feature shape.");
    (void)identity();
}

FeatureSchemaType FeatureSchemaType::retain() const
{
    return FeatureSchemaType(SchemaType::retain());
}

FeatureIdentity FeatureSchemaType::identity() const
{
    auto discriminator = field("typeId").type();
    auto symbols = discriminator.directEnumSymbols();
    if (symbols.size() != 1)
        throw std::invalid_argument("A native Feature needs one typeId discriminator.");
    auto text = pool()->strings()->resolve(symbols.front());
    if (!text)
        throw std::invalid_argument("Invalid native Feature discriminator.");
    return static_cast<MapSchema const*>(pool())->getTypeInfo(*text);
}

simfil::SchemaObjectType FeatureSchemaType::properties() const
{
    return field("properties").type().asObject();
}

simfil::SchemaObjectType FeatureSchemaType::attributeLayers() const
{
    return properties().field("layer").type().asObject();
}

AttributeSchemaType::AttributeSchemaType(simfil::SchemaType type)
    : SchemaObjectType(std::move(type))
{
    if (!dynamic_cast<MapSchema const*>(pool()) ||
        SchemaType::kindNameId(kind()) != SchemaType::kindNameId(LayerSchema::AttributeKind) ||
        composition() != Composition::None || !alternatives().empty())
        throw std::invalid_argument("AttributeSchemaType requires a physical native Attribute.");
    for (auto name : {"validity", "_sourceData"}) {
        auto member = field(name);
        if (!member || member.types().size() != 1 || member.required() != false ||
            member.multimap() || member.role() != FieldRole::Data)
            throw std::invalid_argument("Invalid common native Attribute member.");
    }
    if (!validities().hasAffinity(simfil::ValueType::Object) ||
        !sourceDataReferences().hasAffinity(simfil::ValueType::Array))
        throw std::invalid_argument("Invalid common native Attribute types.");
}

AttributeSchemaType AttributeSchemaType::retain() const
{
    return AttributeSchemaType(SchemaType::retain());
}

simfil::SchemaType AttributeSchemaType::validities() const
{
    return field("validity").type();
}

simfil::SchemaType AttributeSchemaType::sourceDataReferences() const
{
    return field("_sourceData").type();
}

void MapSchema::validateFieldMutation(TypeId id, std::string_view field) const
{
    if (auto found = schema_->reservedFields.find(id); found != schema_->reservedFields.end() &&
        (found->second.contains(field) ||
         simfil::SchemaType::kindNameId(schemaType(id).kind()) ==
             simfil::SchemaType::kindNameId(LayerSchema::FeatureKind)))
        throw std::invalid_argument("Cannot mutate reserved mapget field: " + std::string(field));
}

void MapSchema::validateStructureMutation(TypeId id) const
{
    if (schema_->reservedFields.contains(id))
        throw std::invalid_argument("Cannot change the structure of a native mapget envelope.");
}

void MapSchema::Impl::reserveFields(TypeId id, std::set<std::string, std::less<>> fields)
{
    if (reservedFields.contains(id))
        return;
    if (metadataAppendDepth)
        appendedPolicyTypes.push_back(id);
    try {
        reservedFields.emplace(id, std::move(fields));
    }
    catch (...) {
        if (metadataAppendDepth)
            appendedPolicyTypes.pop_back();
        throw;
    }
}

void MapSchema::Impl::protectNativeType(TypeId id)
{
    auto type = pool.schemaType(id);
    if ((simfil::SchemaType::kindNameId(type.kind()) ==
             simfil::SchemaType::kindNameId(LayerSchema::AttributeKind) ||
         metaType(id) == "Attribute") &&
        isAttributeType(id, true))
    {
        protectAttributeType(id);
    }
    else if (
        simfil::SchemaType::kindNameId(type.kind()) ==
        simfil::SchemaType::kindNameId(LayerSchema::FeatureKind))
    {
        // Thin GeoJSON/legacy declarations deliberately have no native envelope policy.
        try {
            (void)FeatureSchemaType(type);
        }
        catch (std::invalid_argument const&) {
            return;
        }
        if (!type.field("attributes"))
            type.asObject().addAlias("attributes", "properties");
        std::set<std::string, std::less<>> fields{"attributes"};
        type.forEachDirectField([&](auto const& field) { fields.emplace(field.name()); });
        reserveFields(id, std::move(fields));
        reserveFields(type.field("properties").type().id(), {"layer"});
        reserveFields(type.field("properties").type().field("layer").type().id(), {});
        type.seal();
    }
}

bool MapSchema::Impl::isAttributeType(TypeId root, bool nativeOnly) const
{
    if (!valid(root))
        return false;
    auto physical = [&](simfil::SchemaType type)
    {
        if (simfil::SchemaType::kindNameId(type.kind()) !=
                simfil::SchemaType::kindNameId(LayerSchema::AttributeKind) ||
            !type.hasAffinity(simfil::ValueType::Object))
            return false;
        if (!nativeOnly)
            return true;
        if (!metaType(type.id()).empty() && metaType(type.id()) != "Attribute")
            return false;
        try {
            (void)AttributeSchemaType(type);
            return true;
        }
        catch (std::invalid_argument const&) {
            return false;
        }
    };
    if (physical(pool.schemaType(root)))
        return true;
    // Only logical combinations need traversal state. A path stack bounds storage
    // by nesting, not the size of the whole map; cycles fail without C++ recursion.
    std::vector<std::pair<TypeId, size_t>> path{{root, 0}};
    while (!path.empty()) {
        auto type = pool.schemaType(path.back().first);
        if (physical(type)) {
            path.pop_back();
            continue;
        }
        // A native logical combination derives the shared members from its
        // physical leaves; a direct override would defeat their protected shape.
        if (nativeOnly && (type.field("validity") || type.field("_sourceData")))
            return false;
        auto alternatives = type.alternatives();
        if (type.composition() == Composition::None || alternatives.empty())
            return false;
        auto& position = path.back().second;
        if (position == alternatives.size()) {
            path.pop_back();
            continue;
        }
        auto child = alternatives[position++];
        if (!valid(child) ||
            std::ranges::any_of(
                path,
                [&](auto const& ancestor) { return ancestor.first == child; }))
            return false;
        path.emplace_back(child, 0);
    }
    return true;
}

void MapSchema::Impl::protectAttributeType(TypeId root)
{
    if (auto found = reservedFields.find(root); found != reservedFields.end() &&
        found->second.contains("validity") && found->second.contains("_sourceData"))
        return;
    std::vector<TypeId> pending{root};
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        if (auto found = reservedFields.find(id); found != reservedFields.end()) {
            if (!found->second.contains("validity") || !found->second.contains("_sourceData"))
                throw std::invalid_argument(
                    "Attribute binding conflicts with another native schema role.");
            continue;
        }
        reserveFields(id, {"validity", "_sourceData"});
        auto alternatives = pool.schemaType(id).alternatives();
        pending.insert(pending.end(), alternatives.begin(), alternatives.end());
    }
}

void MapSchema::cloneTypeExtension(TypeId source, TypeId target)
{
    schema_->appendMetadata(
        [&]
        {
            auto row = schema_->data(source);
            row.key = {};
            row.pointer = {};
            row.attributeCode = {};
            row.owners = 0;
            row.flags &= ~Impl::QueryRootFlag;
            // A clone is a fresh physical declaration, not another named use site.
            if (auto found = schema_->reservedFields.find(source);
                found != schema_->reservedFields.end())
                schema_->reserveFields(target, found->second);
            schema_->data(target) = row;
        });
}

void MapSchema::requireMutableType(TypeId id) const
{
    requireMutable();
    schema_->requireType(id);
    if (schemaType(id).sealed())
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
    for (auto const& symbol : symbols)
        schemaType(id).asEnum().addSymbol(symbol);
    if (kind != Kind::Any && kind != Kind::Unknown && kind != Kind::Value)
        setNullable(id, kind == Kind::Null);
    schemaType(id).seal();
    return id;
}

TypeId MapSchema::arrayOf(TypeId element)
{
    requireMutable();
    schema_->requireType(element);
    if (auto found = schema_->arrayTypes.find(element); found != schema_->arrayTypes.end())
        return found->second;
    auto id = addSchema(Kind::Array);
    addElementSchema(id, element);
    setNullable(id, false);
    schemaType(id).seal();
    schema_->arrayTypes[element] = id;
    return id;
}

TypeId MapSchema::nullable(TypeId source)
{
    requireMutable();
    schema_->requireType(source);
    auto original = schemaType(source);
    if (original.nullable() == true) {
        schemaType(source).seal();
        return source;
    }
    if (auto found = schema_->nullableTypes.find(source); found != schema_->nullableTypes.end())
        return found->second;
    auto extra = schema_->annotation(schema_->data(source).annotations);
    auto wrap = original.composition() != Composition::None ||
        simfil::SchemaType::kindNameId(original.kind()) ==
            simfil::SchemaType::kindNameId(Kind::Never) ||
        extra.contains("not") || extra.contains("if");
    auto result = wrap ?
        simfil::SchemaPool::newType(original.kind()) :
        simfil::SchemaPool::cloneType(original);
    auto id = result.id();
    auto row = schema_->data(source);
    row.key = {};
    row.pointer = {};
    row.flags &= ~Impl::QueryRootFlag;
    row.owners = 0;
    row.attributeCode = {};
    if (wrap) {
        result.setTypeName(original.typeName());
        simfil::SchemaPool::setComposition(id, Composition::AnyOf);
        simfil::SchemaPool::addAlternative(id, source);
        simfil::SchemaPool::addAlternative(id, primitive(Kind::Null));
        row.annotations = {};
    }
    else {
        // Literal constraints belong to the non-null branch; admit null explicitly too.
        if (extra.contains("const")) {
            auto value = extra["const"];
            auto admitted = !extra.contains("enum") ||
                std::ranges::find(extra["enum"], value) != extra["enum"].end();
            extra.erase("const");
            extra["enum"] = nlohmann::json::array({nullptr});
            if (admitted && !value.is_null())
                extra["enum"].push_back(std::move(value));
        }
        else if (
            extra.contains("enum") &&
            std::ranges::find(extra["enum"], nullptr) == extra["enum"].end())
            extra["enum"].push_back(nullptr);
        bool constrained = false;
        original.forEachEnumSymbol([&](auto) { constrained = true; });
        original.forEachEnumValue([&](auto const&) { constrained = true; });
        if (constrained)
            simfil::SchemaPool::addEnumValue(id, std::monostate{});
        row.annotations = schema_->annotation(extra);
    }
    result.setNullable(true);
    schema_->data(id) = row;
    schemaType(source).seal();
    schemaType(id).seal();
    schema_->nullableTypes.emplace(source, id);
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
            setFieldRole(result, name, simfil::SchemaType::FieldRole::Context);
        }
        schemaType(result).seal();
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
        schemaType(foreign).seal();
        schemaType(parts).seal();
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
        schemaType(coordinates).seal();
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
        schemaType(bounds).seal();
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
        schemaType(position).seal();
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
        std::vector<std::pair<std::string, std::vector<TypeId>>> fields;
        schemaType(relation).forEachDirectField(
            [&](simfil::SchemaField const& item)
            {
                auto children = item.types();
                fields.emplace_back(
                    item.name(),
                    std::vector<TypeId>(children.begin(), children.end()));
            });
        for (auto const& [name, children] : fields) {
            for (auto child : children)
                addFieldSchema(reference, name, child);
            setFieldRole(reference, name, simfil::SchemaType::FieldRole::Context);
        }
        schemaType(reference).seal();
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
        schemaType(position).seal();
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
            schemaType(sequence).seal();
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
    schemaType(id).seal();
    return id;
}

AttributeSchemaType MapSchema::newAttribute(std::string key, std::string typeName, bool open)
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
    schema_->reserveFields(id, {"_sourceData", "validity"});
    return AttributeSchemaType(schemaType(id));
}

void MapSchema::setAttributeType(TypeId id, std::string name)
{
    requireMutableType(id);
    auto& row = schema_->data(id);
    if (schema_->text(row.metaType) != "Attribute")
        throw std::invalid_argument("Discriminator provenance requires an Attribute type.");
    if (row.attributeType && schema_->text(row.attributeType) != name)
        throw std::invalid_argument("Conflicting Attribute discriminator provenance.");
    row.attributeType = schema_->text(name);
}

FeatureSchemaType LayerSchema::newFeature(
    FeatureTypeInfo info,
    simfil::SchemaType properties,
    simfil::SchemaType attributeLayers)
{
    auto& owner = mutableOwner();
    for (auto view : {properties, attributeLayers})
        if (view && (view.pool() != &owner || !view.hasAffinity(simfil::ValueType::Object)))
            throw std::invalid_argument(
                "Native Feature containers must be same-pool object types.");
    if (auto existing = owner.getTypeInfo(info.name_, false);
        existing && !existing.sameIdentity(info))
        throw std::invalid_argument("Conflicting map-wide feature identity: " + info.name_);
    static constexpr std::string_view RESERVED[]{
        "type",
        "typeId",
        "id",
        "mapId",
        "layerId",
        "geometry",
        "properties",
        "attributes",
        "relations",
        "_sourceData"};
    if (!info.uniqueIdCompositions_.empty())
        for (auto const& part : info.uniqueIdCompositions_.front())
            if (std::ranges::find(RESERVED, part.idPartLabel_) != std::end(RESERVED))
                throw std::invalid_argument(
                    "Feature ID part conflicts with reserved field: " + part.idPartLabel_);
    auto name = info.name_;
    owner.addFeatureType(std::move(info));
    return newFeature(name, properties, attributeLayers);
}

FeatureSchemaType LayerSchema::newFeature(
    std::string_view featureName,
    simfil::SchemaType propertiesView,
    simfil::SchemaType attributeLayersView)
{
    auto& owner = mutableOwner();
    for (auto view : {propertiesView, attributeLayersView})
        if (view && (view.pool() != &owner || !view.hasAffinity(simfil::ValueType::Object)))
            throw std::invalid_argument(
                "Native Feature containers must be same-pool object types.");
    auto properties = propertiesView ? propertiesView.id() : simfil::NoTypeId;
    auto attributeLayers = attributeLayersView ? attributeLayersView.id() : simfil::NoTypeId;
    auto info = owner.getTypeInfo(featureName);
    static constexpr std::string_view RESERVED[]{
        "type",
        "typeId",
        "id",
        "mapId",
        "layerId",
        "geometry",
        "properties",
        "attributes",
        "relations",
        "_sourceData"};
    for (auto const& part : info.compositions().front().parts())
        if (std::ranges::find(RESERVED, part.name()) != std::end(RESERVED))
            throw std::invalid_argument(
                "Feature ID part conflicts with reserved field: " + std::string(part.name()));
    for (auto view : {propertiesView, attributeLayersView})
        if (view)
            owner.requireMutableType(view.id());
    if (schemaId(featureKey(std::string(info.name()))))
        throw std::invalid_argument("Feature already declared: " + std::string(info.name()));
    auto string = owner.primitive(Kind::String);
    auto geometry = owner.nullable(owner.builtin(MapSchema::Builtin::Geometry));
    auto relations = owner.arrayOf(owner.builtin(MapSchema::Builtin::Relation));
    auto refs = owner.builtin(MapSchema::Builtin::SourceDataReferences);
    if (!properties)
        properties = addSchema(Kind::Object, {}, "FeatureProperties");
    if (!attributeLayers)
        attributeLayers = owner.schemaType(properties).field("layer").type().id();
    if (!attributeLayers)
        attributeLayers = addSchema(Kind::Object, {}, "AttributeLayerMap");
    for (auto id : {properties, attributeLayers}) {
        owner.requireMutableType(id);
        if (!owner.schemaType(id).hasAffinity(simfil::ValueType::Object))
            throw std::invalid_argument("Feature properties and layer map must be objects.");
    }
    auto existingLayer = owner.schemaType(properties).field("layer");
    if (existingLayer &&
        (existingLayer.types().size() != 1 || existingLayer.types().front() != attributeLayers))
        throw std::invalid_argument("Feature properties already contain a different layer map.");
    if (!existingLayer)
        owner.addFieldSchema(properties, "layer", attributeLayers);
    owner.schema_->reserveFields(properties, {"layer"});
    owner.schema_->data(properties).metaType = owner.schema_->text("FeatureProperties");
    owner.schema_->data(attributeLayers).metaType = owner.schema_->text("AttributeLayerMap");
    owner.schema_->reserveFields(attributeLayers, {});
    registerSchemaKey(featurePropertiesKey(std::string(info.name())), properties);
    registerSchemaKey(attributeLayerMapKey(std::string(info.name())), attributeLayers);
    auto id = addSchema(FeatureKind, featureKey(std::string(info.name())), "Feature");
    owner.setTypeName(id, std::string(info.name()));
    owner.setNullable(id, false);
    auto field = [&](std::string name, TypeId child, bool required = false)
    {
        owner.addFieldSchema(id, name, child);
        owner.setFieldRequired(id, std::move(name), required);
    };
    field("type", owner.primitive(Kind::String, std::vector<std::string>{"Feature"}), true);
    field(
        "typeId",
        owner.primitive(Kind::String, std::vector<std::string>{std::string(info.name())}),
        true);
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
    owner.simfil::SchemaPool::addFieldAlias(id, "attributes", "properties");
    // Only primary composition fields occur on produced Features. Secondary IDs describe
    // references.
    for (auto const& part : info.compositions().front().parts()) {
        if (owner.schemaType(id).field(part.name()))
            throw std::invalid_argument(
                "Feature ID part conflicts with reserved field: " + std::string(part.name()));
        auto key = "mapget:id-part:" + part.toJson().dump();
        auto scalar = owner.findSchema(key);
        if (!scalar) {
            bool text = part.datatype() == IdPartDataType::STR ||
                part.datatype() == IdPartDataType::UUID128;
            scalar = owner.addSchema(text ? Kind::String : Kind::Int, std::move(key));
            owner.setNullable(scalar, false);
            nlohmann::json annotations{
                {"description", part.description()},
                {"x-mapget", {{"datatype", nlohmann::json(part.datatype())}}}};
            if (part.datatype() == IdPartDataType::UUID128)
                annotations.update({{"minLength", 16}, {"maxLength", 16}});
            if (!text) {
                int64_t minimum = INT64_MIN, maximum = INT64_MAX;
                if (part.datatype() == IdPartDataType::I32) {
                    minimum = INT32_MIN;
                    maximum = INT32_MAX;
                }
                if (part.datatype() == IdPartDataType::U32) {
                    minimum = 0;
                    maximum = UINT32_MAX;
                }
                // U64 IDs use the full signed int64 projection in the native value model.
                if (part.datatype() == IdPartDataType::U64)
                    annotations["x-mapget"]["encoding"] = "signed-64-bit-pattern";
                annotations["minimum"] = minimum;
                annotations["maximum"] = maximum;
            }
            owner.setJsonSchemaAnnotations(scalar, std::move(annotations));
            owner.schemaType(scalar).seal();
        }
        field(std::string(part.name()), scalar, !part.optional());
    }
    std::set<std::string, std::less<>> reserved{"attributes"};
    owner.schemaType(id).forEachDirectField(
        [&](simfil::SchemaField const& member) { reserved.emplace(member.name()); });
    owner.schema_->reserveFields(id, std::move(reserved));
    addFeatureType(info.name(), id);
    auto result = FeatureSchemaType(owner.schemaType(id));
    result.seal();
    return result;
}

void LayerSchema::bindAttribute(
    simfil::SchemaType attribute,
    std::string_view feature,
    std::string_view layer,
    std::string_view name)
{
    auto& owner = mutableOwner();
    if (!attribute || attribute.pool() != &owner)
        throw std::invalid_argument("Attribute bindings require a type in the same map schema.");
    auto id = attribute.id();
    if (feature.empty() || layer.empty() || name.empty())
        throw std::invalid_argument("An Attribute use site requires feature, layer and name.");
    auto& graph = *owner.schema_;
    auto native = FeatureSchemaType(owner.schemaType(featureSchema(feature)));
    auto layers = native.attributeLayers();
    auto layerEdge = layers.field(layer);
    auto container = layerEdge.type();
    if (layerEdge &&
        (layerEdge.types().size() != 1 || !container ||
         !container.hasAffinity(simfil::ValueType::Object) || !layerEdge.multimap()))
        throw std::invalid_argument(
            "Attribute layer binding conflicts with the existing container edge.");
    if (container && !graph.metaType(container.id()).empty() &&
        graph.metaType(container.id()) != "AttributeContainer")
        throw std::invalid_argument("Attribute layer binding requires an AttributeContainer.");
    auto existing = container ? container.field(name) : simfil::SchemaField{};
    if (existing &&
        (existing.types().size() != 1 || existing.type() != attribute || !existing.multimap()))
        throw std::invalid_argument("Attribute binding conflicts with the existing payload edge.");
    // A logical alternative is one physical use site; its branches retain their
    // independent provenance, not fabricated duplicate container edges.
    if (!graph.isAttributeType(id, true))
        throw std::invalid_argument(
            "Attribute bindings require native Attribute types or their union.");
    AttributePathOwner site{
        std::string(feature),
        std::string(layer),
        std::string(name),
        id,
        std::string(this->name())};
    auto old = graph.data(id).owners;
    auto containerKey = attributeContainerKey(feature, layer);
    auto declaredContainer = schemaId(containerKey);
    if (declaredContainer && container && declaredContainer != container.id())
        throw std::invalid_argument(
            "Attribute layer binding conflicts with its scoped schema key.");
    if (declaredContainer && !container)
        throw std::invalid_argument(
            "Attribute container key exists without its feature layer edge.");
    auto key = graph.text(graph.data(id).key);
    auto useKey = key.empty() ? std::string{} : attributeKey(feature, layer, name, key);
    if (!useKey.empty())
        if (auto known = schemaId(useKey); known && known != id)
            throw std::invalid_argument(
                "Attribute use-site schema key already names another type.");

    graph.appendMetadata(
        [&]
        {
            MapSchema::Impl::TypeData staged;
            ModelNodeAddress containerMeta;
            auto stage = [&]
            {
                graph.protectAttributeType(id);
                staged = graph.prepareAttributeOwner(id, site);
                staged.metaType = graph.text("Attribute");
                if (container)
                    (void)graph.data(container.id());  // Reserve metadata before publication.
                containerMeta = graph.text("AttributeContainer");
                if (!useKey.empty() && !schemaId(useKey))
                    registerSchemaKey(useKey, id);
            };
            if (existing) {
                // No generic edge changes: the metadata-only transaction still owns
                // all string/value allocations, including key and owner registration.
                owner.appendExtension([&] { stage(); });
            }
            else if (container) {
                owner.appendFieldWithExtension(
                    container.id(),
                    name,
                    [&]
                    {
                        stage();
                        return attribute;
                    },
                    true,
                    false);
            }
            else {
                owner.appendFieldWithExtension(
                    layers.id(),
                    layer,
                    [&]
                    {
                        auto created = addSchema(Kind::Object, containerKey, "AttributeContainer");
                        container = owner.schemaType(created);
                        container.asObject().addField(name, attribute).setMultimap(true);
                        container.field(name).setRequired(false);
                        stage();
                        return container;
                    },
                    true,
                    false);
            }
            // These assignments and arena retirements cannot allocate. Both sides of
            // the binding become visible together only after generic staging succeeds.
            graph.data(id) = staged;
            graph.data(container.id()).metaType = containerMeta;
            if (old && old != staged.owners)
                graph.owners.truncate(old, 0);
        });
}

void LayerSchema::setRelationDeclarations(std::vector<RelationDeclaration> declarations)
{
    auto& owner = mutableOwner();
    owner.schema_->setRelations(addr_.index(), std::move(declarations));
}

}  // namespace mapget
