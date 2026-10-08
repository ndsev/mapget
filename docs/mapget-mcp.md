# MCP Actions

Mapget exposes native data/query tools and a trusted browser-action catalog through
`POST /mcp`, routing viewer calls to a user's connected tabs. The feature is **disabled unless
explicitly configured**. It does not create another datasource, tile worker
pool, or browser session protocol.

The core browser tools are:

| Tool | Purpose | Permission |
| --- | --- | --- |
| `viewer_list_sessions` | List the caller's connected, compatible tabs | `viewer-read` |
| `viewer_describe_app_state` | Discover supported application-state channels | `viewer-read` |
| `viewer_get_app_state` | Read bounded state summaries | `viewer-read` |
| `viewer_set_app_state` | Assign supported application-state channels | `viewer-control` |

Browser tools are described by the installed catalog, not by C++ copies of
frontend schemas. Native `mapget_*` tools are independent of browser registration
and do not require `clientId`; see [Native Tools](#native-tools).

<!-- mcp:
title: "Viewer screenshot contract"
keywords: ["screenshot", "image", "readiness", "rendering", "capture"]
hint: "A delivered image is not proof of complete data or finished rendering; inspect its readiness metadata."
-->
### Viewer Image Results

`viewer_screenshot` is a browser-owned read action. Its browser result contains
`{image, metadata}`: a JPEG/base64 image plus capture dimensions, viewport/layout
identity, time, readiness and fidelity warnings. The relay validates this complete
result against the installed browser contract before projecting it for MCP.

For MCP, `tools/list` advertises the self-contained **metadata** schema as
`outputSchema`. `structuredContent` and the text block contain only that metadata;
the bytes appear once in a standard `ImageContent` block. They are not returned as
a large base64 text string, stored file, or externally fetched URL. This is one
explicit presentation mapping, not a different browser action or capture implementation.

Native framing requires canonical base64, JPEG MIME and at most 240000 encoded
characters. It also checks the complete outgoing response against `mcp-result-bytes`
(256 KiB by default), including duplicated metadata and protocol framing. Oversized
captures fail with `result_too_large`; the browser owns scaling/compression. Readiness
and fidelity are reported by the capture owner, not inferred from successful delivery.

<!-- mcp:
keywords: [MCP, local setup, connection, CLI, authentication]
-->
## Local Setup

The matching erdblick build generates `web-mcp-actions.json` beside `index.html`.
Enable local MCP using only the ordinary service options:

```sh
mapget serve --host 127.0.0.1 -p 8099 -w /path/to/viewer --mcp local
```

The same options work in MapViewer's `serve` command. No separate MCP config
file is needed. The default mode is `off`; opt-in is always explicit.
Local mode derives the endpoint and loopback Host/Origin allowlists from the
configured listener and port, never from request headers. Use a fixed port;
port zero cannot provide a predictable connection URL. The default catalog is
`<webapp directory>/web-mcp-actions.json`, including when the mount uses
`/viewer:/path/to/viewer` syntax. Without a webapp, omit the catalog for a headless native-tool server, or pass
`--mcp-catalog FILE` to additionally expose viewer tools. A configured catalog
that is missing or invalid still fails startup.

Every `--mcp-*` CLI option is also a flat YAML key under `mapget.serve`:

```yaml
mapget:
  serve:
    host: 127.0.0.1
    port: 8099
    webapp: /path/to/viewer
    mcp: local
sources: []
```

Run this with `mapget --config service.yaml serve`. Explicit CLI values take
precedence over YAML, which takes precedence over defaults. An explicit CLI
list **replaces** the YAML list; it does not expand an existing trust allowlist.
For example, `--mcp-timeout-ms 15000` overrides only the action deadline, and
`--mcp off` disables MCP while leaving hosted settings in the YAML intact.
Unknown options and invalid active settings fail startup.

Relative `mcp-catalog` and `mcp-jwks-file` paths in YAML resolve beside that
YAML file. Relative CLI overrides resolve from the process working directory.
The default catalog follows the actual webapp mount, whose path semantics are
unchanged. The catalog is a generated, trusted build artifact, not a second
operator-maintained configuration file.

Native embedders set `HttpServiceConfig::mcp` (`McpConfig` in
`mapget/http-service/mcp-config.h`). They can call `resolveDefaults(host, port,
webRoot)` before constructing the service, or provide explicit settings.
The service validates the same typed configuration; embedding-relative artifact
paths use the process working directory.

MCP configuration settings, including the trusted catalog **path**, are
**restart-scoped**. The catalog file's **contents** can change without restarting:
`GET /mcp/info`, MCP tool discovery/calls, and browser action registration check
its modification time and size. A changed artifact is fully loaded and validated
before replacing the active catalog. Missing, incomplete or invalid replacements
leave the last valid catalog active and log a warning; a subsequent file change
is retried. Publish completed artifacts atomically when possible.

After rebuilding the frontend, reload the browser to load its matching bundle
and register against the new catalog. Existing tabs using the previous catalog
are notified to reload and excluded from session discovery, but their tile
WebSockets stay open. Already admitted actions keep their original result
validators and deadlines; catalog replacement does not release mutation slots
or replay work. Identical catalog IDs do not disturb registrations. MCP clients
which cache tools may also need to refresh tool discovery.

The settings live in the private `mapget`
section, not in browser-writable datasource or public frontend configuration.
`GET /config` does not expose them; datasource `POST /config` preserves them,
and no MCP trust-setting writer is registered for `PATCH /config`.
The removed `--mcp-config` JSON-file interface is not supported.

Local mode gives explicitly allowed loopback clients one shared local identity.
It requires loopback-only listeners, a loopback socket peer, and exact Host
and Origin allowlists. Forwarding headers are rejected. Do not expose local
mode through a reverse proxy or use it as an OAuth fallback. Ordinary Docker
bridge port publishing does not satisfy this loopback policy. A browser action
connection must supply an allowed Origin; non-browser MCP callers may omit it.

`GET /mcp/info` returns `{"enabled":false}` when disabled. When enabled it returns
`enabled`, `endpoint`, `authentication`, `scopes`, `catalogId`, and an optional
`oauthClientId`. It never returns keys, subjects, or permission rules. The
endpoint URL comes from trusted configuration, not the caller's Host header.

<!-- mcp:
title: "Hosted MCP authentication"
keywords: ["OAuth", "login", "authenticated session", "permissions", "JWT", "proxy"]
-->
## Hosted Authentication

Hosted deployments configure mapget as an OAuth **resource server**. Login,
consent, user groups, and client registration belong to the authorization
server and deployment, not mapget. All MCP POSTs require a validated bearer
token; browser cookies and proxy identity headers do not authenticate them.

```yaml
mapget:
  serve:
    host: 127.0.0.1
    port: 8089
    webapp: /app/erdblick
    mcp: oauth
    mcp-endpoint: https://viewer.example/mcp
    mcp-allowed-hosts: [viewer.example, supplier.example]
    mcp-allowed-origins: [https://viewer.example, https://supplier.example]
    mcp-issuer: https://identity.example/realm
    mcp-jwks-url: https://identity.example/realm/keys
    mcp-required-scopes: [viewer]
    mcp-oauth-client-id: public-viewer-client
    mcp-read-claim: /viewer_permissions
    mcp-read-value: read
    mcp-control-claim: /viewer_permissions
    mcp-control-value: control
    mcp-trusted-proxy-addresses: [127.0.0.1]
    mcp-browser-issuer-header: x-viewer-issuer
    mcp-browser-subject-header: x-viewer-subject
    mcp-browser-expiry-header: x-viewer-expiry
    mcp-browser-permissions-header: x-viewer-permissions
sources: []
```

The audience is always `mcp-endpoint`; there is no independently configurable
second audience. Hosts/origins, issuer, signing-key source, scopes, permission
rules and proxy trust must be explicit in OAuth mode. Mapget does not infer
hosted trust from listener addresses, forwarding headers or issuer discovery.
An existing OAuth configuration cannot silently become local authentication:
`--mcp local` rejects retained OAuth settings rather than bypassing them.

The example claim path is illustrative. Each permission rule uses a JSON
Pointer and exact string membership: the claim may be one string or an array.
Read and control are independent permissions. Required scopes must also be
present in the space-separated `scope` claim.

The initial verifier supports **RS256 JWT access tokens**, using jwt-cpp and
OpenSSL for cryptography. It checks the configured issuer, exact resource
audience, signature, subject and expiry, plus optional `nbf`/`iat`. It does not
support opaque tokens, introspection, arbitrary signing algorithms, or
token-supplied key URLs. The audience must equal the configured MCP endpoint;
use different resource audiences for staging and production.

The administrator supplies exactly one `--mcp-jwks-url` or `--mcp-jwks-file`. The URL must
use HTTPS and is fetched with certificate validation, a five-second timeout,
no redirects, coalesced refreshes and a ten-second retry floor. Successfully
loaded keys expire after ten minutes; expired keys never receive an indefinite
stale-key grace period. A local JWKS file is immutable until restart. Tokens
are limited to 16 KiB; accepted key documents to 256 KiB and 32 public keys.
These are parser/acceptance bounds, not a claim that the HTTP library bounds
an issuer response before buffering it.

For viewer WebSockets, the reverse proxy must authenticate the browser, strip
client-supplied identity headers, and set the four configured headers from
verified claims. The permissions header contains the space-separated mapped
names `viewer-read`, `viewer-control`, `config-read`, `config-write`, and/or
`diagnostics`; the expiry is Unix **seconds**. Configuration and diagnostic permissions
are independent of viewer control. These verified headers also authorize `/mcp/browser`.
Mapget trusts them only from an exact configured socket-peer IP and the
configured issuer. Email is never an ownership key. Browser authority ends at
the earlier of the verified expiry and `mcp-browser-max-lifetime-seconds` after connection.
The server notifies the tab that action registration is unavailable when that
authority expires, without closing the normal tile WebSocket. Reconnecting
obtains fresh authority and a new viewer UUID.

Do not expose the backend directly to untrusted callers in a topology where
they can impersonate an allowed proxy peer. Do not rely on `X-Forwarded-For`
for this trust boundary. Keep proxy identity validation for WebSockets and
bearer validation for `/mcp` separate.

Protected-resource metadata is served without login redirects at
`/.well-known/oauth-protected-resource/mcp` and
`/.well-known/oauth-protected-resource`. Invalid tokens receive HTTP 401;
insufficient scopes/permissions receive 403. Both carry a `WWW-Authenticate`
challenge with the metadata URL and required scopes. Authentication failures
do not echo tokens or claims.

## Configuration Reference

All names below work as `--name` CLI options and as `mapget.serve.name` YAML
keys. CLI list options accept multiple space-separated values or repeated
occurrences; YAML lists use the usual sequence syntax.

| Setting | Default | Meaning |
| --- | --- | --- |
| `mcp` | `off` | `off`, loopback-only `local`, or `oauth` |
| `mcp-endpoint` | Local listener URL | Canonical public resource URL ending in `/mcp`; also the required audience |
| `mcp-catalog` | `<webapp>/web-mcp-actions.json` | Trusted generated catalog |
| `mcp-help-docs` | None | Additional trusted Markdown directories/files; supplements bundled docs and `<webapp>/mcp-help`; contents hot-reload |
| `mcp-allowed-hosts` | Local loopback names and port | Exact HTTP Host allowlist; required for OAuth |
| `mcp-allowed-origins` | Local loopback HTTP origins | Exact browser Origin allowlist; required for OAuth |
| `mcp-issuer` | None | Trusted HTTPS issuer |
| `mcp-jwks-url` | None | Trusted HTTPS signing-key URL; exclusive with `mcp-jwks-file` |
| `mcp-jwks-file` | None | Local public JWKS artifact, immutable until restart |
| `mcp-required-scopes` | None | Non-empty required OAuth scope list (at most 16) |
| `mcp-oauth-client-id` | None | Optional public client ID in connection hints |
| `mcp-clock-skew-seconds` | `15` | JWT clock tolerance, 0..60 seconds; expired authority is still rejected |
| `mcp-read-claim`, `mcp-read-value` | None | JSON pointer and exact member granting native data reads and viewer-read |
| `mcp-control-claim`, `mcp-control-value` | None | JSON pointer and exact member granting viewer-control; at least one complete rule is required |
| `mcp-config-read` | `false` | Opt in to masked datasource-config reads; ordinary GET /config must also be enabled |
| `mcp-config-write` | `false` | Opt in to datasource-config edits; requires `--allow-post-config` and direct persistence |
| `mcp-direct-config-persistence` | `false` | Operator assertion that the server config file is the durable configuration, not a transformed wrapper copy |
| `mcp-config-read-claim`, `mcp-config-read-value` | None | Independent verified permission for masked configuration reads |
| `mcp-config-write-claim`, `mcp-config-write-value` | None | Independent verified permission for configuration writes |
| `mcp-diagnostics-claim`, `mcp-diagnostics-value` | None | Independent verified permission for global server diagnostics |
| `mcp-datasource-header-claims` | Empty | At most 16 `lowercase-header=/claim/pointer` mappings into datasource authorization, from verified scalar JWT claims only |
| `mcp-trusted-proxy-addresses` | None | Exact socket-peer IPs permitted to supply browser identity |
| `mcp-browser-issuer-header` | None | Lowercase header name carrying verified issuer |
| `mcp-browser-subject-header` | None | Lowercase header name carrying verified subject |
| `mcp-browser-expiry-header` | None | Lowercase header name carrying verified expiry in Unix seconds |
| `mcp-browser-permissions-header` | None | Lowercase header name carrying mapped permission words |
| `mcp-browser-max-lifetime-seconds` | `3600` | Cap on retained browser authority, 1..86400 seconds |
| `mcp-timeout-ms` | `30000` | Call deadline, additionally capped by caller/browser authority |
| `mcp-invocation-bytes` | `65536` | Complete invoke or registration envelope |
| `mcp-result-bytes` | `262144` | Browser result envelope/native serialized tool-result budget |
| `mcp-calls-per-session` | `4` | Outstanding calls per viewer tab |
| `mcp-calls-per-principal` | `16` | Per-user limit, enforced separately for retained native and viewer calls |
| `mcp-pending-calls` | `128` | Pending HTTP responses; also bounds retained native and viewer calls separately, including uncertain mutations |
| `mcp-sessions` | `256` | Attached action peers, including unregistered tabs |

Limits are positive integers at most 2147483647. Host/origin lists are bounded
to 32 entries each. Identity header names must be distinct. These checks also
apply to native embedders; CLI/YAML is not a separate authorization path.

<!-- mcp:
title: "Viewer routing and action lifetimes"
keywords: ["clientId", "session", "tab", "deadline", "timeout", "cancellation", "mutation"]
-->
## Tool Routing And Lifetimes

Each `/interactive` WebSocket receives a cryptographically random UUIDv4
`clientId` in its initial `RequestContext`, before any tile request. `/tiles`
is the same WebSocket alias. That UUID also targets browser actions: there is
no separate viewer-session ID.

A tab advertises catalog ID, action names, and a display label. Its catalog
must match the server's installed artifact. The tab cannot provide new tools,
schemas, permission rules, or executable server code. The entire
`mapget.actions.*` namespace is routed before tile request parsing, including
invalid or unknown action messages.

`viewer_list_sessions` returns `{"sessions":[...]}` in MCP `structuredContent`.
Each record contains `clientId`, `label`, `origin`, `catalogId`, available
`actions`, and `mutationBusy`. Only compatible, live tabs with the caller's
same **(issuer, subject)** are visible. Origin is descriptive, not a partition
of ownership: one user can see tabs from multiple allowed frontends.

Every browser call includes the selected `clientId` in its arguments. Mapget
validates ownership, permissions, action availability and the trusted input
schema, then strips `clientId` before invoking the browser. Results are
validated against the trusted output schema. No implicit last-selected tab
or reconnect fallback exists.

Server-to-browser actions use VTLV `ActionControl` type 9 containing JSON;
browser-to-server controls are WebSocket JSON text frames. They bypass tile
request replacement, tile outboxes, and datasource workers. Tile binary
protocol compatibility and the action envelope's `version: 1` are separate
contracts.

An HTTP timeout is not proof that a camera mutation stopped. A timed-out or
cancelled mutation continues to occupy its tab's mutation slot until a
matching terminal browser reply or disconnect. It still counts against
admission limits after the HTTP callback is released. Late results never
resurrect an HTTP response, and duplicate replies do not disable the tab.
Mapget never automatically retries mutations with an uncertain outcome.

Only one mutation may run per tab; reads may run concurrently within these
bounds. HTTP JSON input is independently limited to 64 KiB, and external
control admission to 256 queued inputs. JSON nesting and duplicate object
members are checked before downstream parsing. There is no waiting queue of
camera mutations beyond the admission limits.

## MCP Transport Revisions

The endpoint supports these distinct wire contracts over the same POST URL:

- `2025-06-18` and `2025-11-25`: `initialize`,
  `notifications/initialized`, `ping`, `tools/list`, and `tools/call`.
  Subsequent POSTs include the negotiated `MCP-Protocol-Version` header.
- `2026-07-28`: stateless `server/discover`, `tools/list`, and `tools/call`;
  version/client metadata lives in `params._meta`. Required `Mcp-Method`,
  `Mcp-Name` for tool calls, and protocol headers must match the body.

No revision creates an MCP protocol session or returns `MCP-Session-Id`.
The viewer `clientId` must not be used as such a header. No GET event stream,
replay streams, protocol tasks, prompts, or resources are advertised. Feature extraction
supports bounded tool-level continuation cursors; this does not create an MCP protocol
session. Sources, documentation and validation contexts also support stateless
`offset`/`nextOffset` paging. Schema discovery uses compact navigation, without pagination.
`GET /mcp` and `DELETE /mcp` return 405.

Clients accept both JSON and SSE. Immediate results may use JSON; a browser
call returns one terminal JSON-RPC response on its POST's SSE stream and then
closes it. `structuredContent` carries the application result and `content`
also contains its JSON text. Application failures use `isError: true` rather
than pretending to be successful state. The `resultType` field is emitted
only for the 2026 revision.

Cancellation is intentionally revision-specific. In 2026, closing the SSE
caller requests browser cancellation. In 2025, connection loss only releases
the HTTP waiter; accepted browser work runs until its acknowledgement or
bounded deadline. `notifications/cancelled` is accepted but **not acted on**:
without MCP protocol sessions, different agents belonging to the same user
can reuse an RPC request ID, so cancelling by that ID would risk stopping
another agent's work. Neither path releases an uncertain mutation prematurely.

MCP endpoint, metadata, and connection-hint responses use `Cache-Control:
no-store`. Reverse proxies must pass Authorization and MCP headers, route
the well-known metadata paths, allow WebSocket upgrades, disable SSE buffering,
and allow the configured call deadline plus margin. This integration does
not change the existing UUID-capability policy of `/interactive/payload`.

## Validation

Native catalog/relay/auth tests use `[mcp-actions]` in `test.mapget`. OAuth
tests generate disposable RSA keys and test actual signatures and claims,
not hardcoded accepted tokens. `test-native-mcp` runs a separate native CLI
and real HTTP/WebSocket connections using only Python's standard library. It also
checks CLI/YAML overrides, config-relative paths, private configuration and the
default catalog filename:

```sh
build/bin/test.mapget '[mcp-actions]'
ctest --test-dir build -R '^test-native-mcp$' --output-on-failure
```

The transport test is not a real frontend, identity-provider login, or hosted
deployment test. Consumer acceptance additionally requires the matching viewer
catalog/build, real browser actions, and the deployment's OAuth client flow.

## Native Tools

All native input schemas have closed object roots. `tools/list` is the authoritative
machine-readable catalog. Permission-filtered listing does not replace call-time checks.

| Tool | Inputs beyond common budgets | Result |
| --- | --- | --- |
| `mapget_docs` | Optional `query` or exact returned `title`, `component`, `offset` | Up to three complete Markdown sections and five further titles per page, plus content `revision` and optional `nextOffset`; no browser required |
| `mapget_list_sources` | Optional `mapId`, `layerId`, `sourceId`, `details`, `offset` | Ordered source/layer identities, lifecycle/progress, partition kind, levels, feature type names and coverage-range counts; selected `mapId`/`sourceId` defaults to details, including ordered ID compositions and protocol metadata |
| `mapget_get_coverage` | `mapId`, `layerId`; optional `sourceId`, `level`, `format` | Coverage summaries with bounds, counts and sample tile IDs; `format: "raw"` returns exact sparse occupancy masks; `coverageKnown` distinguishes unspecified coverage |
| `mapget_query_schema` | `mapId`, `layerId`; optional `sourceId`, `featureType` or `schemaId`, `query` or `find`, `trace` | Compact grouped field/enum discovery or shallow feature/attribute overview by default; focused sequences of descriptor metadata with navigable `$ref` identities |
| `mapget_validate_expression` | `mapId`, `layerId`, `expression`; optional `sourceId`, `featureType`, `attributeSchema`, `scope`, `rewrite`, `predicate`, `offset` | Compilation and schema-access assessment per context, normalized expression, diagnostics; no tile I/O |
| `mapget_extract_features` | `mapId`, `layerId`; `partition`, `partitions` or canonical `featureIds`; optional `sourceId`, `featureType` or `featureTypes`, `scope`, `attributeIndex`, `attributeName`, `attributeLayer`, `predicate`, `rewrite`, `query` or `expressions`, `geometry`, `cursor`, legacy `offset`, `trace` | Compact actual feature summaries by default; explicit feature/attribute projections with provenance, row completeness and bounded scan continuations |
| `mapget_extract_source_data` | `mapId`, `layerId`, `partitions`, or `mapId`, `partition`, `reference`; optional `sourceId`, `match`, `query`, `trace` | Root/address-match rows, provenance and value sequences |
| `mapget_convert_coordinates` | `from`: `wgs84` or `nds`, `x`, `y` | WGS84 degrees or signed NDS integer coordinates |
| `mapget_convert_tile_id` | Exactly one of `tileId`, `legacyTileId`, `{x,y,level}`, `{longitude,latitude,level}` | Signed packed ID, grid coordinates, level, WGS84 bounds and center |
| `mapget_lookup_place` | `name`, optional `limit` | Up to 50 compact WOF matches: IDs, countries/regions/localities, coordinates, extents and geometry availability; never polygons |
| `mapget_get_place_geometry` | `id` | Exact-ID metadata plus an available complete GeoJSON boundary; response budgets never truncate rings |
| `mapget_get_diagnostics` | Optional `sections`: `workers`, `memory`, `cache`, `transport`, `sources` | Timestamped lightweight status snapshots, not raw logs or expensive cache reports |
| `mapget_get_config` | None | Masked datasource `model`, file `revision`, persistence mode |
| `mapget_set_config` | `model`, `expectedRevision` | Persistence/reload acknowledgement and new revision; datasource initialization remains asynchronous |

Native data tools require the configured read permission. Configuration read/write and
global diagnostics use **separate** permissions: viewer control never grants them.
Local mode grants these permissions to its shared loopback identity, but config access
still requires the deployment opt-ins. OAuth defaults do not grant the new administrative
permissions. Configure their claim/value pairs explicitly when desired.

Datasource ACLs remain in effect. For example, `mcp-datasource-header-claims:
["x-user=/email"]` supplies the verified JWT email to an existing `x-user` datasource
ACL. Missing/non-string claims are omitted; oversized or newline-containing values reject
authentication. The MCP caller cannot supply these headers as tool arguments, and incoming
proxy/caller headers are not forwarded to datasource authorization. Layer/partition loading
is authorized again after catalog selection. `sourceId` disambiguates authorized sources
sharing a map/layer; it does not bypass ACLs. Source-reference targets are authorized
independently. Never return the JWT itself to the datasource.

<!-- mcp:
title: "Native source and layer discovery"
keywords: ["sourceDataLayers", "source discovery", "owning layer", "schemaFeatureTypes"]
-->
### Source And Layer Discovery

Unfocused `mapget_list_sources` keeps renderable layer metadata in `layers` and lists raw
SourceData IDs in `sourceDataLayers`. Selecting `mapId` or `sourceId` automatically includes
ordered primary/secondary identifier compositions, protocol metadata and expanded raw layers.
`details:false` explicitly requests the compact form, even with these selectors;
`details:true` explicitly expands an unfocused inventory. A `layerId` filter alone opens
that layer's metadata (including raw layers), but does not enable source details.
Both forms omit the potentially large feature-model schema and coverage records.
`schemaFeatureTypes` lists schema roots; `featureTypes` also contains identifier definitions
for reference-only types. Neither schema declarations nor a documentation support table proves
that a particular record exists in a loaded map.

<!-- mcp:
keywords: [schema discovery, query, feature type, attributes, filtering, cardinality]
-->
### Designing Search Filters

Start with `viewer_get_app_state` and its active `view.layers` map/layer identities.
Layer names are not feature types: a `Road` layer can contain `Road`, `Intersection`,
and indirect attribute features. Use `mapget_list_sources` filtered to that layer
for compact type names. `featureTypes` advertises identifier compositions, which
can include types used only by references. `schemaFeatureTypes` lists the actual
feature schema roots available for introspection in that layer. Neither list proves
that a particular tile contains instances. If a type is absent from the current
layer's roots, check other layers in the same map before concluding it is unavailable.
Focused `mapId`/`sourceId` discovery includes ordered primary/secondary ID compositions
and optional/synthetic parts by default; they cannot be recovered from the feature-field
schema alone. Source pages use `offset`/`nextOffset` with
unchanged filters. A source catalog may change between calls.

Coverage is a separate request via `mapget_get_coverage`, optionally restricted
to one NDS level. The default summary gives WGS84 bounds, an exact
`coveredTileCount` and up to eight actually covered `sampleTileIds` per range.
Advertised coverage does not guarantee a nonempty feature payload. Its `min`/`max`
are south-west/north-east **grid corners**, not a numeric interval of packed IDs.
Use `format: "raw"` for the full row-major boolean `filled` mask (x increasing,
then y increasing); an empty mask denotes a full rectangle and an all-false
nonempty mask denotes an empty rectangle. A partial sparse mask is never returned.
`coverageKnown: false` means unspecified coverage, not an empty map. An empty
level-filtered result means no range was advertised for that level. Object-layer
coverage describes discovery tiles, not object IDs or exact object geometry.
Summary `bounds` are `[west, south, east, north]`. Full longitude uses west/east
`-180..180`; a range crossing the packed latitude grid's pole discontinuity uses
the conservative south/north bounds `-90..90`.
These geographic bounds summarize the grid range and do not replace its sparse mask.

Call `mapget_query_schema` without a query or selector for a shallow list of
feature types and schema IDs. Select `featureType` to open its fields and shallow
properties. Follow a numeric `$ref` using `schemaId` in the same layer to open a
compound definition and its immediate fields. An implicit union's `$ref` lists
alternative IDs; follow each integer separately. Large enums use `enumCount` and
`enumPreview`; an explicit query on that schema can retrieve the full domain.
For example:

```json
{"mapId":"Very-Large-Map","layerId":"Road","featureType":"Intersection",
 "query":"fields.properties.fields.connectedRoads"}
```

Explicit queries always evaluate the full lazy descriptor graph, **not** the
overview projection. Descriptor expressions address `fields`, `elements`, and
`alternatives`, not feature data paths; `fields` is a member, not a function.
`query: "typename"` lists concrete producer names where supplied, whereas the
result's `featureType` identifies each root even without a typename. `query: "_"`
returns recursive details within the ordinary work/byte/time limits. Cycles become
`$ref` descriptors with `truncated: "cycle"`; shared acyclic branches remain
queryable. An exhausted descriptor allocation budget makes the whole result
`complete: false`, even when a scalar projection hides its `node-budget` marker.
Default descriptor responses identify `schemaView: "shallow_overview"`.
`childrenOmitted: true` marks nonempty compound fields/elements/alternatives that
were not expanded; open their `$ref` with `schemaId`, or use `find` to discover a
field across the selected type/layer. `complete: true` means the overview response
is complete, not that every descendant is displayed. An unexpanded reference does
not imply missing data.

`connectedRoads` is an array, so its cardinality filter is
`#properties.connectedRoads > 3`. At a feature root, `attributes` is a lookup alias
for `properties`; `#attributes.connectedRoads > 3` is equivalent unless the model
declares an actual `attributes` member. Static validation and completion use the
same alias while returning canonical property paths. Validate with
`mapget_validate_expression` and `scope: "auto"`, then run `viewer_start_search`
over the active map/layer. Unresolved metadata indicates uncertainty, not an
invalid runtime field. For runtime sampling, `mapget_extract_features` requires
explicit nonempty partitions or canonical feature IDs; it never scans a map.

<!-- mcp:
keywords: [partial results, limits, extraction, integer, bytes]
-->
### Results And Budgets

Input-schema rejections include bounded field-level issues. `conflictingFields` names a
supplied combination prohibited by the input schema. Remove at least one of those fields.
`triggerFields` explains a selector dependency: `missingField` names a required companion and
`expected.const` supplies its required value. For example, `attributeName` requires
`scope:attribute`; `featureType` and `schemaId` select alternative schema roots.

Success is an object in `structuredContent`, with the same JSON in a text content block:

```json
{"items": [], "complete": true, "reason": null, "issues": [], "traces": {}}
```

Failures use `isError: true` and a structured error. A partial enumeration is a successful
bounded response with `complete: false` and a reason such as `item_limit`,
`expression_result_limit`, `work_limit`, `schema_node_limit`, `byte_limit`, `query_error`, or
`load_failed_or_cancelled`. Follow `nextCursor` for feature extraction, or a returned
`nextOffset` for tools supporting offset paging. Schema discovery offers `narrowing`
and `expandArguments` instead. No tool promises a whole-source snapshot or automatic
continuation. No matches is a valid empty result, but incomplete output cannot prove absence.
Repeated evaluator warnings with the same expression, message and source span are
aggregated into one issue with `expression` and `occurrences`. Counts refer to
emitted diagnostics, not necessarily distinct features. Repeated instances occupy one slot in the shared 100-issue budget;
optional fields missing on many records do not themselves
stop an otherwise bounded scan. Predicate decisions consume evaluator work but
are not charged as returned payload bytes.

Common inputs are `limit` (default 100, maximum 1000) and `maxWork` (default 100000,
maximum 1000000). SourceData extraction defaults to 1000000 because resolving an
address scans the owning compound records, which can exceed 100000 in a single tile.
An explicit smaller budget is still honored.
`limit` caps rows **and** values per expression separately; coverage uses it for whole ranges.
There is no public depth knob. A defensive serialization stack guard at 256 container
levels reports `serialization_limit`, rather than returning a silently shortened value.
Simfil's independent evaluator safety guard reports `evaluation_limit`.
Partition inputs are capped at 32, feature IDs at 100, expression lists at 16 and expression
text at 4096 characters. The configured MCP deadline, per-principal/overall admission caps,
and result-byte cap also apply. JSON conversion shares the work/byte budget with
simfil evaluation. Native result construction uses at most one third of the wire allowance
(capped at 1 MiB), reserving space for text fallback escaping and envelope duplication;
actual serialized size is checked before sending.

These are cooperative guards, **not a hard sandbox**: datasource I/O, compilation, regex
engines, custom functions and individual model accessors can contain non-preemptible work.
A client timeout does not imply rollback of a config mutation. Do not retry uncertain writes
without rereading the revision. No response-task continuation or paging snapshot is retained.

JSON values preserve zero, one or many expression results as arrays. Actual arrays remain
nested arrays; objects remain objects. Non-JSON/lossy scalars use explicit tags:

```json
[
  {"$mapget":"undefined"},
  {"$mapget":"int64","value":"9223372036854775807"},
  {"$mapget":"uint64","value":"18446744073709551615"},
  {"$mapget":"bytes","hex":"00ff"},
  {"$mapget":"nonfinite","value":"Infinity"}
]
```

A genuine object containing `$mapget` is escaped as `{"$mapget":"object","value":{...}}`.
Integers within JavaScript's exact range remain JSON numbers. Null remains null;
simfil's missing-field behavior is not rewritten (a missing field can yield null and a
diagnostic). Repeated object fields retain mapget's array/`_multimap` projection.

