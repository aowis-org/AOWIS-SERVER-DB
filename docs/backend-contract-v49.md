# V49 — SQLite baseline migration extraction

This patch continues the StorageBackend lifecycle boundary introduced by V48.
The schema version remains **13** and there is **no data migration** on
existing installations.

## Changes

- V1 SQLite schema DDL (tables, indexes, immutable-history triggers) is now
  wholly owned by `SqliteBackend::createInitialSchema`. The original SQL
  statements and ordering are unchanged.
- `StorageBackend::createInitialSchema` defines the transaction boundary:
  the `Database` opens and controls the transaction, while the backend only
  executes DDL. It does not commit, roll back or modify the version.
- `Database::migrate` delegates V1 creation to the backend; the existing
  version advancement and V2–V13 upgrades retain their prior order.
- The backend contract test validates V1 schema creation through the abstract
  interface, verifies one immutability trigger, and confirms a DDL failure
  followed by rollback leaves no initial tables.

## Not addressed in V49

V2–V13 migrations are still distributed among domain modules. SQLite-specific
SQL for those migrations must move to corresponding backend-owned migration
steps before a PostgreSQL implementation can run the same lifecycle.
The domain layer still assumes SQLite binary UUID binding and uses some
SQLite-specific SQL semantics. `openSqlite()` remains the only entry point.

## Tests

Run `./compile_linux_tests.sh` on the development machine. The 49 existing
CTest suites remain registered, with the backend contract test expanded.
