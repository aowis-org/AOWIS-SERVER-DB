# V19 — Persistent HTTP bearer sessions (SQLite schema V8)

`aowis-db-server` no longer accepts a single static environment token or an
actor UUID on its command line. Every request other than `/api/v1/health` must
provide `Authorization: Bearer TOKEN`. The server resolves it to an actor
using `Database::authenticateSession`, after which all existing authorization
checks are applied unchanged.

## Security properties

- 32 random bytes generated with `QRandomGenerator::system()`; bearer token is
  represented as **64 lowercase hexadecimal characters**.
- Only a SHA-256 digest with a dedicated domain separator is persisted; the
  plaintext bearer token is returned only once at issuance.
- Sessions have UUIDv7 identifiers, an identified user, issuer, creation time,
  expiry, and optional revocation timestamp.
- Expired, revoked, malformed, and disabled-user tokens are refused. Disabling
  a user immediately blocks all of their existing tokens.
- Only an actor granted `security.manage` at system scope can issue, revoke, or
  disable accounts through the C++ database core.
- Session metadata is never included in finalized project snapshots. Schema V8
  is backward compatible with V1–V7 finalized revision hashes.
- HTTP continues binding **only 127.0.0.1**, with no TLS termination, rate
  limiting, password authentication, OIDC, or user-login endpoint. **This is
  not yet a production internet-facing authentication service.**

## Trusted local provisioning

The CLI is a **trusted local file-operator tool**. It directly opens the SQLite
file; file access and machine ownership are its trust boundary. Its
`ADMIN_UUID` argument selects the existing administrator **inside that trusted
local environment** and is not an authentication credential. Do not make this
CLI remotely callable. An ordinary HTTP client cannot invoke these commands.

After an administrator has been bootstrapped (once):

```bash
# Bootstrap on a new, locally owned database; prints administrator UUID.
./build-linux/aowis-db-admin bootstrap /path/to/aowis.sqlite admin

# Issue a 1-hour token for that user (prints token to stdout; session UUID to stderr).
umask 077
./build-linux/aowis-db-admin issue /path/to/aowis.sqlite ADMIN_UUID ADMIN_UUID 3600 > admin.token

# Start the local server. Session state survives server restarts.
./build-linux/aowis-db-server /path/to/aowis.sqlite 8080

# Use the token without putting it into the shell history.
curl -H "Authorization: Bearer $(cat admin.token)" \
     http://127.0.0.1:8080/api/v1/projects

# Revoke an individual session, using its UUID from the CLI stderr output.
./build-linux/aowis-db-admin revoke /path/to/aowis.sqlite ADMIN_UUID SESSION_UUID

# Disable an account and automatically reject all its sessions.
./build-linux/aowis-db-admin disable-user /path/to/aowis.sqlite ADMIN_UUID USER_UUID
```

The maximum token lifetime is 30 days. The application does not currently
support password-based login, OAuth, token refresh, user-driven logout,
revocation audit streams, or multi-process distributed caching. Those need
separate design and testing before internet deployment.