`trace: true` captures simfil `trace(...)` samples immediately into bounded JSON, rather
than keeping source models alive. At most 16 names, 100 samples per name, and 512 characters
per name are allowed. Calls, elapsed microseconds and sample truncation are reported.
Without the flag, `trace` still forwards values but does not accumulate samples.

<!-- mcp:
keywords: [schema, descriptor, validation, completion, simfil]
-->
### Schema And Expression Semantics

`mapget_query_schema` uses simfil's lazy `SchemaModel` on the same `LayerSchema` graph used
by completion. Descriptors contain `kind`, `fields`, `elements`, `alternatives`, enum values,
open/nullable/required flags and terminal recursion/budget markers as appropriate.
Array elements are possible domains, not fabricated sample records; union alternatives are
distinct from arrays. Omitting `query` returns the shallow overview; explicit
queries evaluate the full lazy graph. For example:

```json
{"mapId":"Example","layerId":"Road","featureType":"Road","query":"fields"}
```

Use `find` for direct field/symbol discovery before walking many references:

```json
{"mapId":"Example","layerId":"Road","find":"speed","limit":20}
```

`find` matches declared field names, producer type names and enum symbols,
ignoring case, spaces, underscores and hyphens. For example, `speed limit` can
match `SPEED_LIMIT` and `speedLimit`. Exact enum symbols rank first, followed by
exact field/type names and substring matches. Within each class, shorter paths
bring owning assignments ahead of deeply nested generic fields. This supports
both finding the values of a known field and locating the field that accepts a known classification.

