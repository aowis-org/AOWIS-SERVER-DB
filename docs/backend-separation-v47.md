# V47 — Database backend boundary, stage 1

The installed backend is still **SQLite only**. V47 is a refactoring of its
connection and schema lifecycle, not an implementation of PostgreSQL.

## SQLite adapter responsibilities

`src/sqlite_backend.h` and `src/sqlite_backend.cpp` own:

- Opening the named Qt `QSQLITE` connection.
- Enabling foreign keys, WAL mode, and the five-second busy timeout.
- `BEGIN IMMEDIATE`, `COMMIT`, and `ROLLBACK` transaction primitives.
- Reading and setting the SQLite `PRAGMA user_version`. Cursor finalization
  happens *before* running migration DDL.
- Post-migration checks of the security-audit, grant, password, recovery-token,
  and session-redaction schema guards, which must fail closed.

Every V1–V13 migration still calls the same transaction helpers, but schema
version writes use `Database::setSchemaVersion` rather than inlining PRAGMAs.
The current SQLite schema remains V13. No existing database is rewritten merely
because of this refactoring.

`tests/backend_contract_test.cpp` tests adapter transaction, FK, journal-mode,
version rollback, connection lifecycle, and schema-guard failure behavior. The
existing migration tests cover V1→V13 preservation and historical hashes.

## PostgreSQL work still required

PostgreSQL is **not supported** by `Database::openSqlite`, and neither
`QPSQL` nor a PostgreSQL schema should be passed through this adapter. Before
introducing a production PostgreSQL entry point:

1. Implement PostgreSQL-specific schema/migrations with equivalent immutable
   revision, audit, recovery, and permission invariants. SQLite triggers and
   `sqlite_master` checks do not translate directly.
2. Define a PostgreSQL schema-version table and migration lock (the SQLite
   adapter currently uses `PRAGMA user_version`).
3. Map UUID/BLOB, integer, case-folding, and binary comparison semantics
   explicitly. SQLite stores RFC 4122 UUIDs as 16-byte BLOBs; PostgreSQL native
   UUID types and byte arrays require a deliberate serialization contract.
4. Map SQLite's `BEGIN IMMEDIATE` single-writer concurrency assumptions to
   PostgreSQL transaction isolation, row/advisory locks, and optimistic
   generation checks. Avoid relying on an unprotected read-then-write.
5. Audit the remaining SQLite-specific SQL in the domain layer, including
   `WITHOUT ROWID`, trigger `RAISE(ABORT, ...)`, `AUTOINCREMENT`,
   `COLLATE NOCASE`, `PRAGMA table_info`, and `hex(...)`. These are **not**
   made portable by V47.
6. Run the complete database contract tests on both engines before advertising
   PostgreSQL support; also test simultaneous writers and migration recovery.

Neither the Model codec's canonical serialized bytes nor historical revision
hash inputs should change during database-backend work.
