# V61 — Complete SQL UUID boundary review

The central project-creation path is now backend-neutral when looking up and
assigning the built-in `project.owner` role. Its role UUID is decoded through
`StorageBackend::readUuid()` and rebound through `StorageBackend::bindUuid()`;
it is no longer treated as a SQLite BLOB in domain code. A missing or invalid
role identifier fails the project-creation transaction.

The security-catalog regression test now verifies the exact role UUIDs of the
system administrator and automatically assigned project owner, in addition to
their actor identities and scopes.

After reviewing the production SQL-facing UUID reads/bindings, the other direct
`QUuid::toRfc4122()` and `QUuid::fromRfc4122()` uses are for deterministic
SHA-256 encodings, immutable manifests, object comparison keys, or packed
hydraulic-reference index keys. They intentionally remain independent of SQL
storage representation.

This is the end of the mechanical SQLite UUID portability sweep. PostgreSQL
still needs a separate backend adapter and schema, and its semantics need
actual integration tests. This patch does not introduce PostgreSQL support.

No SQL statement, SQLite schema, migration, public API, canonical serialization,
or historical hash encoding is changed.