Matches are grouped by `schemaId`, with up to two representative owning `contexts`.
Each context has a `path` (`*` means an array element); feature-root searches also
return `featureType`, a copyable `featureQuery`, and `attributeContext` when known.
A search starting at `schemaId` returns paths relative to that definition rather
than inventing a feature-root query. `contextsOmitted` identifies encountered contexts omitted from this preview;
false does not establish that these are every possible owner.
Shared definitions are traversed once per root, so these are representative paths,
not an exhaustive inventory of every route through the graph.

Enum groups include `matchingSymbols`, their count, and an eight-symbol `preview`.
Exact symbols remain visible even when many substring matches are omitted.
Symbols describe actual string literals; flag domains identify their symbols as
flags, not an exhaustive list of flag combinations. Separate `literalPreview`
values retain non-string enum literals, including lossless tags for unsafe integers.
The schema does not infer a numeric source-format encoding for a string symbol.
`valuesArguments` opens the full enum using a focused query, subject to the normal
value/work/byte budgets. `expandArguments` opens the definition's shallow structure.

Discovery defaults to eight groups and allows at most thirty-two. It has **no
pagination**. `discovery.scanComplete` says whether the traversal finished;
`matchedDomains` and `omittedDomains` are lower bounds when it did not. A completed
scan with omitted groups returns `complete:false, reason:"matches_omitted"`.
Use the returned `narrowing[].arguments` to focus on an observed feature type or
assignment/domain, or use one group's `expandArguments`. A work-limited scan is
explicitly incomplete even if it found no matches.

