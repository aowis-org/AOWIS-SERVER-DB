# V32 — Revoke all other user sessions

An authenticated user can invalidate every other unexpired, unrevoked bearer
session belonging to the same account without needing administrator rights.
The presented session remains active.

`POST /api/v1/session/revoke-others`

- Requires `Authorization: Bearer <token>` and an empty request body and query.
- Does not accept a user UUID or any other session identifier.
- Responds with `current_session_uuid` and `revoked_count` (decimal string).
- A repeat request with no other sessions succeeds and reports `"0"`.
- Invalid tokens return HTTP 401; unexpected request bodies or query fields
  return 400.
- SQLite revokes qualifying sessions and writes one `session.revoke_others`
  security-audit event in the **same transaction**. The event's actor is the
  bearer holder and its subject is the kept session's UUID.
- When the audit insert fails, the entire revocation is rolled back.
- Session records and audit events are retained for documentation.

This complements individual self-service revocation and the current-session
logout endpoint. No new tables, schema migrations, password derivations, or
revision-hash changes are necessary.
