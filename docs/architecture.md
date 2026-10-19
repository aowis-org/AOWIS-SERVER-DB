# Architecture of the Distributed Versioned Model Database

AOWIS-SERVER-DB is the model-storage and history foundation for AOWIS. The
**Distributed Versioned Model Database** name describes the intended full
product: independent repositories, versioned models, distributed collaboration
and domain-aware merging. **Distributed synchronization is not implemented
yet.** The working system is a C++ database library with SQLite and PostgreSQL
backends and an optional authenticated HTTP/REST service.

## Model and revision graph

**Project → Branch → Revision** is the public model. Projects, branches,
revisions, identities and model objects carry stable UUIDs; new IDs are
created with `AOWIS-SERVER-MODEL`'s `createUuidV7()`.

Each branch has one editable working revision. Finalization seals a model state,
its history and immutable revision metadata and advances the branch to a new
working revision. A branch may begin at an immutable historical revision.
Finalized revisions are immutable, and ordered parent links allow merge
commits. Revision numbers are branch-local counters, not portable identities
for a distributed history.

## Layers and code ownership

| Layer | Component | Responsibility |
| --- | --- | --- |
| Model/version service | `aowis-db-core`; `include/aowis/db/database.h` | Project and revision lifecycle, transactions, history, merge orchestration, permissions |
| Immutable model storage | `src/content_store.cpp`, `src/merkle_tree.cpp`, `src/revision_root.cpp` | Shared snapshot trees, content hashes, historical object reads and verification |
| Hydraulic model representation | `src/model_codec.cpp`, `src/model_persistence.cpp` | Deterministic model data, entity ownership, references and hydraulic validation |
| Relational storage adapters | `src/sqlite_backend.cpp`, `src/postgres_backend.cpp` and schema sources | Driver connections, backend SQL, transactional storage, UUID parameter conversion |
| Access and security | `src/identity.cpp`, `src/security_*.cpp`, `src/sessions.cpp` | Users, groups, permissions, credentials, authorization and audit |
| HTTP API | `aowis-db-http`, `src/http_routes.cpp` | Authenticated REST adapter, using the same core checks |
| HTTP executable | `aowis-server-db` | Configuration, logging and loopback HTTP service |
| Operator tool | `aowis-db-admin` | Local database administration |
| Distributed repositories | **Future subsystem** | Repository identity, clone/fetch/push/pull, remote refs and authenticated history exchange |
| Snippet library | **Future subsystem** | Portable network selections, variants and atomic reusable applications |

## Versioned storage

Current finalized object-mode revisions are complete logical model snapshots
represented by immutable content-addressed records and persistent shared
snapshot trees. Revisions share unchanged content rather than retaining
separate historical SQL model rows. Finalized roots carry object versions and
tombstones. Historical reads, model loading and merge inputs use these roots.

Working revisions have materialized SQL rows and hydraulic membership/reference
indexes for efficient transactional edits. Finalization hands the validated
working state to the next working revision inside the same transaction, while
removing SQL model/index rows from the sealed predecessor. Creating a new
branch instead materializes an independent working state from its finalized
base root. See [Versioned storage](versioned-storage.md) for the canonical
byte-level format and integrity contract.

## Concurrency and validation

SQLite uses foreign-key enforcement and database transactions. PostgreSQL uses
transactions and schema-scoped advisory write locking. The core checks working
revision generations and expected object versions to reject stale edits.
Hydraulic reference and structural rules are validated independently of SQL
foreign keys. Grouped undo/redo is recorded rather than deleting earlier
audit events. Merge commits are atomic, but domain conflicts require explicit
resolution and validation. See [History and merges](history-and-merges.md).

## Access and trust

Users, groups, roles, permissions and session credentials are managed by the
core, not solely by REST routes. Both embedded and HTTP operations must
respect authorization. Immutable SHA-256 hashes detect unexpected content
changes; they do not authenticate the publisher or prevent a privileged
administrator from rewriting a complete database and its integrity hashes.
Future replication requires its own authenticated import/publish boundary.
See [Identity and access](identity-and-access.md).

## Planned distribution and snippets

A full DVMD will let independently stored local/offline repositories exchange
immutable project history with server repositories. Branch updates must be
conditional; different replicas may finalize divergent descendants of the
same revision, requiring explicit integration. The current REST server is
centralized access, **not** synchronization. See
[Distributed repositories](distributed-repositories.md).

Reusable [network snippets](snippets.md) will let engineers copy selected
connected network entities across projects and branches, with new UUIDs,
reference remapping and an atomic edit group. Duplicated variants and optional
scenario composition are future workflows on top of the same versioned model
foundation. No snippet APIs have been implemented yet.

## Current limitations and development status

- No clone/fetch/push/pull, remote-tracking branches or remote authorization.
- No snippet-library or scenario-composition APIs.
- No reachability-based immutable-content garbage collector.
- HTTP binds to loopback without TLS; public deployment, backup/recovery and
  operational monitoring require further engineering.
- Merge coverage and large-network behavior require continued tests and
  workload-specific benchmarking.
- The supported typed model domain is currently hydraulic networks; other
  AOWIS model domains and external files are not automatically packaged.
- Pre-release databases use one current schema per backend. No obsolete
  development schema or historical hash migration is supported. Recreate only
  genuinely disposable development databases after incompatible changes.