Overviews show at most sixteen immediate children per container and eight enum
values. `childrenOmitted`, `fieldCount`/`domainCount`, and `enumCount` identify larger
domains. Follow `$ref`/`schemaId`, use `find` for an omitted named member or symbol,
or request a focused descriptor `query`. Explicit queries can raise `limit` to
1000 values; they remain bounded and do not paginate. `find` and `query` are
exclusive. Schema declarations do not establish actual feature values; open or
dynamic data can also contain undeclared fields.

If the default overview returns `complete: false`, especially `reason: "byte_limit"`,
its missing feature types or fields are **not** evidence of absent data. Even an
empty `items` array can mean that the first descriptor exceeded the budget.
Discover feature type names with `mapget_list_sources`, select one with
`featureType`, and project a narrow descriptor path with `query`. For example,
a Classic Routing layer with declared `Link.properties.travelDirection` can be
inspected with `featureType: "Link"` and
`query: "fields.properties.fields.travelDirection"`. Follow a returned `$ref`
using `schemaId` in that same source/layer. Check completeness on each read.
Schema descriptors establish types and possible domains; use bounded feature
extraction to establish actual values and missing-value behavior.

Expression validation instead binds **data** schemas to the compiler. It never evaluates
against fake features or fetches map data. `scope` is `feature`, `attribute`, or `auto`;
`rewrite: true` requests the existing search normalization. Auto scope also uses normalization
when metadata is available. An explicit `attributeSchema` must identify a real attribute context.
Without it, available attribute contexts are compiled separately. `limit` bounds
contexts, `offset` selects the first, and `nextOffset` continues with the same
expression and selectors. `contextCount` describes the available contexts;
`valid` describes only those returned on this page. `schemaCertain` reports
statically resolved accesses against a closed root, not a guarantee that runtime data/functions
will succeed. `runtimeValidated` is always false. Missing/open/dynamic metadata stays uncertain.

