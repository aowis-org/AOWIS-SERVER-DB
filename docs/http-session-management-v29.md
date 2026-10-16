# V29 — Manage own active sessions

A session holder can inspect and revoke their own active bearer sessions,
without `security.manage` and without receiving anyone else's sessions.
Neither bearer tokens nor their hashes appear in API responses or audit records.

| Method | Endpoint | Result |
| --- | --- | --- |
| `GET` | `/api/v1/sessions?limit=100&after_uuid=<uuid>` | `{ "sessions": [...] }` |
| `DELETE` | `/api/v1/sessions/<session-uuid>` | `{ "revoked": true, "session_uuid": "..." }` |

All requests require the existing `Authorization: Bearer <token>` header.
`limit` defaults to 100 and must be 1–100. `after_uuid` is an *exclusive*
keyset cursor; IDs are ordered by the 16-byte stored UUID value. The list
includes only sessions that are not revoked and have not expired. Fields are
`session_uuid`, `created_at_ms`, `expires_at_ms` (timestamps as decimal strings)
and `current` (boolean). The current session is marked in whichever page
contains it. To list every page, continue with the final UUID on each page
until the returned array is empty.

Deleting a different user's session or a revoked/expired session produces 404
without disclosing its owner. Revoking the current session logs out the caller.
The database verifies ownership again under `BEGIN IMMEDIATE`. The revocation
and audit event (`session.revoke.own` for another session, `session.logout` for
the current one) are atomic. On audit insertion failure, no session is revoked.

Unlike administrator revocation, these routes cannot touch other users and
cannot access the credentials or recovery-token tables. This change requires
no SQLite migration and does not modify project revision hashes.
