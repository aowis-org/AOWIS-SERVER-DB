# V62 — PostgreSQL backend foundation

This introduces `PostgresBackend`, an internal implementation of the existing
`StorageBackend` contract, using Qt SQL's `QPSQL` plugin.

- Native PostgreSQL `uuid` parameters are bound as `QUuid` variants, never as
  SQLite's 16-byte `BLOB` values. Typed UUID results and text-cast UUIDs can
  be decoded. SQL NULL remains distinct from the all-zero UUID in queries.
- The connection takes **only a database name**. libpq obtains host, port,
  username and authentication settings from `PGHOST`, `PGPORT`, `PGUSER`,
  `.pgpass`, or the usual PostgreSQL client configuration. Use an isolated test
  database when experimenting with the adapter.
- A write transaction acquires a transaction-scoped PostgreSQL advisory lock
  before the core performs read/modify/write operations. Connection setup also
  limits lock waits to five seconds. This serializes
  AOWIS cooperating writers in the same way SQLite uses `BEGIN IMMEDIATE`.
  AOWIS's eventual PostgreSQL implementation must consistently use this lock;
  it is not a defense against administrators issuing arbitrary SQL.
- Table and column existence queries use PostgreSQL system catalogs and
  parameterized schema-inspection SQL instead of SQLite PRAGMAs.
- **PostgreSQL schema creation and verification deliberately fail closed.**
  The current 88-definition schema contains SQLite-specific constructs and
  guards. Porting and testing all constraints and domain SQL is a separate
  stage. Do not expose PostgreSQL via `Database` until this is completed.
- No schema migration system is introduced; SQLite remains the default and
  its schema, serialized snapshots, UUID byte encodings inside hashes, and
  audit/revision history are unchanged.

## Tests

The existing `aowis-db-backend-contract` executable checks PostgreSQL UUID
binding and fail-closed behavior without requiring a PostgreSQL server.

For a real PostgreSQL UUID/transaction smoke test, install the matching Qt
`QPSQL` driver and provide the name of an **already-created test database**:

```bash
export AOWIS_DB_TEST_PG_DB=aowis_db_test
export PGHOST=localhost
export PGUSER=aowis_test
# Configure credentials using ~/.pgpass or other libpq mechanisms.
./compile_linux_tests.sh
```

When `AOWIS_DB_TEST_PG_DB` is unset, the live PostgreSQL part is skipped.
The live test uses a temporary table inside a rolled-back transaction and
never creates or changes permanent database tables. Tests do not require a
PostgreSQL server by default, so the CTest count remains 48.
