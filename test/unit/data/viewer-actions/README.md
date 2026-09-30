# Viewer Action Contract Fixtures

Snapshot of the coordinated, **uncommitted** erdblick working-tree contract on
2026-09-30, not a published erdblick release:

`sha256:bae94ac327fc097ee2ec6c7a433fdbc62c719be857f20d0f146373315d597299`

- `viewer-actions.json`: erdblick `app/actions/generated/viewer-actions.json`.
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

The 51 validation cases include astral-Unicode string-length boundaries. Runtime
validation and truncation use Unicode code points, as Draft-07 `maxLength` does,
not JavaScript UTF-16 code units. This runtime correction and fixture expansion did
not change the catalog or relay/info schema bytes or their identity.
