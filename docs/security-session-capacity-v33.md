# V33 — Bounded active bearer sessions

An account may have at most **32 simultaneously active sessions**. Active
means `revoked_at_ms IS NULL` and `expires_at_ms > now`. Historical revoked
and expired session rows remain in the SQLite audit trail but do not consume
quota slots.

The limit is enforced by the DB core on **both** issuance paths:

- Administrator `Database::issueSession` (trusted local provisioner)
- Authenticated `Database::loginWithPassword` (HTTP password login)

The count and the token insertion happen under the same `BEGIN IMMEDIATE`
write transaction so independent SQLite connections cannot over-issue. A
rejected issuance rolls back and writes no `session.issue` or
`session.password_login` audit event.

Administrators receive an explicit quota error when issuing a session.
Password login returns the existing generic login error, so unauthenticated
callers cannot determine the account's active-session count.

Users with a valid session can use the existing session-management endpoints
to revoke others and free slots. Administrators can revoke sessions as well.
A full quota does not automatically revoke any existing session. The limit
requires no schema migration and does not rewrite historical revision or
audit hashes.

This controls the number of *active* sessions, not total historical records.
Long-term retention/pruning of expired historical session records is a
separate operational question because the audit trail is append-only.
