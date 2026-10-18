# V55 — security identity and catalog UUID values

The internal SQLite `StorageBackend` already owns SQL UUID binding/reading.
The security-management and catalog operations now use that same contract:

- Built-in role/permission seeding and administrator bootstrap;
- User, group, role, permission and scoped role assignment writes;
- Authorization principal and project/branch scope checks;
- Revoke/enable operations and catalog result decoding;
- Security-list UUID cursor pagination and grant filters.

Only SQL parameter and result conversions have changed. Scope references that
are absent are passed as SQL NULL. The all-zero UUID used for the initial
security catalog pagination cursor remains a real 16-byte UUID BLOB, **not**
SQL NULL. This distinction matters for both SQLite and PostgreSQL.

No schema changes, SQL migrations, API changes, hash-format changes, or new
backend abstraction have been introduced. In particular, cryptographic UUID
serialization in finalized-revision and audit-chain hashing is unchanged.

SQLite security tests now additionally cover scoped UUID lookup, bootstrap's
system-scoped assignment, related membership/grant identities and authorization
on branch versus system scopes. This patch does not implement PostgreSQL;
it only removes SQLite-specific UUID encoding from these modules.