<!-- mcp:
keywords: [extract features, source data, provenance, native reference]
-->
### Extraction And Native Source Links

Tile partitions are `{"kind":"tile","id":131073}`; object IDs are lossless decimal strings,
for example `{"kind":"object","id":"18446744073709551615"}`. Feature IDs are canonical
mapget strings beginning with the exact feature type, with source-specific ID parts
separated by dots. A numeric/local ID alone is incomplete.
`mapget_list_sources` with the target `mapId`, `layerId` and `details:true` returns
`featureTypes[].uniqueIdCompositions`. Malformed IDs return `invalid_feature_id`
with identity-specific recovery guidance before any tile loading; this is distinct
from a valid ID that cannot be located.
The datasource's cheap locate hook supplies candidate partitions
and optional secondary-ID selectors. Ordinary service/cache loading resolves
primary IDs or applies those selectors to the loaded tile; returned rows retain
canonical primary identities. Explicit partitions restrict candidate loading.
Unresolved requested IDs are reported as locate issues, not silently treated as
an empty successful lookup. There is no coverage-wide scan, object discovery or
direct datasource conversion. Supply explicit partitions when cheap locate is
unavailable.
Use `partition` for one partition or `partitions` for several; these spellings are
mutually exclusive. Likewise, `featureType` selects one type and `featureTypes`
selects several. Omitted/empty `featureTypes` searches all types; an omitted
`predicate` matches all candidates. Feature type names are not layer IDs: discover
the owning layer before following a relation into another type.

