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

No production HTTP API, user authentication, password/session handling, model validation,
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
but grouped changes and object-level undo/redo are now implemented (V6). Older edits from before the schema-v3 migration
cannot be reconstructed into journal events, and only working-revision edits
are undoable. Security administration events are now audited separately in schema V9.

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

## Grouped persistence and undo/redo: V6

Use `commitObjectGroup(actor, branch, expected_generation, edits)` to apply
multiple optimistic-versioned object changes as one durable action. The
operation rolls back completely on any failure. `undoObjectGroup` and
`redoObjectGroup` persist inverse/forward changes without deleting prior
history. Typed whole-network saves, single-object puts and deletes now produce
undoable groups when they change state. A new V5 SQLite schema migration
protects grouped history; the finalized manifest covers it without changing
old V4 hashes. See [object-history-v6.md](docs/object-history-v6.md) for
exact semantics and limitations.

## Hydraulic model reference validation (V7)

`saveHydraulicNetwork` checks non-null inter-entity references, typed UUID
relationships (nodes, links, patterns, curves, controls, demand points and
MSX), plus pipe-position and assigned-junction constraints before committing.
Use `validateHydraulicNetwork` to retrieve individual diagnostics. Incomplete
drafts with null link endpoints remain valid. Low-level object writes and
object-level undo/redo are **not yet** transactionally validated as full models.
See [model-validation-v7.md](docs/model-validation-v7.md).

## Transactional hydraulic reference validation (V8)

Indexed hydraulic entity edits, low-level grouped object commits, and grouped
undo/redo validate the final network graph before committing. Invalid changes
roll back both state and history. See
[docs/model-object-integrity-v8.md](docs/model-object-integrity-v8.md).

## Indexed network membership (V9; SQLite schema version 6)

`hydraulic_network_members` maps each indexed hydraulic entity to its owning
network within a revision. A low-level edit now uses indexed ownership lookup
and reconstructs only the affected network, rather than scanning every root
and all unrelated model objects. The migration backfills existing finalized
and working revisions without modifying their hashed snapshots. See
[model-membership-v9.md](docs/model-membership-v9.md) for details and limits.

## V10 reference index

Schema V7 maintains a derived reverse-reference index for hydraulic networks.
Simple pipe/junction attribute edits can skip whole-network reconstruction when
all reference-sensitive fields are unchanged. See [V10 notes](docs/model-references-v10.md).

Full-network SQLite saves use bounded indexed lookups limited to the affected
network, without scanning objects from unrelated networks. See
[model-indexed-save-v11.md](docs/model-indexed-save-v11.md).

## Optional development HTTP transport (V17)

When Qt6 HttpServer and Network are installed, CMake additionally builds
`aowis-db-http`, the local-only `aowis-db-server` executable, and the
`aowis-db-http` network test. It does not change the SQLite core or require
HTTP for embedded deployments (`-DAOWIS_DB_BUILD_HTTP=OFF` disables it).
Read [docs/http-foundation-v17.md](docs/http-foundation-v17.md) for the
current endpoints, authentication boundary, setup, and security limitations.
This is a local development transport, **not** an internet-facing service.

## V18 REST model and history operations

The optional HTTP adapter now supports full typed hydraulic network reads and
atomic saves, generic non-hydraulic grouped object writes, and persistent
object-group undo/redo and history reads. All requests use the same core
permissions and transactions as embedded applications. See
[docs/http-model-and-history-v18.md](docs/http-model-and-history-v18.md)
for endpoint contracts, request size limits, and the current security boundary.

## V19 persistent HTTP sessions (SQLite schema V8)

The single static development bearer token has been removed. The HTTP server
now resolves random, per-user bearer sessions through the DB core. Only the
SHA-256 token digest is persisted, with expiry and revocation; disabling a user
blocks all their sessions. A local operator can provision sessions through
`aowis-db-admin` and the server remains bound to loopback. See
[docs/http-sessions-v19.md](docs/http-sessions-v19.md) for provisioning,
security boundaries and limitations. Existing revision hashes are unchanged.

## Security administration audit (schema V9)

Security mutations now append hash-linked audit records atomically. Authorized
operators can page and verify the journal through the C++ API or via
`aowis-db-admin audit` / `aowis-db-admin audit-verify`. See
[docs/security-audit-v20.md](docs/security-audit-v20.md). Existing revision
hashes and model data are unaffected.

## Security administration lifecycle (V21)

Grants and memberships can now be revoked and later re-granted using fresh
UUIDs, without deleting the original relationships. Disabled users can be
reactivated, but must receive new sessions. All successful changes are atomic
with the security audit chain. See [security-admin-v21](docs/security-admin-v21.md).

## Security administration catalog (V22)

Administrative catalog reads are available through the C++ `Database` API
and authenticated, system-admin-only HTTP GET routes. Pages use UUID cursors
and include revoked grants only when explicitly requested. See
[docs/security-catalog-v22.md](docs/security-catalog-v22.md).
