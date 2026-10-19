# SQLite and PostgreSQL storage

The database core owns project/revision semantics, Model persistence, access
control, history and transactions. Storage adapters own driver connections,
parameter conversion, backend-specific SQL and current-schema creation.
Both backends must expose equivalent results to the same public `Database` API.

## SQLite

`src/sqlite_backend.cpp` and `src/sqlite_schema.cpp` implement the embedded
SQLite backend. UUIDs use 16-byte RFC-4122 BLOB columns. Transactions and SQL
constraints protect references and finalized rows. SQLite is the default for
standalone applications. Its database path is set by the INI or, only for
`aowis-server-db`, by an optional command-line override.

Opening an existing SQLite database verifies the current schema against the
CREATE statements in `src/sqlite_schema.cpp` (tables, columns, keys, indexes,
and trigger definitions). Missing or modified definitions cause a clear failure;
there is no automatic migration or repair. SQLite preserves the original CREATE
SQL in `sqlite_master`, so even structurally equivalent hand-written DDL must
match the repository's current schema exactly. Unrelated additional SQL objects
are not blocked. The schema verification test deliberately damages disposable
databases to exercise these failures.

## PostgreSQL

`src/postgres_backend.cpp` and `src/postgres_schema.cpp` implement the QPSQL
backend. PostgreSQL stores UUIDs natively; `StorageBackend::bindUuid()` and
`readUuid()` handle driver-specific conversion. Importantly, an **all-zero UUID
is a UUID value, not SQL NULL**. PostgreSQL uses libpq connection settings
and an explicitly selected schema, which must exist before connecting.

PostgreSQL write transactions acquire a transaction-scoped two-key advisory
lock: a fixed AOWIS namespace and the selected schema's PostgreSQL namespace
OID. Writers accessing **the same schema** are serialized, while writers
accessing distinct schemas in the same PostgreSQL database can proceed
independently. The backend checks that the configured schema still matches
`current_schema()` when acquiring the lock; a changed or missing schema fails
closed. The lock releases on transaction commit or rollback, including failed
writes. Schema OIDs are exact database identities, not truncated text hashes;
the PostgreSQL advisory-lock key space is database-local. The live backend
contract test holds simultaneous transactions against two schemas and verifies
same-schema contention and rollback release.

Both backends initialize one current schema, without a historical SQL migration
framework. Schema changes during pre-production development require refreshing
*disposable* databases, not rewriting valuable data.

## Driver-independent integrity

Canonical serialized Model bytes, packed hydraulic reference keys and SHA-256
hash input use fixed byte encodings. Do not substitute a SQL driver's UUID
representation or rely on physical database ordering for hashing. Historical
revision-chain and audit-hash format identifiers must remain byte-for-byte
unchanged.

## Testing and status

The backend-contract test covers shared API behavior. With PostgreSQL enabled,
it also tests a real disposable PostgreSQL database, including UUID
round-trips, project/branch/revision operations, object history and hydraulic
network loading. The regular Linux test runner can provision that database
through Docker Compose. See [Testing](testing.md).

Cross-backend functionality is exercised, but production operational recovery,
large-network performance and publicly exposed HTTP deployments are not yet
qualified.