```json
{
  "mapId":"Example", "layerId":"Road",
  "partitions":[{"kind":"tile","id":131073}],
  "predicate":"typeId == 'Road'",
  "expressions":["id", "properties", "properties.layer.rules.speed.limit"],
  "geometry":true
}
```

Feature rows include source/map/layer/partition/feature ID provenance and
`rowComplete`. In feature scope, omitting both `query` and `expressions` returns a
compact `summary` of the actual feature:

- `typeId` and named `idParts` identify the concrete record.
- `properties` lists immediate fields, scalar values and compound child counts.
- `attributeAssignments` preserves layer/name, global attribute index, optional
  layer instance ID, validity count and immediate assignment fields.
- `geometries` lists semantic names, types and point counts without coordinates.
- `relationCount`, `relationNames` and `sourceDataReferences` guide focused follow-up.

In the property and assignment inventories, long strings and byte buffers are represented by their sizes. This is an inventory
of the returned feature, not proof that all features in a layer share those fields.
Use a single string `query` for a focused projection, `expressions` for several,
or explicit `query: "_"` to request the complete model. Feature scope starts at the
feature: use `_.properties`, not `$feature.properties` or `$.properties`.

A partial extraction can return `nextCursor`. Continue with the same tool and
only the cursor, optionally changing `limit` or `maxWork`:

```json
{"cursor":"<nextCursor from the previous response>","limit":20}
```

The cursor retains the original selection, projections and actual scan position,
including the feature, attribute assignment and validity within the active
partition. Earlier partitions and predicates are not replayed. A work-limited page
can contain no matches and still return an advancing cursor. A retry of a cursor
starts from the same checkpoint; it does not consume or advance that cursor.

A partially projected row has `rowComplete:false` and remains pending. If a fresh
page cannot finish that row without advancing, no new cursor is returned: narrow
the projection in a new request or increase `limit` for `expression_result_limit` / `maxWork` for a work limit. Cursor
calls reject selection/projection changes rather than silently mixing queries.

Cursors expire after two minutes (or earlier with their originating authority),
are scoped to the caller and datasource authorization, and fail explicitly after
catalog changes. Storage is bounded to four checkpoints per caller, sixteen
process-wide, and 64 MiB of conservatively accounted tile/query storage; older
checkpoints may be evicted sooner. Shared datasource schemas and dictionaries are
service-owned and are not charged per checkpoint. Obsolete catalog generations
are retired by worker maintenance. Oversized checkpoints are not retained.
The active tile is retained; later partitions are loaded normally. This provides
stable positions within that tile, **not a snapshot of the whole live source**.
Restart the original extraction after `cursor_unavailable` or `cursor_stale`.

Legacy `offset`/`nextOffset` remains available for matching-row random access, but
repeats earlier scanning and predicates. It counts contexts after type/predicate
filtering and advances only past complete rows. Retain filters and partition order;
a changing source can change the sequence. Prefer cursors for sequential reads.

With `scope: "attribute"`, use `attributeIndex` from the summary to inspect one
assignment, or exact `attributeName` and `attributeLayer` to select assignments.
These selectors combine with AND and require attribute scope. Indices are local
to each feature; `attributeLayer` is the assignment layer, not the map `layerId`.
They apply before the predicate and matching-row pagination. A focused `query`
still helps when an assignment contains large nested payloads.

