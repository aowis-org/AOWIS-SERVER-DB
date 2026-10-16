# V41 — Explicit top-level field conflict resolutions

V41 extends the existing `Database::mergeObjectRevisions()` API without
changing its signature, SQLite schema, or finalized revision hashes. It permits
independent choices for **each conflicting field** of a canonical,
round-trippable hydraulic entity, while keeping existing whole-entity `Left` /
`Right` choices compatible.

`previewObjectMerge()` already reports the exact `conflicting_fields` names for
18 typed hydraulic entity kinds. Pass one `ObjectMergeResolution` per conflict:

```cpp
using namespace aowis::db;

ObjectMergeResolution junction_resolution;
junction_resolution.object_uuid = junction_uuid;
junction_resolution.field_resolutions = {
    {QStringLiteral("elevation_m"), ObjectMergeSide::Left},
    {QStringLiteral("metadata.description"), ObjectMergeSide::Right}
};

// Pass junction_resolution as one item in mergeObjectRevisions(..., resolutions, ...).
```

If `field_resolutions` is not empty, its entries must match the preview's
`conflicting_fields` **exactly once each**. The `ObjectMergeResolution::side`
member is ignored in this mode. Every nonconflicting field is synthesized from
the three-way comparison as before. Choices only decide fields changed
incompatibly by both sides. Fields inherited from only one branch are never
silently lost merely because another field was resolved toward the other branch.

A resolution with an empty `field_resolutions` vector retains the V38–V40
whole-entity behavior: `side` selects the complete left or right state. This is
still required for unsupported, opaque, create/create, delete/modify and
kind-change conflicts, whose preview `conflicting_fields` is empty. Do not
submit resolutions for automatic `FieldsMerged` changes.

## Safety rules

- Missing, extra, duplicate, empty, or unknown field names fail **before**
  opening the SQLite write transaction. Unsupported field-choice conflicts
  also fail closed. Every object conflict still needs exactly one resolution.
- Canonical codec and UUID round-trip checks remain mandatory. Field synthesis
  uses the same 4 MiB per-source limit as V40 and does not splice binary bytes.
- Since V42, `EntityMetadata` is merged through seven independent subfields,
  addressed as `metadata.enabled`, `metadata.model_role`, `metadata.date_added`,
  `metadata.date_installed`, `metadata.description`, `metadata.comment`, and
  `metadata.tags`. The tags list is atomic. All other nested structs and
  collections remain atomic fields.
- Existing source branch access, write/finalize permissions, generation and
  physical-state checks remain intact. All edits, prior local journal groups,
  two-parent finalization and SHA-256 hashes remain in the existing single
  SQLite transaction. Transaction failure fully rolls back the merge.
- The database only performs structural and reference consistency checks.
  EPANET is not run and hydraulic feasibility is not established.

`aowis-db-object-merge-field-resolutions` tests mixed left/right choices within
one junction, preservation of independent changes, invalid choices, unsupported
payloads, local journal retention, late SQL rollback and persistence checks.
