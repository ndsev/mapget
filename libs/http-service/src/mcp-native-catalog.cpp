#include "mcp-native-tools.h"
#include "simfil/model/schema.h"

namespace mapget::detail
{

void McpNativeTools::buildCatalog()
{
    using Json = nlohmann::json;
    auto string = [](size_t max = 512) -> Json
    {
        return {{"type", "string"}, {"minLength", 1}, {"maxLength", max}};
    };
    auto array = [](Json item, size_t max) -> Json
    {
        return {{"type", "array"}, {"items", std::move(item)}, {"maxItems", max}};
    };
    auto object = [](Json properties, Json required = Json::array()) -> Json
    {
        return {
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
    };
    auto integer = [](int64_t min, int64_t max) -> Json
    {
        return {{"type", "integer"}, {"minimum", min}, {"maximum", max}};
    };
    auto partition = Json{
        {"oneOf",
         Json::array(
             {object(
                  {{"kind", {{"const", "tile"}}}, {"id", integer(INT32_MIN, INT32_MAX)}},
                  {"kind", "id"}),
              object(
                  {{"kind", {{"const", "object"}}},
                   {"id", {{"type", "string"}, {"pattern", "^[0-9]{1,20}$"}}}},
                  {"kind", "id"})})}};
    auto budgets = Json{{"limit", integer(1, 1000)}, {"maxWork", integer(1, 1000000)}};
    budgets["limit"]["default"] = 100;
    budgets["maxWork"]["default"] = 100000;
    auto selection = Json{{"mapId", string()}, {"layerId", string()}, {"sourceId", string()}};
    auto query = Json{{"query", string(4096)}, {"trace", {{"type", "boolean"}}}};
    auto output = object(
        {{"items", array(Json::object(), 1000)},
         {"complete", {{"type", "boolean"}}},
         {"reason", {{"type", {"string", "null"}}}},
         {"issues", array(Json::object(), 100)},
         {"traces", {{"type", "object"}}}},
        {"items", "complete", "reason", "issues", "traces"});
    auto add =
        [&](std::string name,
            std::string description,
            Json properties,
            Json required = Json::array(),
            bool mutation = false)
    {
        properties.update(budgets);
        auto input = object(std::move(properties), std::move(required));
        input["$schema"] = "http://json-schema.org/draft-07/schema#";
        auto& action = actions_[name];
        action.tool = {
            {"name", name},
            {"description", std::move(description)},
            {"inputSchema", input},
            {"outputSchema", output},
            {"annotations",
             {{"readOnlyHint", !mutation},
              {"destructiveHint", mutation},
              {"idempotentHint", !mutation},
              {"openWorldHint", true}}}};
    };

    add("mapget_list_sources",
        "List authorized sources in configuration order, with lifecycle state and compact layer "
        "metadata including feature types and ordered ID compositions. Excludes feature-model "
        "schemas and coverage records; performs no tile I/O.",
        selection);
    auto coverage = selection;
    coverage["level"] = integer(0, 15);
    coverage["level"]["description"] = "Restrict advertised ranges to this NDS tile level.";
    add("mapget_get_coverage",
        "Read a layer's advertised NDS tile-grid coverage. min/max are packed grid-corner IDs, "
        "not a numeric ID interval. Sparse filled masks use row-major order (x increasing, then "
        "y increasing); empty masks mean full rectangles. "
        "coverageKnown=false means unspecified coverage, not an empty map; no matching ranges "
        "does not prove data absent. For object layers this describes discovery-tile coverage, "
        "not object IDs. limit caps complete ranges; no tile I/O.",
        coverage,
        {"mapId", "layerId"});
    add("mapget_get_diagnostics",
        "Read a bounded timestamped operational snapshot. Requires separate global diagnostics "
        "permission; "
        "never triggers expensive cache scans or returns raw logs.",
        {{"sections",
          array({{"enum", {"workers", "memory", "cache", "transport", "sources"}}}, 5)}});
    add("mapget_get_config",
        "Read secret-masked datasource configuration, revision and persistence capabilities. "
        "Requires explicit configuration-read permission and deployment opt-in; excludes "
        "server/security settings.",
        Json::object());
    add("mapget_set_config",
        "Update datasource configuration only, preserving untouched sections and masked secrets. "
        "Requires config-write permission, administrator opt-in and direct-file persistence. "
        "Use the revision from get_config; never retry an uncertain mutation automatically.",
        {{"model", {{"type", "object"}}}, {"expectedRevision", string(128)}},
        {"model", "expectedRevision"},
        true);

    auto schema = selection;
    schema.update(query);
    schema["featureType"] = string();
    schema["schemaId"] = integer(1, simfil::MaxSchemaId);
    schema["schemaId"]["description"] =
        "Open a descriptor's $ref within this layer; mutually exclusive with featureType.";
    schema["query"]["description"] =
        "Simfil over descriptor metadata, e.g. fields.properties.fields or **.typename; "
        "evaluates the full lazy graph, not the default overview.";
    add("mapget_query_schema",
        "Inspect schema descriptors, not feature values. Without query, show feature fields and "
        "attribute-layer/name inventories, referencing attribute internals and other compound "
        "domains by $ref. schemaId opens one definition. fields/elements/alternatives distinguish "
        "objects, arrays and logical combinations; explicit queries are not overview-limited. "
        "No tile I/O.",
        schema,
        {"mapId", "layerId"});
    auto validation = selection;
    validation["expression"] = string(4096);
    validation["featureType"] = string();
    validation["attributeSchema"] = integer(1, UINT32_MAX);
    validation["scope"] = {{"enum", {"feature", "attribute", "auto"}}};
    validation["rewrite"] = {{"type", "boolean"}};
    validation["predicate"] = {{"type", "boolean"}};
    add("mapget_validate_expression",
        "Compile simfil with mapget functions and the chosen layer schema, without tile I/O. "
        "Optional search normalization uses the same schema normalization as /filter. "
        "Syntax validity is not proof of runtime success. Unresolved/dynamic access means "
        "metadata cannot prove a path, not that the field or data is absent.",
        validation,
        {"mapId", "layerId", "expression"});
    auto features = selection;
    features.update(query);
    features["partitions"] = array(partition, 32);
    features["partitions"]["minItems"] = 1;
    features["featureIds"] = array(string(2048), 100);
    features["featureIds"]["minItems"] = 1;
    features["featureTypes"] = array(string(), 64);
    features["scope"] = {{"enum", {"feature", "attribute"}}};
    features["predicate"] = string(4096);
    features["rewrite"] = {{"type", "boolean"}};
    features["expressions"] = array(string(4096), 16);
    features["geometry"] = {{"type", "boolean"}};
    add("mapget_extract_features",
        "Load complete tile/object partitions through the shared cache and query features or "
        "attribute contexts. "
        "Supply partitions or canonical primary featureIds (cheap locate, no map scan). "
        "query/expressions return all scalar/compound values per expression; query defaults to _. "
        "Attribute contexts expose $feature/$name/$layer/$attributeIndex/$validityIndex. "
        "geometry=true includes effective validity geometry. Provenance uses native mapget source "
        "references. "
        "Unsafe integers and bytes use explicit $mapget tags; no browser or pagination.",
        features,
        {"mapId", "layerId"});
    actions_["mapget_extract_features"].tool["inputSchema"]["anyOf"] =
        Json::array({{{"required", {"partitions"}}}, {{"required", {"featureIds"}}}});
    auto source = selection;
    source.update(query);
    source["partitions"] = array(partition, 32);
    source["reference"] = object(
        {{"layerId", string()},
         {"address", {{"type", "string"}, {"pattern", "^[0-9]{1,20}$"}}},
         {"qualifier", {{"type", "string"}, {"maxLength", 512}}}},
        {"layerId", "address"});
    source["partition"] = partition;
    source["match"] = {{"enum", {"exact", "containing"}}};
    add("mapget_extract_source_data",
        "Query native SourceData roots, or resolve reference {layerId,address,qualifier?} with "
        "mapId/partition provenance. "
        "address is the lossless decimal u64 native SourceDataAddress, not an erdblick inspection "
        "link. "
        "Exact matching is default; containing chooses minimal enclosing bit ranges and returns "
        "all ties. "
        "Target layers are independently authorized; opaque addresses support exact matching only.",
        source,
        {"mapId"});

    add("mapget_convert_tile_id",
        "Convert exactly one packed signed-int32 tileId, legacy decimal-string ID, grid "
        "{x,y,level}, "
        "or WGS84 {longitude,latitude,level}. Return packed ID, NDS Morton-grid coordinates and "
        "WGS84 bounds. "
        "Grid y is NDS order, not slippy-map/Web Mercator order. Object IDs and metadata tile zero "
        "are not tiles.",
        {{"tileId", integer(INT32_MIN, INT32_MAX)},
         {"legacyTileId", {{"type", "string"}, {"pattern", "^[0-9]{1,20}$"}}},
         {"x", integer(0, 65535)},
         {"y", integer(0, 32767)},
         {"level", integer(0, 15)},
         {"longitude", {{"type", "number"}, {"minimum", -180}, {"maximum", 180}}},
         {"latitude", {{"type", "number"}, {"minimum", -90}, {"maximum", 90}}}});
    add("mapget_convert_coordinates",
        "Convert WGS84 lon/lat degrees to signed NDS integer coordinates or back, using "
        "ndslive-math. "
        "No implicit axis swap, Mercator or arbitrary EPSG transforms. NDS conversion floors "
        "values; "
        "ndslive-math normalizes boundary coordinates.",
        {{"from", {{"enum", {"wgs84", "nds"}}}},
         {"x", {{"type", "number"}}},
         {"y", {{"type", "number"}}}},
        {"from", "x", "y"});
    add("mapget_lookup_place",
        "Search the offline Who's On First gazetteer by name. Returns stable wof: IDs, place "
        "types, WGS84 longitude/latitude, bounds and geometry availability, without loading "
        "polygons. Use mapget_get_place_geometry only when the full boundary is needed.",
        {{"name", string(200)}}, {"name"});
    add("mapget_get_place_geometry",
        "Resolve a stable wof: place ID, including its complete GeoJSON Polygon/MultiPolygon "
        "when available. Point-only records never fabricate a polygon. Large boundaries may "
        "exceed response budgets; an incomplete response retains metadata, never a partial ring.",
        {{"id", string(64)}}, {"id"});
    for (auto& [_, action] : actions_) {
        McpActionCatalog::compileSchema(action.input, action.tool["inputSchema"]);
        McpActionCatalog::compileSchema(action.output, action.tool["outputSchema"]);
    }
}

}  // namespace mapget::detail
