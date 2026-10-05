# Viewer Action Contract Fixtures

Snapshot of the coordinated erdblick working-tree contract with homogeneous
camera-vector schemas on 2026-10-05, not a published erdblick release:

`sha256:d7fe97894636e8b976786a25f01629daaac74d6427aaf29b4d8fa0b2b3f0dbbf`

- `web-mcp-actions.json`: erdblick `app/actions/generated/web-mcp-actions.json`.
- `viewer-action-relay.schema.json`: erdblick
  `app/actions/generated/viewer-action-relay.schema.json`.
- `fixtures.json`: erdblick `test/viewer-actions/fixtures.json`.

Generate the two artifacts with erdblick's `npm run generate:viewer-actions`.
The sources are `app/actions/viewer-action.contract.ts`,
`app/actions/viewer-action-relay.contract.ts`, and
`app/shared/app-state-channel.contract.ts`.

Refresh these snapshots together after a coordinated contract change. Keeping
them here lets standalone mapget CI test the same acceptance/rejection cases
without building Angular or depending on a sibling checkout. Runtime action
metadata must come from the deployment's trusted artifact, never these fixtures
or a browser-supplied catalog. The catalog ID is opaque to native code; erdblick
owns canonicalization and hashing.

The validation cases include astral-Unicode string-length boundaries. Runtime
validation and truncation use Unicode code points, as Draft-07 `maxLength` does,
not JavaScript UTF-16 code units. Camera offsets remain optional, but when present
must contain exactly three numbers in both arguments and results. Their schema
uses homogeneous `items` with `minItems` and `maxItems`, avoiding tuple-style
schemas that some MCP clients discover but cannot expose as model-callable tools.
