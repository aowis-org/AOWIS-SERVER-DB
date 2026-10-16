# V28 — Administrator-authorized one-time password recovery

Password recovery is a **trusted-operator** operation, not an automated email
flow. AOWIS does not currently verify email address ownership, phone numbers,
or other recovery factors.

## Issue a recovery capability

A system administrator who has independently verified the user's identity can
run, on the machine hosting the SQLite database:

```bash
umask 077
./build-linux/aowis-db-admin recover-password /path/to/aowis.sqlite ADMIN_UUID USER_UUID 900
```

The token is written to stdout **once**; redirect it to a secured file or
provide it out of band to the intended person. The session-like recovery
capability is not an ordinary bearer session. Issuing a second recovery token
invalidates previously outstanding tokens for the same user. Validity is
limited to 1–3600 seconds (default 900 seconds). Do not put the token into
URLs, logs, source files, or command-line arguments.

## Redeem over the HTTP API

```
POST /api/v1/session/recover
Content-Type: application/json

{"recovery_token":"<64-lowercase-hex-characters>","new_password":"<new passphrase>"}
```

Successful redemption returns a `changed` flag and a `sessions_revoked` flag.
The user may then authenticate with `/api/v1/session/login` using the new
password. The recovery token cannot be reused. Both expired and otherwise
invalid tokens produce the same 401 response. A malformed request or new
password returns 400. Only HTTPS via the existing trusted TLS reverse proxy
should be used for delivery; the application listener remains loopback-only.

The password is hashed with the existing PBKDF2-HMAC-SHA-256 credential scheme.
The SQLite database contains **only a domain-separated SHA-256 digest** of the
32-byte randomly generated recovery token. Each token has its own UUIDv7.
Recovery invalidates all user sessions. Issuance and redemption appear in the
security audit chain without logging the secret. The audit event for redemption
attributes the authority to the administrator who issued the capability; it
cannot establish the identity of the person who physically presented it.

The reset transaction atomically closes the token, updates the password,
revokes sessions, and appends the audit entry; failure rolls back all changes.
Issuing a new token, changing the password through either existing path, or
disabling the user invalidates outstanding recovery tokens. Re-enabling a user
does not resurrect them.

## Deliberate limitations

- No self-service account recovery **enrollment** or verified delivery.
- No email/SMTP integration, OIDC recovery flow, or 2FA.
- Trust in the SQLite file administrator remains outside the application threat
  model.
- The anonymous redemption route is subject to the existing loopback/reverse
  proxy hardening rules and should not be published without TLS and controls.
