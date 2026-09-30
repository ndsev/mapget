# MCP Viewer Actions

Mapget can expose a trusted browser-action catalog through `POST /mcp` and
route calls to a user's connected viewer tabs. The feature is **disabled unless
explicitly configured**. It does not create another datasource, tile worker
pool, or browser session protocol.

The initial tool set is deliberately small:

| Tool | Purpose | Permission |
| --- | --- | --- |
| `viewer_list_sessions` | List the caller's connected, compatible tabs | `viewer-read` |
| `viewer_describe_app_state` | Discover supported application-state channels | `viewer-read` |
| `viewer_get_app_state` | Read bounded state summaries | `viewer-read` |
| `viewer_set_app_state` | Assign supported application state; initially a camera | `viewer-control` |

The three browser tools are described by the installed catalog, not by C++
copies of frontend schemas. Datasource/configuration/schema/extraction tools
are not part of this first implementation.

## Local Setup

Export `viewer-actions.json` from the matching viewer build and install it
beside a separate JSON trust configuration, for example `mcp.json`:

```json
{
  "authentication": "local",
  "endpoint": "http://127.0.0.1:8099/mcp",
  "catalogPath": "viewer-actions.json",
  "allowedHosts": ["127.0.0.1:8099", "localhost:8099"],
  "allowedOrigins": ["http://127.0.0.1:8099", "http://localhost:8099"]
}
```

Start the native server, optionally also serving a viewer build with `-w`:

```sh
mapget serve --host 127.0.0.1 -p 8099 --mcp-config ./mcp.json -w /path/to/viewer
```

Embedding applications set `HttpServiceConfig::mcpConfigPath`. Config and
catalog paths are local files; relative artifact paths resolve beside the
MCP config. Configuration is restart-scoped, and unknown settings fail startup
rather than silently disabling a security control.

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

```json
{
  "authentication": "oauth",
  "endpoint": "https://viewer.example/mcp",
  "catalogPath": "viewer-actions.json",
  "allowedHosts": ["viewer.example", "supplier.example"],
  "allowedOrigins": ["https://viewer.example", "https://supplier.example"],
  "oauth": {
    "issuer": "https://identity.example/realm",
    "audience": "https://viewer.example/mcp",
    "jwksUrl": "https://identity.example/realm/keys",
    "requiredScopes": ["viewer"],
    "clientId": "public-viewer-client",
    "clockSkewSeconds": 15,
    "permissions": {
      "viewer-read": {"claim": "/viewer_permissions", "value": "read"},
      "viewer-control": {"claim": "/viewer_permissions", "value": "control"}
    },
    "browser": {
      "trustedProxyAddresses": ["127.0.0.1"],
      "issuerHeader": "x-viewer-issuer",
      "subjectHeader": "x-viewer-subject",
      "expiryHeader": "x-viewer-expiry",
      "permissionsHeader": "x-viewer-permissions",
      "maxLifetimeSeconds": 3600
    }
  }
}
```

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

The administrator supplies exactly one `jwksUrl` or `jwksFile`. The URL must
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
the earlier of the verified expiry and `maxLifetimeSeconds` after connection.
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

Optional `limits` in `mcp.json` override these positive integer defaults:

| Setting | Default | Meaning |
| --- | ---: | --- |
| `timeoutMs` | 30000 | Call deadline, additionally capped by caller/browser authority |
| `invocationBytes` | 65536 | Complete invoke or registration envelope |
| `resultBytes` | 262144 | Complete browser result envelope |
| `callsPerSession` | 4 | Outstanding calls per viewer tab |
| `callsPerPrincipal` | 16 | Outstanding calls across one user's tabs |
| `pendingCalls` | 128 | Process-wide calls, including uncertain mutations |
| `sessions` | 256 | Attached action peers, including unregistered tabs |

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
and real HTTP/WebSocket connections using only Python's standard library:

```sh
build/bin/test.mapget '[mcp-actions]'
ctest --test-dir build -R '^test-native-mcp$' --output-on-failure
```

The transport test is not a real frontend, identity-provider login, or hosted
deployment test. Consumer acceptance additionally requires the matching viewer
catalog/build, real browser actions, and the deployment's OAuth client flow.