Attribute scope iterates
the same feature-local attribute and validity indices as `/filter`, with its `$feature`, `$name`,
`$layer`, `$attributeIndex`, `$validityIndex`, `$validityCount`, and `$hasValidity` overlay.
There is no new `match.` or `feature.` prefix. `geometry: true` computes the selected validity's
actual geometry, not an unconditionally copied primary shape; failed computation is an issue
and null geometry. Attribute geometry math is preflighted against a conservative tile vertex
budget. Attribute scope without a query still projects its complete assignment context.
Explicit `query: "_"` exposes the ordinary model, including geometry and fields.
`geometry: false` omits the extra computed geometry; it does not
remove coordinates explicitly selected by `query: "_"`. Use focused expressions
such as `["typeId", "_sourceData"]` for compact identity/provenance reads. JavaScript
object literals are not Simfil projection syntax. Attribute scope is also useful
for requiring a value and condition on the **same assignment**, rather than
matching unrelated descendants of one feature.

Source reference projections preserve the native `{layerId,address,qualifier?}` contract,
with `address` encoded as a decimal u64. Pass its owning map and partition explicitly:

```json
{
  "mapId":"Example", "partition":{"kind":"tile","id":131073},
  "reference":{"layerId":"Raw","address":"137438953536","qualifier":"origin"},
  "match":"containing", "query":"_"
}
```

Default `exact` works for opaque and bit-range addresses. `containing` only supports bit ranges
and returns **all** minimally enclosing ties, with `matchCount` and `ambiguous` metadata.
Ranges are absolute even beneath a presentation address scope. A zero-length requested span
is a point in a half-open range. The qualifier is provenance, not an additional address key.
Stale links yield an empty list and an issue; lookup/match-limit failure never pretends
that an incomplete candidate set is unambiguous. Lookup scans owned compound records
once, including compounds not attached to a presentation root. Scalar arrays and
shared presentation paths do not multiply this lookup work. No erdblick inspection
link format is consumed.

A SourceData address may identify a decoded subrecord rather than a database row.
Its decimal value is not a SQL column or a query. Inspect the source layer's retained
root metadata when you need the query or enclosing rows; see the datasource's own
SourceData documentation for its root fields. Reference-match items include
`rootReadArguments` retaining the same source/map/layer/partition and omitting the
address selector. Pass them to `mapget_extract_source_data` with a focused query
for those root fields; the decoded match alone does not establish SQL provenance.

<!-- mcp:
keywords: [configuration, permissions, persistence, revision]
-->
### Configuration Persistence

Enable reads with `mcp-config-read: true`. Enable writes only with all of
`mcp-config-write: true`, `mcp-direct-config-persistence: true`, and `--allow-post-config`.
A direct-file deployment may do this; a Docker wrapper that transforms host YAML must **not**
set direct persistence until it can propagate edits back to the authoritative host file.
This remains an explicit deployment gate, not a claim that an in-container edit persists.

Get-config excludes host/security settings and schemas. It uses the existing datasource-config
secret masking rules (password/secret/API-key fields). Datasource authors must not hide secrets
in arbitrary unmarked fields or URI strings; config-read is an administrative permission,
not ordinary data read access. Set-config accepts only datasource-schema top-level sections,
restores recognized masked placeholders, validates the merged document, checks the file revision,
and atomically replaces the file while preserving mode and unrelated sections. Unknown/stale
masked placeholders are rejected rather than replacing credentials. Reusing an old revision fails
with `conflict`. Acknowledgement does not wait for datasource initialization to finish.

### Execution Ownership

`McpServer` owns transport and authenticated response routing. `McpNativeTools` owns immutable
contracts, admission and one `Call` per native operation. Metadata/query preparation runs as
bounded tasks on the existing homogeneous `ServiceScheduler` pool; it must never wait for
another task on that pool. Extraction submits one partition request at a time, evaluates it
on the delivering worker, then releases the model before loading the next partition. It does
not accumulate a queue of loaded models or construct another tile cache/worker pool.

The query environment and dictionary are invocation-private. Cached tile/source-data environments,
metadata schemas and authoritative datasource pools are not mutated. `Attribute::queryContext`
is shared with `/filter`; `PartitionSourceDataLayer::findSourceData` owns address traversal.
HTTP and MCP diagnostics share the lightweight snapshot collector, and REST POST/config and
MCP share the validation/persistence operation. Query errors are returned to that invocation;
operational exceptions are sanitized rather than exposing arbitrary upstream URLs or secrets.

<!-- mcp:
keywords: [help, documentation, keywords, tool workflows, hot reload]
-->
## Documentation Search

Call `mapget_docs` with plain keywords rather than FTS syntax:

```json
{"query":"array cardinality attribute search"}
```

The result uses the usual `items`, `complete`, `reason`, `issues`, and `traces`
envelope, plus a SHA-256 `revision` of the documentation corpus. The first three
matches contain `title`, `content` (the full Markdown section), and a portable
`source` path. Up to five further matches contain just `title`. This deliberate
selection keeps each page bounded. `limit` defaults to eight; when additional
matches exist, `complete: false`, `reason: "item_limit"` and `nextOffset` identify
the next page. Repeat the same query and `component` filter with that offset.
Check `revision` for corpus changes between calls. Byte/work/time budgets can
also make a response incomplete.

Pass one returned title verbatim to read that section, using the same response
shape with one item. `query` and `title` are mutually exclusive. An empty call
lists the first eight sections in title order. Optional `component` restricts
searches, exact titles and listings to the registered component (for example
`livesource`, `classicsource`, `erdblick` or `mapget`). No match returns an empty list,
not an invented explanation. Retrieval uses SQLite FTS5/BM25, weighting titles
above annotation keywords above content; it is lexical, not semantic search.
Common conversational filler (for example “please show me”) is ignored when
substantive terms remain; an all-filler query retains its original terms. Prefer
specific domain terms such as “Classic lane validity” or “style labels”.

Help rows are charged by their serialized JSON size, with the complete envelope
and escaped text fallback checked before each row is included. A section that
cannot fit is omitted with `complete: false` and `reason: "byte_limit"`; reduce
`limit` or request a returned title to isolate the needed section.

Each invocation checks current files before querying, including changes whose
file size and modification time are unchanged. A background check also runs
approximately every two seconds when service workers are available. New, renamed,
and deleted Markdown files are recognized without a restart or CMake step.
Changing configured root paths still requires restarting. Documentation changes
do not change the tool schema, so clients need no catalog refresh or reconnect.
Tool implementations and compiled descriptions retain their normal build lifecycle.

Add deployment-specific documentation through the ordinary configuration pipeline:

```yaml
mapget:
  serve:
    mcp: local
    host: 127.0.0.1
    port: 8099
    mcp-help-docs:
      - customer-docs
      - extra-examples.md
```

Relative YAML paths resolve beside the configuration file; relative CLI paths
resolve against the working directory. A CLI list replaces the YAML extra list,
not the bundled docs. `<resolved-webapp-root>/mcp-help` is included automatically,
including when the webapp uses a mount prefix. Missing optional folders can appear
later. Register only trusted content: these documents become agent-visible context.
The tool accepts neither filesystem paths nor remote URLs from its caller.

<!-- mcp:
title: "Author MCP help snippets"
keywords: ["MCP snippets", "documentation annotations", "keywords", "hint", "authoring"]
hint: "Mark substantive canonical sections, not portal include wrappers. Keep each section independently useful and use domain vocabulary."
-->
### Authoring Help Sections

