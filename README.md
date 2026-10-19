# AOWIS-SERVER-DB — Distributed Versioned Model Database

**AOWIS-SERVER-DB (DVMD)** is the database foundation for AOWIS engineering
models.

AOWIS-SERVER-DB provides a C++ database library and an optional HTTP/REST
server. It supports embedded **SQLite** repositories and **PostgreSQL** server
repositories through the same model, history and authorization APIs.

**Product direction:** An AOWIS project should be usable independently on a
local computer, including offline, and synchronized with other repositories
when a connection is available. **Distributed repository synchronization is a
first-class architectural goal, not an implemented feature yet.** Today's
SQLite and PostgreSQL backends provide local or centralized access; they do
not currently offer clone/fetch/push/pull or offline replication.

This is pre-release software. The HTTP executable binds to loopback and is not
qualified for direct public-internet exposure. Storage formats and SQL schemas
are still subject to change before the first stable release.

## How model versioning works

The core model is **Project → Branch → Revision**:

- A **project** contains an engineering model and independent branches.
- A **branch** has one editable working revision. Engineers can branch from an
  immutable revision and develop alternatives without overwriting history.
- **Finalizing** a revision preserves a complete, immutable model state,
  records its edit history and parent relationships, and advances the branch
  to a new editable revision.
- **Comparing and merging** branches uses shared ancestry, structured model
  differences and explicit conflict handling. Model validation still applies.
- **Undo and redo** use persistent, grouped edit history; finalized revisions
  are never rewritten to undo an engineering decision.

Although a finalized revision represents a complete logical model, it does
**not** contain a second physical copy of every unchanged pipe, junction or
other object. The database stores unchanged content once and reuses it across
snapshots. This makes long-lived network histories practical without making
engineers manage file copies or manually replay edits.

Under the hood, this uses immutable content-addressed objects and a shared
Merkle index. Those are **implementation details**, not concepts users need to
understand in order to work with projects, branches or revisions. The
[versioned storage reference](docs/versioned-storage.md) documents the exact
formats, integrity rules and performance characteristics.

## What works today

| Capability | Status |
| --- | --- |
| Project, branch and revision lifecycle | Implemented |
| Finalized snapshots with shared unchanged content | Implemented |
| Revision ancestry, comparison and supported three-way merges | Implemented |
| Transactional working models, optimistic edits and grouped undo/redo | Implemented |
| Typed hydraulic networks and structural/reference validation | Implemented |
| Users, groups, roles, permissions, password login, sessions and audit | Implemented; deployment hardening remains |
| SQLite embedded and PostgreSQL server storage | Implemented |
| C++ library, optional HTTP server and administration CLI | Implemented |
| Clone, fetch, push, pull and synchronization between repositories | **Planned, not implemented** |
| Reusable network snippets and snippet libraries | **Planned, not implemented** |
| Composable planning scenarios using snippet variants | **Longer-term design, not implemented** |

## Distributed repositories — required future capability

A local AOWIS repository should ultimately be a **full participant**, not
merely an online cache of the server. For example, an engineer should be able
to clone a project to a SQLite database, edit and finalize revisions offline,
then exchange history with a PostgreSQL-backed server or another AOWIS
repository. Divergent work must remain identifiable and mergeable.

The distributed design must preserve revision and object identities across
repositories, transfer only missing immutable data when possible, support
remote-tracking branches and conditional branch updates, and validate incoming
history and hydraulic model structure. Authorization, provenance and trust must
be enforced at the receiving repository; content hashes alone do not establish
permission to publish changes.

**Not yet available:** repository IDs/remotes, clone/fetch/push/pull, sync
protocols, remote branch tracking, offline exchange, partial clones and
replication authorization. Opening the same server over REST is **not**
distributed version control. Current code should retain compatibility with
this target, but no application should assume synchronization already works.

See [Distributed repository design](docs/distributed-repositories.md) for
invariants, proposed operations and unresolved decisions.

## Network snippets — planned

A **snippet** is a reusable, self-contained selection of network entities
and their internal relationships: for example, a pumping station with pipes,
pumps, valves, curves, controls and connection points. Engineers should be
able to save snippets in a library, duplicate and edit variants, and insert
them into other networks, branches or projects.

Ordinary insertion creates **new UUIDs** for inserted entities while keeping
internal connections consistent. External references and connection ports
must be bound explicitly to the destination, with compatible coordinates,
elevations, units and dependencies validated before application. An insertion
should be **one atomic, undoable operation**, with provenance showing which
snippet definition/version was used.

A future planning workflow could also apply a pinned snippet variant to
**existing entities** or include/exclude it as part of a scenario based on an
immutable revision. That requires stable target identities, preconditions,
conflict checks and deterministic composition. **A scenario never changes its
baseline finalized revision by toggling a snippet.** These are design goals,
not shipped APIs or data formats.

See [Snippet and scenario design](docs/snippets.md).

## Persistence and integrity

SQLite is suited to standalone applications; PostgreSQL serves shared
installations. Both use the same canonical object encoding, immutable revision
roots and history verification. An editable working head has relational
objects and hydraulic indexes for efficient transactions; finalized model
states retain immutable roots instead of historical SQL copies.

Normal integrity verification checks stored objects, revision ancestry and
history seals. An optional forensic verification additionally replays object
journals to compare their effect against the finalized model. Neither
mechanism protects against a privileged database owner rewriting *all* data
and recomputing hashes. Reachability-based garbage collection and operational
backup/recovery are still future work.

The optional [network benchmark](docs/performance.md) measures storage,
loads, edits, finalization, branch creation and history verification on both
backends. Results depend on model size, revision patterns, backend and hardware.

## Libraries and executables

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

The opt-in [SQLite and PostgreSQL network benchmark](docs/performance.md)
measures model import and load, single-entity edits, finalization, independent
branch creation, history verification and physical storage use. For 100,000-
pipe stress cases, `--memory` reports benchmark-process RSS and
`--deep-verify` enables forensic journal replay. These runs are intentionally
separate from the routine CTest suite.

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

## Further documentation

- [Architecture and system boundaries](docs/architecture.md)
- [Versioned storage and integrity formats](docs/versioned-storage.md)
- [Distributed repositories — future design](docs/distributed-repositories.md)
- [Reusable snippets and scenario composition — future design](docs/snippets.md)
- [Hydraulic model persistence](docs/model-persistence.md)
- [Revision history, undo/redo and merges](docs/history-and-merges.md)
- [Identity and access](docs/identity-and-access.md)
- [Configuration and startup](docs/configuration.md)
- [SQLite and PostgreSQL backends](docs/storage-backends.md)
- [REST/HTTP API](docs/http-api.md)
- [Performance and profiling](docs/performance.md)
- [Building and testing](docs/testing.md)
