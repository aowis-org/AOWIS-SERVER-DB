# AOWIS-SERVER-DB

AOWIS multi-project, multi-branch revision database. The repository is being rebuilt
in stages, beginning with an embedded SQLite core. An optional REST server and a
PostgreSQL adapter are planned, but **not implemented yet**.

## Identity and history

- **Project**: independently managed AOWIS project; permanent UUIDv7.
- **Branch**: named development line belonging to one project; permanent UUIDv7.
- **Revision**: an individual state on a branch; permanent UUIDv7, with a
  branch-local sequence number.
- Creating a project creates its `main` branch and revision 1 (working).
- Branching is currently allowed only from a finalized revision of the same
  project. The new branch starts at working revision 1 using that snapshot.
- Each branch has exactly one current working revision. Finalization creates
  the next working revision in the same atomic SQLite transaction.
- Finalized revisions and their snapshot bytes cannot be edited through the
  library or ordinary SQL updates. SQL triggers enforce this invariant.
- SHA-256 hashes bind immutable revision metadata, a complete opaque snapshot,
  and the parent revision's hash. Branches can therefore have different
  histories with a common finalized ancestor.
- **Merges are not implemented.** The schema and V1 hash encoding support
  multiple ordered parents; the merge operation and model-level merge conflict
  resolution will be built in a later stage.
- Editing concurrent working snapshots requires an expected edit generation.
  Stale writes are rejected rather than silently overwriting another client.

The schema-v2 security foundation adds UUIDv7 users, groups, custom roles,
permissions, memberships, grants and scoped role assignments; fixed UUIDs for
built-in role/permission definitions; and explicit actor-aware reads/writes.
New finalizations use an authorship-aware SHA-256 V2 chain while v1 finalized
hashes are preserved. There is no authentication or login system yet.

See [docs/architecture.md](docs/architecture.md) and
[docs/identity-and-access.md](docs/identity-and-access.md) for the contracts.

## Build and test

Requirements: CMake 3.21+, a C++20 compiler, Qt 6 Core/Sql, Python 3 for schema compatibility checks, the Qt SQLite SQL
plugin, and the initialized `external/AOWIS-SERVER-MODEL` Git submodule. The Model's existing `createUuidV7()` function is used for IDs.

```bash
./git_submodule_init.sh
./compile_linux.sh
./compile_linux_tests.sh
# Optional: ./compile_linux_tests.sh --both
```

From the DB repository root, `./git_submodule_update.sh` explicitly moves
Model to the latest tracked remote revision; review and commit that new gitlink.
Do not use the update script for normal builds: `git_submodule_init.sh` checks
out the **pinned** version. The scripts are independent of the shell's working
directory. The compile script builds the library (there is no DB GUI executable).

Alternatively set `-DAOWIS_MODEL_SOURCE_DIR=/path/to/AOWIS-SERVER-MODEL`.
The library target is `aowis-db-core` (`AOWIS::DB` alias). All Qt SQL connections
are thread-affine: create one `Database` instance per thread, not one shared
connection across threads.

## Current scope and limitations

For local initialization, the trusted application must call
`bootstrapAdministrator(username)` once, then pass an `OperationContext` with
that actor's UUID to all project APIs. Remote callers must be authenticated by
the future server (not implemented yet). Database files in schema V1/V2/V3 are
upgraded transactionally to V4 without changing existing finalized hashes.

The core persists project/branch/revision history, legacy whole snapshots,
and UUID-keyed object records. **V5 now encodes and reconstructs the entire
`NetworkHydraulic` structure** with nested metadata, simulation options,
water-quality/MSX configuration, curves, controls, pipe materials, meters,
demand points and the 18 entity collections, preserving order and UUIDs.
`saveHydraulicNetwork` stores each top-level entity separately and records
per-entity changes in one SQLite transaction. Individual entity updates use
`putHydraulicEntity` with an expected object version, and can be read through
`loadHydraulicEntity`. The previous `replaceWorkingSnapshot` interface remains
only for legacy-mode revisions.

This is typed hydraulic-network persistence, **not yet production-ready full
project persistence**: project descriptions, external attachment file contents,
other non-network model domains, domain-reference validation, grouped object
undo/redo, and model-level merge semantics are still outstanding.

No HTTP API, user authentication, password/session handling, model validation,
model-aware validation, grouped object changes, object undo/redo, merge
handling, or PostgreSQL backend is implemented yet. Authorization is implemented but must be supplied
with trusted, externally authenticated actor contexts.
The SQLite file and hash chain are not tamper-proof against the database
administrator, by design.

## SQLite operation history (schema v3)

Each complete snapshot replacement, undo, and redo is stored as an immutable
`operation_events` entry in the **same transaction** as the corresponding
working snapshot update and generation increment. Each entry has a UUIDv7,
actor UUID, revision/branch/project UUID, timestamp, operation type, target
when undoing/redoing, and before/after snapshot contents. History survives
reopening the DB. Existing history is never erased to implement Undo.

`historyForRevision()` reads old finalized revision events (subject to
`project.history.read`). Finalization stores a separate SHA-256 digest of that
revision's operation journal, and `verifyProject()` verifies it. **For legacy whole-snapshot revisions, the journal seal is not included
in the V2 parent hash chain yet**; the verifier checks it separately.
For object-mode revisions, the manifest hashes its object journal digest, and
the existing V2 chain authenticates that manifest.

This is a deliberately transitional *whole-snapshot* implementation. It has
O(snapshot size) storage per edit, and undo/redo reject divergent state rather
than attempting semantic merges. V4 starts incremental per-object storage and a separate object history,
but full reversible grouped change sets are still to be implemented. Older edits from before the schema-v3 migration
cannot be reconstructed into journal events, and only working-revision edits
are undoable. User/security audit events remain future work.

## Share the repository for development

`./tools/zip_db.sh` creates `../db.zip`, packaging tracked and non-ignored
untracked files (including the initialized Model submodule source tree)
without Git metadata, secrets, or ignored build artifacts.
To choose another path, run `./tools/zip_db.sh /path/to/db.zip`.

## Granular object storage (initial V4 foundation)

A new `putObject` / `deleteObject` / `object` / `objects` API persists
versioned, UUID-keyed model-object blobs with transactionally recorded
`object_events`. The first write to an empty working revision switches it
into object mode. Revisions with existing legacy whole-snapshot data stay in
legacy mode; mixing modes within a revision is prohibited.

Object-mode edits are guarded by **per-object optimistic versions**, allowing
unrelated objects to change without forcing a branch-wide expected generation
match. Deletions retain tombstones. Finalization hashes a deterministic
manifest of the complete object set **and edit journal**, while preserving
previously finalized hashes. Objects are copied into a new working revision
after finalization and when branching.

The low-level object API still accepts caller-supplied bytes for compatibility.
Prefer the V5 typed API for hydraulic networks and entities. See
[model-persistence.md](docs/model-persistence.md) for the binary contract,
serialization coverage, migration limitations and current risks.
