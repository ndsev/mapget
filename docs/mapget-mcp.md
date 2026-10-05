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

All MCP settings are **restart-scoped**. They live in the private `mapget`
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
names `viewer-read` and/or `viewer-control`; the expiry is Unix **seconds**.
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
replay, application pagination, prompts, or resources are advertised.
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
| `mapget_list_sources` | Optional `mapId`, `layerId`, `sourceId` | Ordered source IDs, lifecycle/progress, compact layer coverage, partition kind, feature types and ID compositions; no serialized feature-model schemas |
| `mapget_query_schema` | `mapId`, `layerId`; optional `sourceId`, `featureType`, `query`, `trace` | Feature-type roots and sequences of schema descriptors |
| `mapget_validate_expression` | `mapId`, `layerId`, `expression`; optional `sourceId`, `featureType`, `attributeSchema`, `scope`, `rewrite`, `predicate` | Compilation and schema-access assessment per context, normalized expression, diagnostics; no tile I/O |
| `mapget_extract_features` | `mapId`, `layerId`; `partitions` or canonical primary `featureIds`; optional `sourceId`, `featureTypes`, `scope`, `predicate`, `rewrite`, `query` or `expressions`, `geometry`, `trace` | Feature/attribute rows with provenance and one value sequence per expression |
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

### Results And Budgets

Success is an object in `structuredContent`, with the same JSON in a text content block:

```json
{"items": [], "complete": true, "reason": null, "issues": [], "traces": {}}
```

Failures use `isError: true` and a structured error. A partial enumeration is a successful
bounded response with `complete: false` and a reason such as `item_limit`,
`expression_result_limit`, `work_limit`, `depth_limit`, `byte_limit`, `query_error`, or
`load_failed_or_cancelled`. Narrow the request; there is no cursor, continuation state,
or automatic pagination. No matches is a valid empty result, not an error.

Common inputs are `limit` (default 100, maximum 1000), `maxWork` (100000, maximum 1000000),
and `maxDepth` (16, maximum 64). `limit` caps rows **and** values per expression separately.
Partition inputs are capped at 32, feature IDs at 100, expression lists at 16 and expression
text at 4096 characters. The configured MCP deadline, per-principal/overall admission caps,
and result-byte cap also apply. JSON conversion shares the work/depth/byte budget with
simfil evaluation. Native result construction uses at most one third of the wire allowance
(capped at 1 MiB), reserving space for text fallback escaping and envelope duplication;
actual serialized size is checked before sending.

These are cooperative guards, **not a hard sandbox**: datasource I/O, compilation, regex
engines, custom functions and individual model accessors can contain non-preemptible work.
A client timeout does not imply rollback of a config mutation. Do not retry uncertain writes
without rereading the revision. No result pagination or response-task continuation is retained.

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

### Schema And Expression Semantics

`mapget_query_schema` uses simfil's lazy `SchemaModel` on the same `LayerSchema` graph used
by completion. Descriptors contain `kind`, `fields`, `elements`, `alternatives`, enum values,
open/nullable/required flags and terminal recursion/budget markers as appropriate.
Array elements are possible domains, not fabricated sample records; union alternatives are
distinct from arrays. Descriptor queries default to `_`; for example:

```json
{"mapId":"Example","layerId":"Road","featureType":"Road","query":"fields"}
```

Expression validation instead binds **data** schemas to the compiler. It never evaluates
against fake features or fetches map data. `scope` is `feature`, `attribute`, or `auto`;
`rewrite: true` requests the existing search normalization. Auto scope also uses normalization
when metadata is available. An explicit `attributeSchema` must identify a real attribute context.
Without it, available attribute contexts are compiled separately. `schemaCertain` reports
statically resolved accesses against a closed root, not a guarantee that runtime data/functions
will succeed. `runtimeValidated` is always false. Missing/open/dynamic metadata stays uncertain.

### Extraction And Native Source Links

Tile partitions are `{"kind":"tile","id":131073}`; object IDs are lossless decimal strings,
for example `{"kind":"object","id":"18446744073709551615"}`. Feature IDs are canonical
mapget strings. With only primary IDs, the datasource's cheap locate hook supplies candidate
partitions, then ordinary service/cache loading and exact ID lookup resolve them. This does
not perform secondary-ID mapping, object discovery, a coverage-wide scan, or direct datasource
conversion. Supply explicit partitions when the datasource cannot cheaply locate a feature.
Omitted/empty `featureTypes` searches all types; omitted `predicate` matches all candidates.

```json
{
  "mapId":"Example", "layerId":"Road",
  "partitions":[{"kind":"tile","id":131073}],
  "predicate":"typeId == 'Road'",
  "expressions":["id", "properties", "properties.layer.rules.speed.limit"],
  "geometry":true
}
```

Feature rows include source/map/layer/partition/feature ID provenance. Attribute scope iterates
the same feature-local attribute and validity indices as `/filter`, with its `$feature`, `$name`,
`$layer`, `$attributeIndex`, `$validityIndex`, `$validityCount`, and `$hasValidity` overlay.
There is no new `match.` or `feature.` prefix. `geometry: true` computes the selected validity's
actual geometry, not an unconditionally copied primary shape; failed computation is an issue
and null geometry. Attribute geometry math is preflighted against a conservative tile vertex
budget. Default `_` projections still expose the ordinary model, including its geometry/fields.

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
Stale links yield an empty list and an issue; traversal/match-limit failure never pretends that
an incomplete candidate set is unambiguous. No erdblick inspection link format is consumed.

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
