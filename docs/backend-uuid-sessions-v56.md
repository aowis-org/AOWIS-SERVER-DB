# V56 — session and password UUID binding

`src/sessions.cpp` and `src/password_credentials.cpp` now bind SQL UUIDs
through `StorageBackend::bindUuid()` and read them through
`StorageBackend::readUuid()`, like the identity, history and security modules.
The shared active-session quota check receives the backend explicitly.

This includes session issuance, lookup, pagination and revocation; credential
changes, password login, account lockouts, recovery-token issuance/redemption,
and audit actor references. The first page's all-zero UUID cursor remains an
actual SQLite UUID BLOB, not SQL NULL.

**Not changed:** the cryptographic session-token digest, SQLite session-token
redaction marker, authentication behavior, credential KDF, SQL statements,
SQLite schema, security audit hash encoding, and finalized project hashes.
The session-redaction SQL and trigger are intentionally still SQLite-specific;
the future PostgreSQL adapter will need an equivalent operation.

Existing tests now also assert the stored UUID BLOBs for session issuer/user
and credential ownership, plus UUID round-trips through authenticated session
lookup and password login. This is a portability step, not a PostgreSQL backend.
