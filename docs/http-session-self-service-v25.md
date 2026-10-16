# V25 — Authenticated session self-service

The REST server now offers two endpoints for the **current bearer session**.
Both require exactly the same session authentication as the other REST routes.
No administrator permission or actor UUID in the payload is needed.

| Method | Path | Successful response |
| --- | --- | --- |
| `GET` | `/api/v1/session/current` | `200` with session UUID, user UUID, and expiry |
| `DELETE` | `/api/v1/session/current` | `200` with `{"revoked":true}` |

Both endpoints require `Authorization: Bearer <64 lowercase hex characters>`.
`expires_at_ms` is a **decimal string** of Unix milliseconds to avoid
JavaScript's 53-bit JSON integer precision issue. Bearer tokens and token
hashes are never returned by the GET endpoint.

`GET` and `DELETE` reject request bodies and query parameters with `400`.
Missing, invalid, expired, disabled-user, or revoked sessions receive `401`.
Requests cannot choose a different actor or session ID by supplying headers,
query arguments, or JSON fields. No session issuance or password login is
introduced over HTTP.

`DELETE` permanently revokes **only** the bearer token used for that request.
It does not terminate the user's other sessions. Revocation and its
`session.logout` security audit event are committed in one SQLite transaction.
Reusing the revoked token returns `401`. Other sessions remain valid. This
operation is not an administrator permission; all valid session holders can
log out. Administrators retain their separate, audited `revokeSession` API.

This is **not** a credential enrollment or refresh-token mechanism. Tokens
must still be provisioned locally. Continue using a loopback-only server
behind a properly configured TLS-terminating reverse proxy for remote access.
