# V48 — Engine contract and SQLite metadata boundaries

This patch continues V47 without changing SQLite schema v13, domain API calls,
revision-hash inputs, Model serialization bytes, or stored database records.

## Implemented

- Internal `StorageBackend` interface, owned by each `Database` instance.
  Connection creation, pragmas/configuration, schema verification/version,
  write transactions and rollback are dispatched through this interface.
- `SqliteBackend` is the only implementation at present. `openSqlite(...)`
  remains the only supported entry point. No PostgreSQL support is claimed.
- Schema introspection (`hasColumn`) moved from the V10 security-grant
  migration into the adapter. Unlike the former boolean implementation, it
  distinguishes a missing column (`false`) from an invalid identifier or SQL
  metadata error (`std::nullopt`). The migration aborts on metadata errors.
- SQLite configuration verifies that foreign keys really are enabled,
  the busy timeout is set, and the connection's journal mode matches the
  on-disk (WAL) or in-memory (memory) mode. A successful SQL statement alone
  is not sufficient: SQLite may ignore a foreign-key PRAGMA mid-transaction.
- The existing backend contract test now dispatches through the abstract
  interface. Added checks for invalid identifiers, missing/present columns,
  configuration failures in an open transaction, and an in-memory database.

## What remains backend-specific

Existing V1–V13 migration definitions and SQL queries in the domain files are
still SQLite-specific. The new interface is a **lifecycle boundary**, not a
finished portable data-access layer. Some migration SQL uses triggers,
`WITHOUT ROWID`, `COLLATE NOCASE` and `PRAGMA`s. Domain SQL binds UUIDs as
16-byte RFC 4122 BLOBs. Neither this SQL nor SQLite locking guarantees can be
assumed to work unchanged on PostgreSQL.

Before a PostgreSQL backend is offered, move backend-specific migration DDL,
UUID storage/binding conventions, conflict-upsert differences and concurrency
behavior behind explicit adapters. Keep the Model codec and finalized hash
formats unchanged. A PostgreSQL adapter must have its own schema and verified
immutability/authorization constraints, followed by parity tests covering all
read/write paths, migrations, and concurrent transactions.

## Validation

Run `./compile_linux_tests.sh` and CTest on the normal development host.
The changes are intended to leave all existing tests in place; no database
migration is required to install this patch.
