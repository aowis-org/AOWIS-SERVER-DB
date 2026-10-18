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

## PostgreSQL

`src/postgres_backend.cpp` and `src/postgres_schema.cpp` implement the QPSQL
backend. PostgreSQL stores UUIDs natively; `StorageBackend::bindUuid()` and
`readUuid()` handle driver-specific conversion. Importantly, an **all-zero UUID
is a UUID value, not SQL NULL**. PostgreSQL uses libpq connection settings
and an explicitly selected schema, which must exist before connecting.

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
