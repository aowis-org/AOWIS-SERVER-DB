# V27 — Authenticated password change

An authenticated user can replace their own local password **without**
`security.manage`. The caller must present **both** their current bearer
session and the correct current password.

| Method | Endpoint | Purpose |
| --- | --- | --- |
| `POST` | `/api/v1/session/password` | Change the current user's local password |

Example body (exactly these two string fields):

```json
{"current_password":"existing password","new_password":"a new long password"}
```

Use `Authorization: Bearer <64-character-token>` as for the existing session
endpoints. No user UUID or session UUID can be selected by the caller.

- `200`: `{"changed":true,"sessions_revoked":true}` — **all** sessions for
  this user (including the caller's session and sessions on other devices)
  have been permanently revoked. Log in again with the new password.
- `400`: bad JSON, unexpected fields, query parameters, unchanged password,
  or an invalid new password. The same 12–1024-byte password rule applies.
- `401`: missing, revoked, expired, or disabled-user bearer session.
- `403`: wrong current password or active password lockout.
- `500`: database, hashing, or audit write failure; no partial change is saved.

Verification uses the persisted salt and work factor. The expensive derivation
runs before opening a SQLite write transaction; the session and exact
credential bytes are rechecked under that transaction. Failed old-password
attempts share the password-login failure counter and 60-second lockout.

On success, the database atomically saves the new salted verifier, revokes all
existing user sessions, and appends a `credential.change` event to the SHA-256
security audit journal. Neither password nor bearer token is logged or added
to audit data. A failing audit insert rolls back both the password change and
session revocations.

This endpoint does **not** implement forgotten-password recovery. Users who
cannot supply their existing password still need an administrator to reset it
through the existing local-only workflow. Secure recovery requires a separate
verified channel and is intentionally deferred.

As before, the HTTP service must remain on loopback behind an appropriately
configured TLS-terminating reverse proxy for remote use. Protect password
request bodies and Authorization headers from proxy and application logs.
