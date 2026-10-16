# V42 — Independent EntityMetadata subfield merging

The V40/V41 field-aware merge can now distinguish the seven codec-defined
`EntityMetadata` subfields in **all 18 typed hydraulic entity kinds** that use
metadata. For example, one branch may edit `metadata.description` while another
edits `metadata.comment`; both changes survive without a conflict. No new
SQLite schema, revision format, or public C++ entry point is introduced.

## Preview and resolutions

`previewObjectMerge()` reports a `FieldsMerged` object if there are no
conflicting subfields, or lists overlapping fields by dotted names:

- `metadata.enabled`
- `metadata.model_role`
- `metadata.date_added`
- `metadata.date_installed`
- `metadata.description`
- `metadata.comment`
- `metadata.tags`

`metadata.tags` remains an **atomic ordered list**: concurrent differing tag
list edits conflict. All other nested structs and collections, including
coordinates, demands, curves and vertices, retain their conservative atomic
handling. Use the existing `ObjectMergeFieldResolution` with the dotted field
name, e.g. `{QStringLiteral("metadata.description"), ObjectMergeSide::Left}`.
The old name `metadata` no longer identifies an individual conflict choice;
choose `Left` or `Right` for the **entire entity** to preserve the original
fallback behavior.

Canonical Model decoding, byte-exact field comparison and full canonical
round-trip checks remain mandatory. Missing/extra/duplicate field choices fail
closed. Validation is structural/reference-based, **not an EPANET simulation**.
The atomic merge transaction still protects object edits, undo/redo history,
parent links, audit and immutable finalized revision hashes.

The configure-time field-schema checker also verifies the exact seven metadata
members against the pinned 68-structure Model codec manifest, preventing silent
drift. `aowis-db-object-merge-metadata` covers automatic and manually resolved
subfield merges, tags, optional dates, local working edits, rejected choices,
transaction rollback and persistence after reopening SQLite.