Folder registration includes `.md` files recursively, but only marked sections
become searchable. Place a YAML annotation immediately before an ATX heading:

````markdown
<!-- mcp:
keywords: [array length, cardinality, collection size]
hint: Distinguish array length from the number of non-false expression results.
-->
### Array Cardinality

Use `#items` to measure one array:

```simfil
#items > 3
```
````

`<!-- mcp: -->` alone is sufficient. Supported metadata is `title` (optional
override), `keywords` (a list of strings), and `hint` (optional prose appended
as MCP guidance). No tool names or handwritten snippet IDs are required. Generic
upstream docs should use domain vocabulary, not downstream action names.

A section extends through its subsections until the next equal/higher heading.
An independently marked subsection is also searchable; its content remains in
the parent section. Fenced examples are preserved and not parsed as annotations.
Ordinary HTML comments and portal `--8<--` directives are omitted, not expanded.
Mark the actual source section rather than a wrapper that consists only of includes.

Titles are automatically qualified by portable component/file and heading ancestry;
an explicit title replaces the heading ancestry, not the file qualification.
Duplicate qualified titles, malformed annotations, and unreadable files reject
the refresh. Errors are logged with document/line context where available. Queries
then return the last valid corpus with `complete: false`, `docs_reload_failed`,
and its unchanged revision; a later successful refresh clears the error. Before
the first successful load, the revision is `null`. Directory scans are limited
to 100,000 entries, 4,096 Markdown files, 1 MiB per file, and 16 MiB total content;
at most 4,096 sections can be indexed.
Nested directory symlinks are not followed. Overlapping roots index each physical
file once. Avoid copying the same document into multiple independent roots.

<!-- mcp:
title: "Source and packaged MCP help"
keywords: ["MCP help packaging", "docs hot reload", "source registry", "staging", "revision"]
-->
### Source And Packaged Documentation

Server builds include the folder helper from `cmake/McpHelp.cmake`:

```cmake
add_mcp_doc_folder(COMPONENT simfil DIRECTORY "${simfil_SOURCE_DIR}/docs")
```

Register each logical component once. Mapget registers its own and simfil's docs;
the MapViewer build registers its product, frontend and enabled datasource docs.
Upstream projects do not depend on mapget's CMake helper. Each registration records
the source directory in the build-local `.mcp-help-sources.json` next to the binary
module. Local servers scan those original directories directly; no intermediate
copy/build/configure step is involved when editing or adding Markdown. Adding a
new registration requires configuration once.

The `mapget-mcp-docs` target stages fresh Markdown copies under `mcp-help/<component>`
on each build, removing stale files. Installations, wheels and Docker packages
include the copied tree but **not** the development registry. Bundle lookup is
relative to the native module, including a Python extension, not the process cwd
or Python interpreter. A registered source directory replaces its component's
entire bundled directory; deleting a source snippet cannot resurrect an old copy.

`McpNativeTools` owns one `McpHelp` SQLite index. A help-local mutex serializes reads
and transactional reindexing; no alternate database, watcher thread or service-wide
lock is needed. The MCP timer schedules at most one refresh on the existing service
workers. Shutdown drains it with the other native operations. Help queries check
cancellation/work/time budgets while scanning and parsing; failed rebuilds roll back.

For agent-feedback experiments, edit the canonical annotated Markdown, invoke
`mapget_docs` again, and record the response revision with the trial. The supervisor
can change examples and guidance without restarting the tested agent or server.

## Catalog Validation Worklist

Current tests validate native schemas at construction and success envelopes at runtime, exercise
all native actions with positive/negative examples, and cover real HTTP tool calls in each
implemented transport revision. Browser catalogs retain their schema-portability checks and
cross-language fixtures. Native `items` payloads currently remain open in the common output
schema. This is **not yet full MCP/client conformance**.

- [ ] Pin official protocol schemas for each supported revision and validate every emitted tool/list, success and error envelope against them.
- [x] Meta-validate native and installed viewer input/output schemas against the supported Draft-07 dialect at startup; reject unsupported keywords and references.
- [ ] Define per-action output item schemas for stable metadata, conversion, config, diagnostics and extraction envelopes; retain open/tagged query values and test actual outputs against those definitions.
- [ ] Expand dialect/portability fixtures across every emitted combinator, closed/conditional root, recursive definition and vector schema in the production frontend catalog.
- [ ] Expand per-action fixtures to exhaustive valid/invalid/boundary inputs and validate actual implementation output, including compound, multiple, empty and limited results.
- [ ] Run combined native/viewer namespace, routing and catalog-size tests against each regenerated frontend artifact, including headless native mode.
- [ ] Pin real Codex and Claude client versions in compatibility CI and assert their model-visible inventory, not merely a successful tools/list response.
- [ ] Validate multimodal image blocks and metadata-only output projections through those clients; codec/HTTP fixtures do not prove model-visible images.
- [ ] Invoke every safe read and controlled mutation through each supported client/revision; assert cancellation, transport loss, byte limits, error shapes and no mutation replay.
- [ ] Run the upstream MCP conformance suite and publish a supported-client/revision matrix with the exact tests and deliberate limitations.
- [ ] Keep hosted PKCE, token refresh, role/audience isolation, JWKS rotation and cross-user session tests as a separate deployment security gate.

<!-- mcp:
keywords: [WebMCP, browser, document, authentication, tools]
-->
## Browser-native WebMCP

Erdblick exposes its current-tab tools through the browser's WebMCP API. To add
native mapget tools to that document, `POST /mcp/browser` accepts the same bounded
JSON-RPC tools/list and tools/call protocol as `/mcp`, including finite SSE results,
with a different authentication boundary:

- Every request requires an explicitly allowed `Origin` and `Host`.
- Local mode requires a loopback peer and rejects forwarding headers as usual.
- OAuth deployments use the trusted proxy's browser identity headers, exactly as
  the interactive WebSocket does. The proxy must strip caller-supplied authority
  headers and inject verified issuer, subject, expiry, and permissions for this
  route too. A bearer token or browser-supplied JSON identity cannot substitute.
- The configured browser permissions header can independently contain
  `viewer-read`, `viewer-control`, `config-read`, `config-write`, and `diagnostics`.
  Viewer control does not grant config/diagnostic authority. Existing server-side
  configuration opt-ins and write gates still apply.
- Browser authority does not synthesize bearer claim mappings for datasource
  headers. Sources requiring those mappings remain inaccessible through this
  endpoint unless a separate supported deployment authentication path grants access.

Discovery includes only authorized native mapget tools. Calls cannot reach
`viewer_list_sessions` or any relayed viewer action, even with an explicit
`clientId`: erdblick executes its own tools inside the current document. `/mcp`
continues to require its ordinary bearer authentication in OAuth mode; trusted
browser headers do not authorize that endpoint.

Serve erdblick and this route through the same origin and base path, with normal
session authentication. No browser access token storage or CORS credential sharing
is required. Exclude neither this route nor `/interactive` from trusted proxy
identity injection when enabling WebMCP. Permission expiry is checked for each
request and retained work keeps the same bounded lifetime as native MCP calls.
