# AOWIS-SERVER-DB

C++ database library and optional local HTTP/REST server for AOWIS. The same
core works with **SQLite** (the default for standalone applications) and
**PostgreSQL** (for server deployments). The HTTP transport is optional:
applications can link the database library directly without starting a server.

This is an actively developed project. Both backends run through the shared
project, revision, authorization, and history logic, but deployment and
large-project performance still need hardening. **Do not expose the included
HTTP executable directly to the public internet.**

## Git-inspired, structurally shared revision storage

**AOWIS uses Git-inspired immutable, structurally shared snapshots:**
every finalized project revision is a complete logical model, but unchanged
objects and Merkle subtrees are reused across revisions and branches. This is
a foundational feature for large, long-lived water infrastructure networks,
not merely a compression option.

The storage architecture combines:

- **Canonical bulk construction.** Building an initial snapshot uses a
  single in-memory radix-tree construction and bounded immutable-content
  batches. PostgreSQL does not perform a network round trip for every
  intermediate Merkle node. The builder computes each immutable node hash
  once, using that same identity for its parent link and for batched insertion;
  reused database content remains independently verified. Incremental
  revisions still update only changed UUID paths, preserving identical roots.
- **Content-addressed immutable objects.** Serialized model-object bytes have
  SHA-256 identities. Identical bytes stored under the same kind are written
  once and can be referenced by many revisions or branches.
- **Persistent Merkle snapshot trees (finalized roots now integrated).**
  Finalized object-mode revisions carry an immutable root mapping model UUIDs
  to content identities. Editing one
  object creates a new object and copies only the small tree path needed to
  reach it. Everything else stays shared.
- **Efficient historical lookup.** Finalized single-object `object()` reads
  now use their immutable Merkle roots, without replaying revision diffs or
  consulting SQL revision-object rows. Tree depth depends on UUID length,
  **not revision count**. Finalized bulk object listings use breadth-first,
  batched Merkle-tree traversal and verified content reads instead of
  revision-object SQL rows. Finalized hydraulic-network model loads also
  resolve their root and referenced entities from sealed Merkle data.
- **Cryptographically linked revisions.** Finalized revision hashes now
  commit to the Merkle root (for object-mode revisions), edit-history digest,
  authorship, and parent
  revision hashes. Historical branches and merges remain immutable.
- **Normal editing and undo/redo.** A working branch retains its own state;
  model validation, permissions, edit groups, and durable undo/redo continue
  to operate on the same transactional core.
- **Both storage engines.** SQLite and PostgreSQL implement the same content
  hashing and tree-node semantics; neither backend defines the hash format.

### Implementation status

**Stage 1 — complete:** The immutable `content_objects` table and cross-backend
SHA-256 content-store API provide deduplicated blobs and integrity-checked
write-once storage, independent of the transport or persistence backend.

**Stage 2 — complete (internal foundation):** A persistent, prefix-compressed
16-way Merkle radix tree maps 128-bit object UUIDs to immutable blob hashes.
The tree supports deterministic roots, lookup, insert/update/delete, batched
changes, and full ordered enumeration. A change writes only its affected
search path, preserving older roots exactly; no history replay is necessary.
Its depth is bounded by the UUID's 32 hexadecimal digits, regardless of
whether the project has 10 revisions or 10,000. Regression tests run against
both SQLite and PostgreSQL.

**Stage 3 — physical storage cutover implemented:** Finalized object-mode revisions seal
and persist a Merkle root covering every model object, its version, and its
delete state. Consecutive revisions reuse their parent's root and update the
UUID paths changed in the operation journal. Historical single-object reads
now come from the sealed tree with strict decoding and hash checks. Finalized
snapshot manifests use immutable stored bytes; object-mode successors do not
copy the full manifest into another working-revision row. Batched
Merkle-tree traversal and blob loading now power finalized `objects()` reads,
including PostgreSQL. Historical `loadHydraulicNetwork()` also reconstructs
models from the sealed Merkle objects, independently of copied hydraulic
membership and SQL object rows. Three-way merge previews now read finalized
base/source states from ordered, batch-decoded Merkle snapshots; working
heads remain SQL-backed. Creating a branch now rebuilds its editable object
rows from the immutable finalized Merkle root, including exact versions and
tombstones; PostgreSQL uses bounded bulk inserts rather than a network round
trip per object. **New branches also rebuild hydraulic ownership and reference
indexes from their finalized Merkle model**, not historical SQL index copies.
Finalization now seals the Merkle root and **hands its already-validated working
SQL model and hydraulic indexes to the new editable revision with three
set-based SQL transfers**, then removes all old-revision materialized rows
within the same transaction. It does not repeatedly decode the complete
Merkle tree to reconstruct an identical working head. Finalized revisions
contain **no duplicated `revision_objects`, membership, or reference rows**.
Full verification checks every reachable immutable tree and blob and rebuilds
the independently recorded edit/group-history manifest. Merge safety continues
to check transient working-head rows against their parent's immutable root.

