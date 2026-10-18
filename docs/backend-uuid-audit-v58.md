# V58 — Security audit SQL UUID portability

`src/security_audit.cpp` uses `StorageBackend::bindUuid()` for audit event,
actor, and subject UUID query parameters and `readUuid()` when decoding audit
rows for listing or full-chain verification. SQLite still stores all three
identifiers as 16-byte BLOBs.

The **security-audit V1 SHA-256 format is unchanged**: `eventHash()` explicitly
encodes each identifier as 16 RFC-4122 bytes, independent of the SQL driver.
This is a critical distinction for a future PostgreSQL UUID implementation.

The existing audit test verifies actor and subject relationships, pagination,
reopen round-trips, raw SQLite BLOB values, append-only enforcement,
and detection of tampered event data. No database schema, migrations, public
APIs, or historical hash encoding were changed.
