# V20 — Persistent security-administration audit

SQLite schema version **9** introduces `security_audit_events`. This is an
independent, append-only system-level audit log, separate from engineering
revision histories, object groups, and finalization hashes.

Each recorded administrative mutation and its audit event commit in one
`BEGIN IMMEDIATE` transaction. The core records:

- Administrator bootstrap (attributed to the bootstrapped user).
- User, group, role, and custom permission creation.
- Adding group members, granting permissions to roles, and assigning roles.
- Session issuance, session revocation, and user disabling.

Every record has a UUIDv7, monotonic sequence, actor UUID, subject UUID,
operation key, UTC millisecond timestamp, previous SHA-256 and chain SHA-256.
The hash uses a frozen V1 canonical byte encoding with domain separation.
SQLite triggers reject updates and deletes to the journal. The administrator
may still bypass these controls directly through SQLite, as agreed for AOWIS.

### Reading and verifying

The `security.audit.read` permission at **system** scope is required. Library
APIs:

- `securityAudit(actor, after_sequence, limit, error)` (1–1000 records/page)
- `verifySecurityAudit(actor)` (validates the entire ordered chain)

Local operator examples:

```bash
./build-linux/aowis-db-admin audit DB_FILE ADMIN_UUID
./build-linux/aowis-db-admin audit DB_FILE ADMIN_UUID 100 50
./build-linux/aowis-db-admin audit-verify DB_FILE ADMIN_UUID
```

There are no public HTTP security-administration or audit endpoints in this
version. Do not send credentials or bearer tokens in audit metadata. The audit
journal stores session UUIDs, **never plaintext session tokens or token hashes**.

### Compatibility and scope

During V8→V9 migration, the built-in `security.audit.read` permission is
installed using its pinned UUID and granted to the existing system
administrator role. This must happen even when the V8 permission catalog was
created before the audit feature existed.

Databases from schemas V1–V8 migrate to V9, without rewriting project model
snapshots, existing revision hashes, or historical model operations. On
upgrading older databases, the journal starts empty: actions from before the
migration were not audited and must **not** be backfilled with invented data.

This is the foundation of security auditing, **not a comprehensive security
log**. Denied login attempts, network requests, future administrative changes,
identity-provider integrations, and tamper-evident external backups are not yet
captured here. Historical audit logs are not included in project hashes.