**Measured storage savings:** On the October 19, 2026 benchmark, SQLite
with 1,000 pipes and 50 revisions occupied **6.4 MB**, compared with
**126.9 MB** under full revision copying (about **95% less**). SQLite with
10,000 pipes and 10 revisions occupied **60.9 MB** versus **302 MB** before
(about **80% less**). Finalized object-mode revisions retain **no duplicated
SQL object rows**. These measurements are workload-dependent, not guaranteed
ratios for all projects.

**Performance tradeoff and active optimization:** Working heads still
materialize objects and derived hydraulic indexes for transactional edits.
Reconstructing those temporary rows from Merkle blobs after every finalization
caused significant latency. The finalization path now transfers the already-
validated SQL head transactionally, without a Merkle round trip per entity or
rebuilding hydraulic indexes. Branch creation still constructs an independent
working head from its finalized base Merkle root, because branches cannot
borrow another branch's mutable SQL state. Branch creation now builds
those derived indexes from the already-verified new working object rows,
without traversing the same finalized Merkle tree a second time. Missing
membership rows are fetched in bounded SQL batches, including during the
initial branch-index build. SQLite bulk writes and verification-local hash
caching remain in place. Branch creation and initial sealing are still
O(network size); further optimization should follow benchmarks, not guesses.

