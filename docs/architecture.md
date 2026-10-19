# Database architecture

AOWIS-SERVER-DB is a C++ database library with an optional REST/HTTP adapter.
The embedded library and HTTP adapter invoke the same domain operations;
security, validation, history, and finalization are not duplicated in routes.

## Data model

A **project** contains **branches**, and each branch has **revisions**.
Projects, branches, revisions, users, groups, roles, permissions, and model
objects carry stable UUIDs. New IDs use `createUuidV7()` from
AOWIS-SERVER-MODEL. Revision numbers identify a sequence *within a branch*,
not a globally unique revision.

Each active branch has one editable working revision. Finalized revisions are
immutable. Finalizing a working revision atomically records its canonical
snapshot and integrity metadata and creates the next working revision.
Branches may start from a finalized revision on another branch. Merge commits
have ordered parent revisions and use the same integrity machinery. Three-way
merge, supported field-level conflict resolution, and the limitations of that
merge algorithm are documented in [History and merges](history-and-merges.md).

## Components

| Component | Source / target | Responsibilities |
| --- | --- | --- |
| Database core | `aowis-db-core`, `include/aowis/db/database.h` | Project and revision lifecycle, permissions, transactions, history, merge logic |
| Model codec | `src/model_codec.cpp` | Deterministic hydraulic entity encoding and decoding |
| Model persistence | `src/model_persistence.cpp` | Network reconstruction, UUID membership and hydraulic reference indexes |
| SQLite adapter | `src/sqlite_backend.cpp`, `src/sqlite_schema.cpp` | Local database connections, current schema and SQLite SQL conventions |
| PostgreSQL adapter | `src/postgres_backend.cpp`, `src/postgres_schema.cpp` | Native PostgreSQL UUIDs, connection, schema and transactions |
| HTTP adapter | `aowis-db-http`, `src/http_routes.cpp` | Authenticated REST endpoints using the same database core |
| Server executable | `aowis-server-db` | INI configuration, logging and loopback HTTP listener |
| Operator CLI | `aowis-db-admin` | Trusted local administrative actions |

## Storage and concurrency

The SQLite backend enables foreign keys and uses write transactions to keep
snapshots, object rows and history journal entries consistent. The PostgreSQL
backend provides the equivalent transaction and lock behavior. The core
uses optimistic generations for working revisions, plus per-object expected
versions for granular object editing. A stale mutation must fail rather than
silently overwrite another user's edit. Cross-object structure validation
protects hydraulic references, not just individual object fields.

The two databases have **one current schema each**. There is no historical
SQL migration runner. Empty databases are initialized, and existing database
schemas are validated. During development, discard only genuinely disposable
database files when changing the schema. Do not remove data you intend to keep.

## Integrity and pre-release format

Finalized revisions are protected by SHA-256 digests over the deterministic
persisted snapshot, operation history, creator/finalizer identities, immutable
revision metadata and ordered parent hashes. The one current revision-chain
format is defined in [History and merges](history-and-merges.md).

This is pre-release software: we deliberately do not implement old revision
hash formats or SQL migrations. Disposable development databases must be
recreated when an incompatible schema or hash format change is introduced.
Freeze these contracts before the first release; only then plan migrations.

The database does not protect against a privileged administrator intentionally
rewriting data and recomputing its hashes. SQL triggers provide application
integrity guards, not a cryptographic security boundary against the owner of
the database file or a PostgreSQL superuser.

For object semantics, reversible history and branch merging, see
[Model persistence](model-persistence.md) and
[History and merges](history-and-merges.md). For identities and authorization,
see [Identity and access](identity-and-access.md).
