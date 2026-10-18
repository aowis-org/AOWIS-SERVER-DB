# V53: Backend-owned SQL UUID values

## Motivation

PostgreSQL's native UUID parameters and results do not share the SQLite
schema's 16-byte BLOB SQL representation. Binding `QUuid::toRfc4122()` in
domain queries makes it possible to run only against the SQLite schema.
A future PostgreSQL adapter must be able to use native UUID parameters and
read native UUID results without changing the domain model or hashes.

## Internal storage contract

`StorageBackend::bindUuid()` produces a backend-appropriate QVariant for a
non-nullable UUID SQL parameter. `readUuid()` reconstructs a QUuid from a
backend-specific SQL query value. Nullable columns may contain SQL NULL;
`readUuid()` maps NULL to a null QUuid, as the existing project/revision
legacy attribution fields do.

The SQLite implementation returns precisely 16 RFC-4122 bytes as a QByteArray.
This includes a zero UUID, which is **not** SQL NULL. The decoder rejects
values that are not 16 bytes. PostgreSQL is not implemented and this contract
does not change the SQLite schema version (13).

V53 migrates UUID input/output in the central project, branch and revision
operations in `database.cpp`: 53 query binds and 23 result decoding sites.
The other domain modules still contain SQLite-specific BLOB bindings and
are **not yet portable**. Those should be migrated methodically in subsequent
patches before implementing the PostgreSQL adapter.

## Immutable hash serialization remains separate

`chainDigest()` continues to use `QUuid::toRfc4122()` directly. It must never
use `StorageBackend::bindUuid()`; canonical hashing is independent of the
SQL driver's parameter/result representation. The same applies to canonical
Model data and audit history hashes.

## Verification

The SQLite backend contract test checks UUID round-trips through actual SQL
BLOB parameters/results, the 16-byte representation, nullable fields,
malformed byte lengths and the distinction between SQL NULL and the zero UUID.
Existing project/branch/revision and hash-history tests exercise the migrated
call sites. No data, migration, or existing hash format is modified.
