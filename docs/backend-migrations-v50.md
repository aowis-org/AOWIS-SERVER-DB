# V50 — SQLite schema DDL extraction for V2–V5

This continues the V48/V49 backend contract work. There is **no new schema
version**: current SQLite schema stays at V13 and existing databases require
no data upgrade just to adopt this patch.

## Changes

- `StorageBackend::applyMigrationSchema(database, target_version, error)` is
  the backend-owned schema-DDL boundary for historical migration steps.
- `SqliteBackend` implements V2 (users/permissions), V3 (snapshot operation
  history), V4 (revision objects and object events), and V5 (object groups).
- These migrations' **47 SQL statements are copied byte-for-byte** from the
  original domain source. They are now in `src/sqlite_schema_migrations.cpp`.
- Existing `Database::migrateSecurityV2` remains responsible for seeding the
  stable built-in role/permission UUIDs. Each domain migration still owns its
  write transaction, migration order, schema-version advancement and rollback.
- Unsupported schema steps fail explicitly; they must not be treated as
  successful no-ops. V6–V13 are **not yet extracted**.

## Atomicity and historical compatibility

Calling the adapter does not commit or update `PRAGMA user_version`. The caller
must hold the transaction and roll it back after any failed DDL or data
backfill. Existing migration methods retain their transaction boundaries, so
upgrading databases from V1–V4 still proceeds in the same order as before.

The backend contract test now exercises V1–V5 creation, per-version advancement,
unsupported migrations, duplicate-DDL failure, and an induced failure in the
middle of V2 (a conflicting `roles` table). It verifies rollback removes tables
created before the failure and preserves the preceding schema version.

## Next

Extract V6–V13 SQLite DDL (including hybrid backfill transitions), retaining
all data migration logic in the domain service until there is a PostgreSQL
migration counterpart. Afterward address UUID binding/storage and SQLite SQL
syntax in domain queries, then add PostgreSQL parity tests. Moving DDL does
**not** by itself make the domain layer backend independent.

## Verification

Run `./compile_linux_tests.sh` on the Qt-equipped development host. The
original 49 CTest registrations are unchanged. Regression tests must pass
before proceeding with further migration extraction.
