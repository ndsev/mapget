# Viewer Action Contract Fixtures

Snapshot of the coordinated erdblick working-tree contract (27 actions and
20 state channels) on 2026-10-08, not a published erdblick release:

`sha256:130434ea0efff9d05394cac2a25126e11bed16b8f9ffd299440a072679260d3e`

For a coordinated refresh from the current erdblick source layout, copy:

- `web-mcp-actions.json`: erdblick `app/action/generated/web-mcp-actions.json`.
- `viewer-action-relay.schema.json`: erdblick
  `app/action/generated/viewer-action-relay.schema.json`.
- `fixtures.json`: erdblick `tests/fixtures/viewer-actions/fixtures.json`.

Generate the two artifacts with erdblick's `npm run generate:viewer-actions`.
Domain controllers declare actions and controls through `registerActions` and
`registerControls`; `app/application-registration.ts` collects them. The generator
uses that same registration through `tools/export-viewer-actions.ts`, rather than
a separate centralized action-schema file. Relay messages remain defined in
`app/action/viewer-action-relay.contract.ts`.
Update the snapshot date, catalog ID and counts above when refreshing; the
historical snapshot is not a claim about the current frontend inventory.

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
