# V39 — Merge into an edited working revision

`Database::mergeObjectRevisions(...)` now accepts a **canonical object-mode
working head with existing local edits** (`expected_generation >= 0`). It
retains the V38 API shape and all existing restrictions on the remote source,
permissions, ancestry and conflict resolution.

## Two distinct left-side identities

The target working revision's **current entity content** is the left side of
the three-way merge. Its sole **finalized parent** remains the first parent of
the resulting two-parent finalized revision. These are deliberately not
interchangeable:

- To display conflicts, call
  `previewObjectMerge(actor, working_head_uuid, remote_finalized_uuid)`.
  Use the working UUID, **not** the previous finalized revision's UUID.
- Pass that same `working_head_uuid` and the preview's `left_generation` as
  `expected_working_revision_uuid` and `expected_generation` when committing.
- The right input is still a finalized revision on a different branch of the
  same project; the two finalized source lineages must have diverged and share
  exactly one finalized merge base.
- Local-only changes are preserved. Remote-only changes are applied.
  Conflicting local/remote changes require explicit `Left` or `Right` choices.
  Choosing `Left` preserves the *current local* bytes or deletion, not the
  content from the earlier finalized parent. Identical changes on both sides
  are applied only once.

## Journals, atomicity and concurrency

Existing local `object_events` and `object_groups`, including undo/redo
actions, remain part of the working revision's history. The merge writes only
actual additional entity changes and, when necessary, creates one new edit
group. A zero-edit merge still produces a two-parent finalized revision. The
finalized snapshot, operation-history digest and ordered parent hash chain
cover both preexisting local activity and the merge.

The working UUID and revision generation are checked both before and under
the `BEGIN IMMEDIATE` write lock. The merge preview also records the SHA-256
of the working head's **physical object rows** (including tombstones and
version counters) in its read transaction. That state is checked under the
write lock to reject unjournaled changes between internal preview and commit.
An untouched generation-zero target retains V38's additional byte-for-byte
copy check against its parent. Any error causes the *entire merge transaction*
to roll back while preserving previously committed local edits and groups.

Only network **structural and reference-consistency validation** is performed.
No EPANET hydraulic simulation is performed by the database.

## Restrictions

- A working head must have exactly one finalized parent before the merge.
- No fast-forward, recursive multi-base merging, field-wise conflict
  resolution, implicit UUID/type remapping or legacy opaque snapshot merging.
- Source and base branch read rights, target model-write and finalize rights
  are enforced as in V38.
- The REST API does not yet expose merge endpoints.

No SQLite schema migration is required. Existing finalized revision hashes
are unchanged.
