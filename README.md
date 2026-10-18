# AOWIS-SERVER-DB

C++ library and optional HTTP/REST server for AOWIS projects. Standalone uses
SQLite; PostgreSQL is under implementation. `AOWIS-SERVER-MODEL` owns the Model types and
UUIDv7 generator. WebAssembly clients communicate through the HTTP API.

## Current capabilities

- Multiple projects with separate branches and UUIDv7 revisions.
- One editable revision per branch; finalized revisions are immutable and
  SHA-256-linked, including parent hashes and author attribution.
- Typed hydraulic-model storage, granular objects, reference/membership
  indexes, edit history, grouped undo/redo, and branch merge operations.
- User, group, role and permission management, security audit records, bearer
  sessions, local password credentials, and recovery tokens.
- Embedded SQLite library and optional HTTP server using the same core.

## SQLite development database: one schema, no migrations

**There is only one current SQLite schema:** `src/sqlite_schema.cpp`.

Opening an empty SQLite database creates the full schema and built-in security
catalog together in one transaction. Opening an existing database checks that
its schema and security guards are present. It does not upgrade or silently
replace an older schema.

**No historical SQLite schema migrations are supported.** This repository is
still under active development and there is no production data to preserve.
After schema changes, delete your *development* SQLite file and let the DB
create a fresh one. Never do this to a database containing data you care about.

Revision hash format identifiers, model codec compatibility checks, and
immutable project history remain important and are **not** database schema
migrations. They must not be discarded merely because we rebuild the SQL
schema while developing.

## Build and test

Requires CMake 3.21+, C++20, Qt 6 Core/Sql/HttpServer (for HTTP builds),
OpenSSL, and the checked-out Model submodule. Python 3 is used for Model-codec
coverage checks.

```bash
./git_submodule_init.sh
./compile_linux.sh
./compile_linux_tests.sh
```

`./tools/zip_db.sh` produces `../db.zip` for development review.

For architectural and feature details, see [architecture](docs/architecture.md),
[model persistence](docs/model-persistence.md), and
[identity and access](docs/identity-and-access.md).

PostgreSQL has an initial public `Database::openPostgres(database, schema)` path
([V65](docs/postgresql-core-v65.md)), using the same core and authorization
logic as SQLite. Full feature parity is **not yet verified**: the live
PostgreSQL integration test is optional and must be run against a dedicated
test database before relying on PostgreSQL. SQLite remains the default.
Complete non-hydraulic project persistence and production-grade HTTP deployment
remain future work.
