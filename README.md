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

Requirements: CMake 3.21+, a C++20 compiler, Qt 6 Core/Sql, the Qt SQLite SQL
plugin, and a checkout of `AOWIS-SERVER-MODEL` (next to this repository by
default). The Model's existing `createUuidV7()` function is used for IDs.

```bash
cmake -S . -B build-linux -G Ninja -DBUILD_TESTING=ON
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

Alternatively set `-DAOWIS_MODEL_SOURCE_DIR=/path/to/AOWIS-SERVER-MODEL`.
The library target is `aowis-db-core` (`AOWIS::DB` alias). All Qt SQL connections
are thread-affine: create one `Database` instance per thread, not one shared
connection across threads.

## Current scope and limitations

For local initialization, the trusted application must call
`bootstrapAdministrator(username)` once, then pass an `OperationContext` with
that actor's UUID to all project APIs. Remote callers must be authenticated by
the future server (not implemented yet). Database files in schema v1 are
upgraded transactionally to v2 without changing old hashes.

The core currently persists the project/branch/revision lifecycle and complete
**opaque snapshot bytes**, not individual AOWIS Model entities. The
`replaceWorkingSnapshot` boundary is temporary: callers are responsible for
supplying deterministic serialization of the *entire* project state, including
managed attachments, once the Model serializer is implemented. Arbitrary test
bytes are **not** a valid production project format.

No HTTP API, user authentication, password/session handling, model validation,
operation log, object-level optimistic locking, merge handling, or PostgreSQL
backend is implemented yet. Authorization is implemented but must be supplied
with trusted, externally authenticated actor contexts.
The SQLite file and hash chain are not tamper-proof against the database
administrator, by design.

## Share the repository for development

`./tools/zip_db.sh` creates `../db.zip`, packaging tracked and non-ignored
untracked files without Git metadata, secrets, or ignored build artifacts.
To choose another path, run `./tools/zip_db.sh /path/to/db.zip`.
