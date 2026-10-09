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
         {"traces", {{"type", "object"}}},
         {"nextOffset", integer(0, INT32_MAX)}},
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

    add("mapget_docs",
        "Search documentation and workflow examples by plain keywords, or read one exact returned "
        "title. Returns Markdown for the top three matches and up to five further titles. "
        "An empty call lists sections. Use component to restrict to classicsource, livesource, "
        "mapget, erdblick, mapviewer or simfil; offset/nextOffset page matches. "
        "Source edits reload before querying. revision identifies the corpus; no browser session "
        "required.",
        {{"query", string(4096)},
         {"title", string(2048)},
         {"component", string(128)},
         {"offset", integer(0, INT32_MAX)}});
    auto& docs = actions_["mapget_docs"].tool;
    docs["inputSchema"]["not"] = {{"required", {"query", "title"}}};
    docs["inputSchema"]["properties"]["limit"] = integer(1, 8);
    docs["inputSchema"]["properties"]["limit"]["default"] = 8;
    docs["outputSchema"]["properties"]["items"] = array(
        object(
            {{"title", string(2048)}, {"content", {{"type", "string"}}}, {"source", string(2048)}},
            {"title"}),
        8);
    docs["outputSchema"]["properties"]["revision"] = {{"type", {"string", "null"}}};
    docs["outputSchema"]["required"].push_back("revision");

    auto sources = selection;
    sources["details"] = {
        {"type", "boolean"},
        {"description",
         "Defaults to true when mapId or sourceId is supplied, false otherwise. Includes "
         "ordered feature ID compositions, protocol metadata and expanded raw SourceData "
         "layers. Explicit false requests compact metadata even for a selected source."}};
    sources["offset"] = integer(0, INT32_MAX);
    add("mapget_list_sources",
        "List authorized sources in configuration order with lifecycle state and layer metadata. "
        "mapId/sourceId selects detailed metadata by default; unfocused discovery is compact. "
        "Compact layers contains renderable layers; sourceDataLayers lists raw layer IDs. "
        "A layerId filter returns that layer's metadata, including raw layers. schemaFeatureTypes "
        "lists schema roots; featureTypes also includes reference-only identifier types. "
        "Excludes schemas and coverage records; no tile I/O. "
        "offset/nextOffset page matching sources in this snapshot; retain the same filters.",
        sources);
    auto coverage = selection;
    coverage["level"] = integer(0, 15);
    coverage["format"] = {{"enum", {"summary", "raw"}}, {"default", "summary"}};
    coverage["level"]["description"] = "Restrict advertised ranges to this NDS tile level.";
    add("mapget_get_coverage",
        "Discover a layer's advertised coverage without tile I/O. Default summary returns "
        "WGS84 bounds, exact coveredTileCount and up to eight covered sampleTileIds per range. "
        "A sample is advertised coverage, not a guarantee of feature payload. Sparse bounds "
        "include holes. format=raw returns full row-major filled masks (x then y); an empty raw "
        "mask means a full rectangle. min/max are packed grid corners, not an ID interval. "
        "coverageKnown=false means unspecified coverage, not an empty map. Incomplete results "
        "never prove absence. Object-layer coverage describes discovery tiles, not object IDs.",
        coverage,
        {"mapId", "layerId"});
    add("mapget_get_diagnostics",
        "Read backend worker, cache, memory and transport diagnostics without resetting state. "
        "Requires separate global diagnostics permission. Excludes browser metrics, expensive "
        "cache scans and raw logs.",
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
    schema["find"] = {
        {"type", "string"},
        {"minLength", 1},
        {"maxLength", 128},
        {"description",
         "Substring of one declared field/type/enum name, ignoring case, spaces, underscores and "
         "hyphens (speed limit matches SPEED_LIMIT or speedLimit). Use one keyword/name per call, "
         "e.g. speed or truck. Returns compact matches with schemaId and representative data path "
         "segments (* means array element). Not actual feature values or an exhaustive list of "
         "paths through shared definitions. Mutually exclusive with query."}};
    schema["schemaId"] = integer(1, simfil::MaxSchemaId);
    schema["schemaId"]["description"] =
        "Open a descriptor's $ref within this layer; mutually exclusive with featureType.";
    schema["query"]["description"] =
        "Simfil over descriptor metadata, e.g. fields.properties.fields or **.typename; "
        "evaluates the full lazy graph, not the default overview.";
    add("mapget_query_schema",
        "Inspect schema descriptors, not feature values; no tile I/O. Unfocused calls list "
        "feature types and schema IDs. featureType/schemaId opens immediate fields and shallow "
        "properties; childrenOmitted/$ref identifies unexpanded compounds. Explicit query/find "
        "is not overview-limited. find groups field/type and enum-symbol matches, with "
        "schemaId, owning contexts and grouped enum symbols. Exact symbols rank first. "
        "No pagination: use expandArguments or narrowing choices for broader results. "
        "fields/elements/alternatives distinguish objects, arrays and logical combinations. "
        "Large enum overviews return enumCount/enumPreview; explicit queries expose exact values.",
        schema,
        {"mapId", "layerId"});
    actions_["mapget_query_schema"].tool["inputSchema"]["allOf"] = Json::array(
        {{{"not", {{"required", {"featureType", "schemaId"}}}}},
         {{"not", {{"required", {"query", "find"}}}}}});
    actions_["mapget_query_schema"].tool["inputSchema"]["properties"]["limit"] = integer(1, 1000);
    actions_["mapget_query_schema"].tool["inputSchema"]["properties"]["limit"]["default"] = 8;
    actions_["mapget_query_schema"].tool["inputSchema"]["properties"]["limit"]["description"] =
        "Default 8; overview/find return at most 32 domains. Explicit queries may raise the value "
        "limit to 1000.";
    auto validation = selection;
    validation["expression"] = string(4096);
    validation["offset"] = integer(0, INT32_MAX);
    validation["featureType"] = string();
    validation["attributeSchema"] = integer(1, UINT32_MAX);
    validation["scope"] = {{"enum", {"feature", "attribute", "auto"}}};
    validation["rewrite"] = {{"type", "boolean"}};
    validation["rewrite"]["description"] =
        "Default false; auto scope also enables normalization. True resolves schema-backed "
        "search shorthand and attribute guards as viewer search does. This never disables "
        "ordinary schema compilation. Read normalizedExpression and the resolved scope.";
    validation["predicate"] = {{"type", "boolean"}};
    add("mapget_validate_expression",
        "Compile simfil with mapget functions and the chosen layer schema, without tile I/O. "
        "Optional search normalization uses the same schema normalization as /filter. "
        "Syntax validity is not proof of runtime success. Unresolved/dynamic access means "
        "metadata cannot prove a path, not that the field or data is absent. "
        "limit/offset page validation contexts; valid describes only the returned contexts. "
        "Follow nextOffset with unchanged selectors/expression for the rest.",
        validation,
        {"mapId", "layerId", "expression"});
    actions_["mapget_query_schema"].tool["outputSchema"]["properties"]["schemaView"] = {
        {"enum", {"shallow_overview", "matches"}}};
    actions_["mapget_query_schema"].tool["outputSchema"]["properties"]["guidance"] = string(1024);
    actions_["mapget_query_schema"].tool["outputSchema"]["properties"]["discovery"] = {
        {"type", "object"}};
    actions_["mapget_query_schema"].tool["outputSchema"]["properties"]["narrowing"] =
        array(Json::object(), 8);
    actions_["mapget_query_schema"].tool["outputSchema"]["properties"].erase("nextOffset");

    auto features = selection;
    features.update(query);
    features["partition"] = partition;
    features["partition"]["description"] =
        "Single partition from selection/coverage provenance. Mutually exclusive with partitions.";
    features["partitions"] = array(partition, 32);
    features["partitions"]["minItems"] = 1;
    features["featureIds"] = array(string(2048), 100);
    features["featureIds"]["minItems"] = 1;
    features["featureIds"]["description"] =
        "Full type-prefixed identifier strings, not local/numeric IDs. Ordered ID parts come "
        "from featureTypes[].uniqueIdCompositions in focused mapget_list_sources results.";
    features["partitions"]["description"] =
        "Array of {kind: tile, id: packedTileId} or {kind: object, id: decimalString}. Use actual "
        "IDs from selection or coverage, never tile level numbers.";
    features["cursor"] = string(48);
    features["cursor"]["minLength"] = 48;
    features["cursor"]["description"] =
        "Opaque nextCursor from a previous page. Supply only cursor and optional limit/maxWork; it "
        "retains the original query and scan position.";
    features["offset"] = integer(0, INT32_MAX);
    features["offset"]["description"] =
        "Skip this many matching feature/attribute contexts, after type and predicate filtering. "
        "Legacy random access; replays earlier predicates. Prefer nextCursor. Stateless: data "
        "changes between calls can change row order.";
    features["featureType"] = string();
    features["featureType"]["description"] =
        "Select one feature type within the layer, as in mapget_query_schema. Mutually exclusive "
        "with featureTypes.";
    features["featureTypes"] = array(string(), 64);
    features["featureTypes"]["description"] =
        "Select multiple feature types within the layer. Mutually exclusive with featureType.";
    features["scope"] = {{"enum", {"feature", "attribute"}}};
    features["attributeIndex"] = integer(0, INT32_MAX);
    features["attributeIndex"]["description"] =
        "In scope:attribute, select one exact assignment index from the feature summary. "
        "The index is local to each feature, not a lane number or schema ID.";
    features["attributeName"] = string();
    features["attributeName"]["description"] =
        "In scope:attribute, select assignments by exact reported name.";
    features["attributeLayer"] = string();
    features["attributeLayer"]["description"] =
        "In scope:attribute, select the exact assignment layer (not map layerId).";
    features["predicate"] = string(4096);
    features["predicate"]["description"] =
        "Boolean Simfil row filter; the argument is predicate, not filter. featureTypes filters by "
        "type before evaluation.";
    features["rewrite"] = {{"type", "boolean"}};
    features["rewrite"]["description"] =
        "Default false. True normalizes schema-backed predicate shorthand and attribute guards "
        "as /filter does. Ordinary schema compilation is always enabled.";
    features["expressions"] = array(string(4096), 16);
    features["geometry"] = {{"type", "boolean"}};
    features["query"]["description"] =
        "One Simfil data expression; _ selects the whole context, _sourceData its native "
        "source references. Mutually exclusive with expressions.";
    features["expressions"]["description"] =
        "Separate Simfil projections, each returning its own value sequence.";
    features["geometry"]["description"] =
        "Add effective row/validity geometry. False omits that extra output, but does not "
        "remove geometry selected by query _ or another expression.";
    features["scope"]["description"] =
        "feature evaluates one feature root; attribute evaluates individual assignments with "
        "their validity and $feature/$name/$layer/$attributeIndex/$validityIndex bindings.";
    add("mapget_extract_features",
        "Load complete tile/object partitions through the shared cache and query features or "
        "attribute contexts without changing the viewer. Supply partition/partitions or full "
        "primary/secondary featureIds (datasource locate, no map scan). "
        "Resolved items use canonical IDs; unmatched requested IDs appear in issues. "
        "In feature scope, omitting query/expressions returns summary: actual basic properties, "
        "attribute assignment names/indices/fields, geometry names/counts and relation "
        "names/counts, without nested payloads. Attribute scope defaults to the full assignment "
        "context. Explicit projections return value sequences; unsafe integers and bytes use "
        "$mapget tags. Continue with cursor:nextCursor and optional limit/maxWork only. "
        "Cursors expire after two minutes, may be evicted, and retain the active tile, not a "
        "whole-source snapshot. rowComplete=false marks a pending partial row. Without nextCursor, "
        "narrow the "
        "selection/projection or increase the exhausted budget.",
        features);
    actions_["mapget_extract_features"].tool["outputSchema"]["properties"]["nextCursor"] =
        string(48);
    actions_["mapget_extract_features"].tool["inputSchema"]["anyOf"] = Json::array(
        {{{"required", {"mapId", "layerId", "partitions"}}},
         {{"required", {"mapId", "layerId", "partition"}}},
         {{"required", {"mapId", "layerId", "featureIds"}}},
         {{"required", {"cursor"}}}});
    auto cursorOnly = object(
        {{"cursor", features["cursor"]},
         {"limit", budgets["limit"]},
         {"maxWork", budgets["maxWork"]}},
        {"cursor"});
    actions_["mapget_extract_features"].tool["inputSchema"]["allOf"] =
        Json::array({{{"if", {{"required", {"cursor"}}}}, {"then", cursorOnly}}});
    actions_["mapget_extract_features"].tool["inputSchema"]["not"] = {
        {"anyOf",
         Json::array(
             {{{"required", {"query", "expressions"}}},
              {{"required", {"featureType", "featureTypes"}}},
              {{"required", {"partition", "partitions"}}}})}};
    actions_["mapget_extract_features"].tool["inputSchema"]["if"] = {
        {"anyOf",
         Json::array(
             {{{"required", {"attributeIndex"}}},
              {{"required", {"attributeName"}}},
              {{"required", {"attributeLayer"}}}})}};
    actions_["mapget_extract_features"].tool["inputSchema"]["then"] = {
        {"required", {"scope"}},
        {"properties", {{"scope", {{"const", "attribute"}}}}}};
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
        "mapId/partition provenance. Obtain references with mapget_extract_features query "
        "_sourceData (or an attribute's _sourceData). The target must be a SourceData layer, "
        "not the original Features layer. "
        "address is the lossless decimal u64 native SourceDataAddress, not an erdblick inspection "
        "link or SQL row/column identifier. A resolved address may identify a decoded subrecord, "
        "not its SQL table row. Query layer roots separately for retained provenance metadata. "
        "Exact matching is default; containing chooses minimal enclosing bit ranges and returns "
        "all ties. "
        "Target layers are independently authorized; opaque addresses support exact matching only.",
        source,
        {"mapId"});
    actions_["mapget_extract_source_data"].tool["inputSchema"]["properties"]["maxWork"]["default"] =
        1000000;
    actions_["mapget_extract_source_data"].tool["inputSchema"]["oneOf"] = Json::array(
        {{{"required", {"reference", "partition"}}, {"not", {{"required", {"partitions"}}}}},
         {{"required", {"layerId", "partitions"}},
          {"not",
           {{"anyOf",
             Json::array({{{"required", {"reference"}}}, {{"required", {"partition"}}}})}}}}});

    add("mapget_convert_tile_id",
        "Convert exactly one packed signed-int32 tileId, legacy decimal-string ID, grid "
        "{x,y,level}, "
        "or WGS84 {longitude,latitude,level}. Return packed ID, NDS Morton-grid coordinates and "
        "WGS84 bounds. "
        "Grid y is NDS order, not slippy-map/Web Mercator order. Object IDs and metadata tile zero "
        "are not tiles. Classic maps also use packed tileId; Classic does not mean legacyTileId. "
        "legacyTileId is only for the removed mapget 0xXXXXYYYYZZZZ encoding.",
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