**Performance profiling:** The benchmark supports `--profile` to identify
first-seal bottlenecks separately from branch materialization and normal
incremental finalization. It reports opt-in, microsecond phase timings for
Merkle-tree construction, content hashing, database inserts and transaction
commits; this instrumentation is disabled during normal operation. See
[docs/performance.md](docs/performance.md#profile-first-seal-and-branch-phases-opt-in).

For forensic integrity audits, the library also exposes
`Database::verifyProjectJournal(actor, project_uuid)`. Unlike normal
`verifyProject()`, it independently replays the complete ordered object-event
journal from each first-parent snapshot and checks its result against the
sealed Merkle model. This is deliberately expensive and opt-in, not part of
ordinary finalization or the HTTP API. It includes two-parent merges (their
edits are relative to the first parent). See
[docs/git-inspired-storage.md](docs/git-inspired-storage.md#deep-journal-to-merkle-consistency-verification).

The content store is an internal primitive, not a new public REST endpoint.

Technical contract, full migration roadmap, and tradeoffs:
[docs/git-inspired-storage.md](docs/git-inspired-storage.md).

## Features and layout

- **Projects → branches → revisions:** permanent UUIDv7 identities from
  `AOWIS-SERVER-MODEL`; multiple independent projects and branches; one editable
  working revision per branch. Finalized revisions are immutable, SHA-256-linked
  to their parent revisions, and include author information in their integrity
  metadata.
- **Model and history:** typed hydraulic-network persistence, granular model
  objects and indexes, optimistic concurrency, grouped edits, persistent
  undo/redo, and branch comparison/merge with conflict handling.
- **Security:** users, groups, roles, permissions, scoped grants, local password
  credentials, revocable bearer sessions, password recovery, and security audit
  records. Both REST and embedded applications use the database core's
  authorization rules.
- **Storage:** SQLite and PostgreSQL backends, selected by INI configuration.
  No database migration system is maintained during pre-production development.

| Component | CMake target / executable | Purpose |
| --- | --- | --- |
| Core library | `aowis-db-core` (`AOWIS::DB`) | Embedded C++ database API; no HTTP dependency |
| HTTP library | `aowis-db-http` (`AOWIS::DBHttp`) | Optional Qt HTTP adapter |
| HTTP server | **`aowis-server-db`** | Local REST service; config-driven backend |
| Operator CLI | `aowis-db-admin` | Trusted local administration; config-driven backend |

`AOWIS-SERVER-MODEL` is a Git submodule under
`external/AOWIS-SERVER-MODEL`. HTTP clients such as the WebAssembly GUI use
REST instead of accessing SQLite or PostgreSQL directly.

## Requirements and build

- CMake **3.21+**, Ninja, Git, a C++20 compiler, and Python 3.
- Qt 6 **Core** and **Sql**, plus OpenSSL Crypto. Install the Qt SQLite driver
  (`QSQLITE`) and, for PostgreSQL, the Qt PostgreSQL driver (`QPSQL`).
- For the optional HTTP server: Qt 6 **Network** and **HttpServer** modules.
- For disposable PostgreSQL integration tests: Docker Engine and
  **Docker Compose v2** (`docker compose`). Docker is not required for regular
  SQLite builds.

```bash
./git_submodule_init.sh
./compile_linux.sh
```

The incremental Linux build uses `build-linux/`. With Qt HttpServer available,
the executable files are:

```text
build-linux/aowis-server-db
build-linux/aowis-db-admin
```

Important CMake options:

| Option | Default | Effect |
| --- | --- | --- |
| `AOWIS_DB_BUILD_HTTP` | `ON` | Build HTTP adapter/server if the Qt modules are installed |
| `AOWIS_DB_SERVER_MODE` | `OFF` | On Linux use `/etc` for config and `/var` for writable DB/log paths |
| `BUILD_TESTING` | Controlled by build script | Register and build CTest executables |

For a Linux system-service build, configure a separate build directory (or
reconfigure the existing one) with `-DAOWIS_DB_SERVER_MODE=ON`:

```bash
cmake -S . -B build-linux-server -G Ninja \
  -DAOWIS_DB_SERVER_MODE=ON -DBUILD_TESTING=OFF
cmake --build build-linux-server --parallel
```

The build scripts use the checked-out Model submodule. In a parent build,
`AOWIS_MODEL_SOURCE_DIR` can instead select another Model checkout.

## Configuration: `config.ini`

Both `aowis-server-db` and `aowis-db-admin` read the **same INI** through
Qt `QSettings` (`IniFormat`). An absent INI uses the defaults: it is *not*
automatically written. Use `--config /absolute/path/config.ini` on either
executable to load a different file.

Qt `QStandardPaths` chooses writable directories appropriate to the OS and
application name. Typical **Linux desktop** paths are:

| File | Default |
| --- | --- |
| Configuration | `~/.local/share/AOWIS-SERVER-DB/config.ini` |
| SQLite database | `~/.local/share/AOWIS-SERVER-DB/aowis.sqlite` |
| Log | `~/.local/share/AOWIS-SERVER-DB/logs/server.log` |

On Linux, the location respects `XDG_DATA_HOME`. On **macOS and Windows**, Qt
selects the platform's application data locations rather than hardcoded Linux
paths. For an **`AOWIS_DB_SERVER_MODE=ON` Linux build**, the defaults are:

| File | Default |
| --- | --- |
| Configuration | `/etc/aowis-server-db/config.ini` |
| SQLite database | `/var/lib/aowis-server-db/aowis.sqlite` |
| Log | `/var/log/aowis-server-db/server.log` |

The server-mode compile flag does not change those locations on non-Linux
systems. Provision `/var/lib/aowis-server-db` and `/var/log/aowis-server-db`
with suitable service-account permissions; `/etc/aowis-server-db` holds
configuration, not mutable project data.

### SQLite (default)

Example `config.ini` (each setting is optional):

```ini
[Database]
Backend=sqlite
File=/absolute/path/to/aowis.sqlite

[Logging]
File=/absolute/path/to/logs/server.log

[HTTP]
Port=8080
```

Omit `File` or `Logging/File` to use the defaults above. INI file-path overrides
must be **absolute**, nonempty paths. The HTTP port must be 1–65535; its
default is **8080**. The server creates the SQLite and log parent directories
when permissions allow. It appends diagnostic logs to the configured file and
also writes them to stderr. Arrange log rotation separately.

### PostgreSQL

```ini
[Database]
Backend=postgres
Name=aowis
Schema=aowis

[Logging]
File=/var/log/aowis-server-db/server.log

[HTTP]
Port=8080
```

`Name` and `Schema` are required for PostgreSQL. The PostgreSQL **database and
selected schema must already exist** and be accessible to the connection's
user; AOWIS creates its tables in an empty selected schema. Use a lowercase
schema identifier of letters, numbers, and underscores, not beginning with a
number. PostgreSQL host, port, username, and credentials come from libpq
(`PGHOST`, `PGPORT`, `PGUSER`, `PGSERVICE`, `.pgpass`, etc.), **not from the
INI**. Do not put secrets in the INI or source control.

`Database/File` is SQLite-only; `Database/Name` and `Database/Schema` are
PostgreSQL-only. Mixing these options is rejected.

## Running the server

The server uses the configured database backend and listens on
**127.0.0.1**, at the port set by `[HTTP] Port`:

```bash
./build-linux/aowis-server-db
./build-linux/aowis-server-db --config /absolute/path/config.ini
```

For a one-off **SQLite** override, you may optionally pass a filename (absolute
or relative to the working directory):

```bash
./build-linux/aowis-server-db /path/to/alternative.sqlite
./build-linux/aowis-server-db --config /absolute/path/config.ini /path/to/alternative.sqlite
```

This does **not** modify the INI. PostgreSQL mode rejects a positional SQLite
filename. There is no positional HTTP port option; configure it in the INI.

The server has session-authenticated REST routes under `/api/v1/`. The public
health endpoint can be checked locally:

```bash
curl http://127.0.0.1:8080/api/v1/health
```

Other routes require a valid bearer session, with permissions checked by the
core. Features include project/branch/revision reads, model operations,
finalization, history undo/redo, branch merges, session management, and
security administration. See the [HTTP session documentation](docs/http-api.md)
and [merge endpoint documentation](docs/history-and-merges.md) for details.

**Security boundary:** the supplied HTTP executable does not implement TLS
and deliberately binds only to loopback. Do not publish its port directly or
assume it is production-ready behind an arbitrary reverse proxy. Secure
network deployment, request isolation, observability, and operational recovery
still require additional work.

## Operator CLI (`aowis-db-admin`)

The CLI uses the configured backend: it **never requires a positional SQLite
database filename**. To operate on a different database, use its INI through
`--config` (this option may precede or follow the command). The CLI is a
**trusted local administration tool**; its administrator UUID argument is an
operator identity, not an HTTP login credential.

To initialize a new empty database and create the first system administrator:

```bash
./build-linux/aowis-db-admin bootstrap administrator
```

This prints the administrator's UUID. Keep it for subsequent commands. For
example (substitute the real UUIDs):

```bash
./build-linux/aowis-db-admin audit-verify ADMIN_UUID
./build-linux/aowis-db-admin audit ADMIN_UUID
./build-linux/aowis-db-admin --config /absolute/path/config.ini audit-verify ADMIN_UUID
./build-linux/aowis-db-admin disable-user ADMIN_UUID USER_UUID
./build-linux/aowis-db-admin enable-user ADMIN_UUID USER_UUID
```

Other supported commands:

| Command | Operands after the command | Purpose |
| --- | --- | --- |
| `issue` | `ADMIN_UUID USER_UUID [LIFETIME_SECONDS]` | Issue a revocable bearer token |
| `set-password` | `ADMIN_UUID USER_UUID` | Read the new password from **stdin** |
| `recover-password` | `ADMIN_UUID USER_UUID [LIFETIME_SECONDS]` | Issue a recovery token |
| `revoke` | `ADMIN_UUID SESSION_UUID` | Revoke a session |
| `remove-member` | `ADMIN_UUID MEMBERSHIP_UUID` | Remove group membership |
| `revoke-role-permission` | `ADMIN_UUID GRANT_UUID` | Revoke permission grant |
| `revoke-role-assignment` | `ADMIN_UUID ASSIGNMENT_UUID` | Revoke role assignment |
| `redact-sessions` | `ADMIN_UUID [MIN_AGE_DAYS [LIMIT]]` | Remove spent session credentials |

`bootstrap`, `audit`, `audit-verify`, `disable-user`, and `enable-user` are
shown above. Run the CLI without operands for its full syntax summary.

The `issue` and `recover-password` commands print **secrets to stdout** and
metadata to stderr. Protect their output (for example, `umask 077` before
redirecting to a file), never include credentials in command-line arguments,
and never commit tokens to the repository. Passwords for `set-password` are
read from stdin, not from the process argument list.

## Performance measurements

An optional [SQLite and PostgreSQL network benchmark](docs/performance.md)
measures storage, network load, single-entity edits, finalization and branch
creation at selected network sizes. PostgreSQL can be benchmarked using
`./tools/benchmark_network_postgres.sh 10000`, which starts and removes an
isolated Docker test instance. Both modes run separately from CTest, so large
workloads never slow the normal regression suite.

The bulk first-seal path uses insert-returned identities to avoid rereading
fresh immutable content, while independently verifying reused content and
retaining full SHA-256 checks on historical reads and integrity verification.
See [performance measurements](docs/performance.md) for profiling details.

## Tests: SQLite and real PostgreSQL

The Linux test runner builds the registered CTest executables and runs the
`aowis`-labelled suite:

```bash
./compile_linux_tests.sh
AOWIS_DB_TEST_POSTGRES=required ./compile_linux_tests.sh
```

The **required** setting is recommended for CI and for checking PostgreSQL
parity. The runner uses `tests/postgres/docker-compose.yaml` to start a
**disposable PostgreSQL 17** instance, waits for its health check, publishes
it only on an automatically chosen localhost port, and removes it after the
run. No application database or persistent test volume is used.

The live PostgreSQL checks run in the backend-contract and CLI integration
CTest executables. The server CLI test starts the actual `aowis-server-db`
process with PostgreSQL configured, checks authenticated HTTP project creation,
and verifies the created project persists after the server stops. These checks run
inside existing CTest entries; they do not add separate test numbers.

Test-runner switches:

| Setting | Behavior |
| --- | --- |
| `AOWIS_DB_TEST_POSTGRES=auto` (default) | Use Docker if available; warn and skip PostgreSQL otherwise |
| `AOWIS_DB_TEST_POSTGRES=required` | Fail if PostgreSQL cannot be started or tested |
| `AOWIS_DB_TEST_POSTGRES=off` | Explicitly skip live PostgreSQL testing |
| `AOWIS_DB_TEST_PG_DB=...` | Use a dedicated existing PostgreSQL test DB instead of Docker (with libpq settings) |
| `AOWIS_DB_TEST_JOBS=4` (default) | Number of concurrent CTest jobs (1–32) |

`./compile_linux_tests.sh --debug`, `--release`, and `--both` select separate
build configurations. Normal test builds use `build-linux-tests/` and are
incremental; **`ninja: no work to do` is normal if no sources changed**. A
passing CTest run with PostgreSQL explicitly disabled or unavailable does not
establish PostgreSQL compatibility.

For an external PostgreSQL test database, use a **dedicated disposable DB**
whose user can create/drop test schemas; never point tests at production data.
See [PostgreSQL Compose testing](docs/testing.md).

## Database schema and persistence contracts

During development, each backend maintains **one current schema**. SQLite's
schema is defined in `src/sqlite_schema.cpp` and PostgreSQL's in
`src/postgres_schema.cpp`. When a database is empty, its tables and security
catalog are initialized; an existing database is checked, **not migrated**.

There is no historical SQL migration machinery. If the schema changes, you may
remove a **disposable development database** and create a new one. Never delete
a database that contains data you need. Revision hash-format identifiers,
Model codec compatibility checks, and immutable history are separate from SQL
schema migrations and must remain stable.

The hydraulic Model codec currently checks persisted field coverage against
the checked-out Model during CMake configuration. The database does **not yet**
guarantee complete packaging of every non-hydraulic project domain or external
asset. Some advanced branch merge cases, large-network performance,
production-grade HTTP deployment, and operational recovery remain future
work.

## Working with ChatGPT patches

From the repository root, apply a patch, capture the new source ZIP, and run
all tests in that order:

```bash
./tools/apply_patch.sh ~/Downloads/PATCH_FILENAME.zip
./tools/zip_db.sh
AOWIS_DB_TEST_POSTGRES=required ./compile_linux_tests.sh
```

`apply_patch.sh` refreshes the modification timestamps of extracted files
**and directories**, so Ninja rebuilds changed sources. `zip_db.sh` produces
`../db.zip` containing the project source and checked-out Model submodule,
while excluding Git metadata and ignored build files. Upload that ZIP with
test output for the next development step.

## Further documentation

- [Architecture and integrity](docs/architecture.md)
- [Model persistence](docs/model-persistence.md)
- [Revision history, undo/redo and merges](docs/history-and-merges.md)
- [Identity and access](docs/identity-and-access.md)
- [Configuration and startup](docs/configuration.md)
- [SQLite and PostgreSQL backends](docs/storage-backends.md)
- [REST/HTTP API](docs/http-api.md)
- [Building and testing](docs/testing.md)

The documents describe current responsibilities and constraints, not the
chronology of development patches. Exact codec, audit, and revision hash format
identifiers are retained in code where required for stored-data compatibility.
