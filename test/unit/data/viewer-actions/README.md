# Viewer Action Contract Fixtures

Snapshot of the coordinated erdblick working-tree contract (27 actions and
20 state channels) on 2026-10-08, not a published erdblick release:

`sha256:035d0a4f3dca7a65863b82230c49e6a32c32964d8792728c8afd6ed5f2562981`

- `web-mcp-actions.json`: erdblick `app/actions/generated/web-mcp-actions.json`.
- `viewer-action-relay.schema.json`: erdblick
  `app/actions/generated/viewer-action-relay.schema.json`.
- `fixtures.json`: erdblick `test/viewer-actions/fixtures.json`.

Generate the two artifacts with erdblick's `npm run generate:viewer-actions`.
The sources are `app/actions/viewer-action.contract.ts`,
`app/actions/viewer-operation.contract.ts`, `app/actions/viewer-ui.contract.ts`,
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

Setter input/output fixtures include conditional channel/value matching. Source
links retain native signed tile IDs and lossless decimal object/address IDs;
view-dependent mutation fixtures require the current layout revision.

Screenshot fixtures describe the browser `{image, metadata}` result. Native MCP
publishes only the metadata schema/structured content, with JPEG bytes in an image
content block. The small codec fixture is not a pixel-rendering acceptance image.

UI fixtures cover bounded DOM reads, snapshot UID targets, pixel or homogeneous
split-percentage resize inputs, and rejection of arbitrary HTML/script/CSS access.
Layout ownership and live-element validation are browser-side behavior; native
code validates the same generated contract and existing read/control permissions.
