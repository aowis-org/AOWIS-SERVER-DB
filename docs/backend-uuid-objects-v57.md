# V57 — object persistence and manifest UUID portability

`src/objects.cpp` uses `StorageBackend::bindUuid()` for object-mode queries,
object copying, object CRUD and journal inserts, and `readUuid()` for object
and event identities returned from SQL. The object-row decoder accepts the
backend explicitly; no second SQLite UUID helper is kept in this module.

The **persisted manifest format is unchanged**: object-event, object, group,
and group-member UUIDs are explicitly serialized as 16 RFC-4122 bytes *after*
reading the backend's SQL UUID type. A SQL NULL target-group UUID still hashes
as sixteen zero bytes. The group-member lookup binds its decoded UUID through
the backend, rather than relying on the SQL result's raw byte representation.
This allows a future native PostgreSQL UUID result without accidentally
changing finalized model-history digests.

SQLite still stores each UUID as a 16-byte BLOB. No schema changes, migrations,
API changes, or changes to the existing hash/serialization algorithms were made.

The object and grouped-history tests additionally check returned object and
actor UUIDs, the SQLite BLOB values persisted for object events, and that the
finalization's snapshot SHA-256 matches the exact pre-finalization manifest.
All existing merge, undo/redo, and immutable-history tests remain registered.
