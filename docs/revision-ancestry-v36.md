# V36 — Read-only revision DAG comparison

`Database::compareRevisions(actor, left_uuid, right_uuid)` compares two
revisions in the **same project**. Both working and finalized revisions may
be endpoints; parent traversal requires all ancestors to be finalized and
in that same project. The caller must have `project.read` for **both**
source branches; there is no access-control bypass via an ancestor UUID.

The result contains:

- `left_is_ancestor_of_right` and `right_is_ancestor_of_left` (inclusive:
  comparing a revision with itself sets both).
- `merge_base_revision_uuids`: **all maximal finalized common ancestors**.
  A working revision is never a merge base, even when compared to itself.
  Results are deterministically ordered by the 16 UUID bytes.

A simple fork has one merge base. Criss-cross DAGs can have several; picking
one arbitrarily would be wrong for a future three-way merge. Traversal
uses a visited set to avoid reloading shared ancestors, and common-ancestor
pruning performs one graph traversal instead of an exhaustive pairwise
comparison. Missing, non-finalized, or cross-project ancestors fail closed.

This is deliberately **read-only**. It does **not** merge hydraulic model
contents, write revision parents, resolve concurrent edits, or choose a
conflict resolution strategy. Merges need a separately designed API that
validates models, authorization, snapshot hashes, and working generations.

No SQLite schema migration, changes to hash encodings, or rewrite of prior
revision history are needed. The regression fixture exercises linear
history, forks, private sibling branches, working tips, two maximal common
ancestors, persistence, and hash verification.
