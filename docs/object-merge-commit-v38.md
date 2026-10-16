# V38 — Atomic object merge commits (original restricted version)

`Database::mergeObjectRevisions(actor, target_branch_uuid,
expected_working_revision_uuid, expected_generation,
right_finalized_revision_uuid, resolutions, title, message)` combines
**two finalized, diverged, same-project revisions** in one atomic SQLite
transaction, reusing V36 ancestry and V37 entity-level preview.

## Required state

- The target branch's head is an **unedited** (`generation == 0`) canonical
  object-mode working revision copied from its **sole** finalized parent.
  That parent becomes the **left** merge input. Pass its actual working UUID as
  well as its generation to reject stale updates. The target is rejected if
  it has journal edits, mismatching canonical object contents or altered
  tombstone versions, or unexpected parent links. The target's object rows are
  compared against the finalized parent under SQLite's write lock.
- The **right** input must be a finalized revision on a different branch in
  the same project. Both inputs must have diverged, and the merge preview must
  have exactly one finalized merge base; there is no fast-forward handling or
  recursive criss-cross merge-base synthesis in this milestone.
- The actor needs `project.read` on left, right, and the merge-base branch,
  and both `project.model.write` and `project.revision.finalize` on the target.
  Read/write permissions are checked again while holding the write lock.
- Every `Conflict` from `previewObjectMerge` requires one
  `ObjectMergeResolution {object_uuid, ObjectMergeSide::Left/Right}`. Missing,
  duplicate, unknown, and nonconflict resolutions are rejected. Unchanged,
  left-only and identical-both changes need no resolution. Right-only changes
  are applied automatically. Choosing left preserves the left object state;
  choosing right applies the right state, including tombstone/deletion.
- Merging a different `kind` into a UUID already in use (even as a tombstone)
  is rejected. Object identity and type remain immutable: this API does not
  bypass the existing guard with delete/recreate tricks.

## Transaction and integrity

The implementation uses `BEGIN IMMEDIATE` to serialize writers, verifies the
head UUID/generation/parent and exact copied entity states, applies only actual
changes with the existing per-object version checks, and appends an immutable
`object_events` journal. Applied object edits form one `object_groups` action.
Affected hydraulic networks are validated as a whole in the resulting state,
with their membership and reverse-reference indices updated transactionally.

The right source becomes the **second ordered parent** on the target working
revision. The target is then finalized using the existing SHA-256 snapshot and
ordered-parent hash chain. A new editable working revision is created on the
target branch, copying merged objects and indexes. All operations either commit
together or roll back, including object edits, journal entries, group history,
parent edges, finalization, and head update.

The API returns the existing `Finalization` structure on success. Errors never
write a partial merge. Source revisions are immutable and unchanged. No SQLite
schema migration or change to historical revision hashes is required.

## Limitations

- Entity-granular left/right choices only; no field-wise conflict resolution.
- No automatic rename, UUID remap, recursive merge base, legacy snapshots, or
  merge of an already edited working head.
- This is a C++ library operation; the REST protocol has no merge endpoint yet.
- Model semantic conflict detection uses existing hydraulic validation. It
  does not attempt to decide whether distinct **valid** hydraulic designs are
  functionally equivalent.

**V39 update:** The pristine-head limitation below describes the original V38
implementation. Edited working heads are supported as of V39; see
[merge with local edits](object-merge-local-edits-v39.md).
