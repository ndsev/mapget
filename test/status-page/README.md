# Status Page Tests

These browser tests exercise mapget's actual embedded status-page HTML with
controlled schema responses. They cover navigation, lazy requests, filtered
discovery, readable metadata, authorization-driven removal, failed requests,
stale completions and narrow-screen layout without an erdblick build.

```sh
cd test/status-page
npm ci
npx playwright install chromium
npm test
```

The isolated fixture server binds loopback port 18982 and refuses to reuse an
existing process. It is not a substitute for native endpoint tests:
`test/unit/test-http-schema.cpp` and the schema finder tests validate the real
authorization, metadata and shared MCP/REST search contracts. Browser smoke
testing against a built server with a real large schema remains useful too.
