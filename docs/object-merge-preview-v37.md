# V37 — Read-only three-way object merge preview

`Database::previewObjectMerge(actor, left_revision_uuid, right_revision_uuid)`
previews the canonical **entity-level** changes required to merge two revisions.
The result is a `std::optional<ObjectMergePreview>`; failure returns `nullopt`
and sets the optional error string. This is a planning/read API, not a merge
or a write operation.

## Preconditions and permissions

- Both revisions must belong to the **same Project**, as enforced by V36
  revision ancestry checks.
- The revision DAG must have **exactly one maximal finalized common ancestor**.
  Multiple merge bases (criss-cross DAG), no finalized merge base, and
  missing/corrupt ancestor links fail closed. V37 does not invent a recursive
  merge base or choose the first one.
- The base and **both** source revisions must be in canonical object mode.
  Legacy opaque snapshot revisions are rejected, including mixed-mode merges.
- The caller must hold `project.read` on the left branch, the right branch,
  **and the merge base's branch**. This extra check matters when the base is
  owned by a separate private branch. No write permissions are needed.
- Working tips are valid inputs. All reads use one SQLite read transaction so
  the ancestry graph and object states are derived from a consistent snapshot
  when another connection concurrently updates a working tip.

## Interpretation

Each `ObjectMergeChange` holds the object UUID and the live `base`, `left`, and
`right` `ModelObject` states (`std::nullopt` means absent/deleted). It is
classified as:

| Kind | Meaning | Automatic resolution *in principle* |
| --- | --- | --- |
| `LeftOnly` | Only left differs from the base | Left's state |
| `RightOnly` | Only right differs from the base | Right's state |
| `BothSame` | Both differ, but are exactly equal to each other | Either state |
| `Conflict` | Both differ from the base, and also differ from each other | **None** |

State equality means **canonical object kind and byte-exact content** are
equal. Per-revision object version numbers are intentionally ignored; they
represent local optimistic-concurrency history and may differ across branches
without a model-content conflict. Two tombstones/absent objects are equal.
Unchanged entities are omitted; changed entries are deterministically sorted
by UUID bytes. The result also captures both working/finalized revision
generation counters for later optimistic-concurrency checks. `conflict_count`
reports how many entries require explicit
conflict resolution. Payloads remain available to callers for inspection.

This is intentionally **object-granular**, not field-granular. Concurrent
edits to different fields of the *same* entity are still conflicts. Concurrent
creation of the same UUID with different type or content is a conflict;
concurrent delete versus modification is a conflict. Independent entities
are reported as separate, non-conflicting changes.

## Explicit non-goals

V37 performs **no** persistence, branch-head updates, parent rewriting, audit
mutations, or model validation of a prospective merged graph. A future merge
operation must resolve conflicts, validate cross-entity hydraulic references
and connectivity, enforce generation/version compare-and-swap, record durable
object history, and finalize a revision with the correct ordered parent hashes.
A conflict-free object preview does **not** certify that the combined hydraulic
network is valid. Existing V2 revision chains and schema V13 remain unchanged.
