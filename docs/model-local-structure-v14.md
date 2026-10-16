# V14 — Dependency-local pipe and demand-point structural validation

This is the first *bounded* structural fast path; all other structural changes
retain the complete hydraulic-network validation from V8/V13.

Within an existing write transaction, edits to already-indexed, existing
`hydraulic.pipe` and `hydraulic.demand_point` rows may use local validation.
The old/new payloads must decode to the same UUID and type. Pipe material UUIDs
and demand-point demand-pattern references must stay unchanged. For such edits:

- Pipe endpoints must resolve to a node (junction, reservoir, or tank) within
  the same network, or be null for an incomplete draft.
- Pipe attachments must resolve to a pipe; junction attachments and assigned
  endpoints must resolve to a junction. Pipe positions must be finite in [0,1].
- An assigned junction must still be an endpoint of its attached pipe.
- When a pipe endpoint changes, the V7 reverse-reference index locates its
  previously attached demand points. Their **current final transactional**
  attachments must still satisfy the assigned-endpoint rule, including when
  some demand points were changed in the same object group.
- On success, only the touched sources' derived reference-index rows are
  delta-updated. No full-network reconstruction or revision-wide scan occurs.

This does **not** bypass optimistic concurrency, the append-only operation
journal, transaction rollback, full-network validation for all unsupported
structural changes, or finalized revision hashing.

Network root changes, insertions/deletions, changed pipe material or demand
pattern references, control-rule changes, malformed records, unindexed sources,
and groups over the existing small-batch limit still take the full validator.
No SQL migration or hash-format change is required.

The `aowis-db-model-local-structure` regression covers invalid standalone
retargeting, coordinated endpoint/attachment groups, undo/redo and their index
deltas, wrong-type and dangling targets, rollback, a second independent pipe,
and finalization integrity. The existing V13 tests remain in the suite.
