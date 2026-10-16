# V26 — Local Password Credentials and Login (SQLite V11)

This adds password authentication to the existing *loopback-only* HTTP service.
An administrator must enroll a user first. Account creation alone does not
establish login credentials. All authorization still runs in the DB core.

## Password credentials

- Stored per user UUID (not by username). SQLite stores a **random 16-byte
  salt**, PBKDF2-HMAC-SHA-256 verifier (32 bytes), 600,000 iterations,
  updater UUID, and time. Plaintext passwords are never stored.
- The iteration count is stored per credential to allow later rehashing and a
  future Argon2id upgrade. Requires OpenSSL libcrypto on the build platform.
- 12–1024 UTF-8 bytes without NUL are accepted for new passwords. Users with
  no credentials cannot log in.
- Five failed attempts temporarily lock that user for 60 seconds. Resetting
  the credential clears the lockout. This also permits targeted lockout abuse;
  a reverse proxy must provide additional IP/client rate limits.
- Issuing a login session is audited (`session.password_login`), and resetting
  a credential is audited (`credential.set`). Wrong passwords do not append to
  the chained audit log; only the per-user failure counter changes.
- A credential reset **revokes all existing bearer sessions** for that user in
  the same transaction as the credential change and its security audit event.
- Passwords are verified before entering the SQLite write transaction.
  Before session issuance, the credential, active-user state, and lockout are
  rechecked under the transaction to avoid a concurrent reset race.

## Local enrollment

Bootstrap, then set a password for the administrator, or for any user already
created through the existing admin APIs:

```bash
./build-linux/aowis-db-admin bootstrap /path/to/aowis.sqlite admin

# The command reads only the first line of stdin. It never takes the password
# from argv. Enter it without echo or shell-history disclosure:
read -r -s -p 'New password: ' AOWIS_PASSWORD; echo
printf '%s\n' "$AOWIS_PASSWORD" | ./build-linux/aowis-db-admin set-password \
  /path/to/aowis.sqlite ADMIN_UUID USER_UUID
unset AOWIS_PASSWORD

./build-linux/aowis-db-server /path/to/aowis.sqlite 8080
```

For automation, provide password bytes via a protected pipe or file descriptor.
Avoid saving passwords in command-line arguments, logs, code, or version control.
The administrator CLI is a **trusted local file-operator tool** and is not a
remote authentication mechanism.

## HTTP login

`POST /api/v1/session/login` accepts exactly:

```json
{"username":"admin","password":"<password>"}
```

On success, returns `{session_uuid,user_uuid,expires_at_ms,bearer_token}`.
The bearer token is returned only at login; its SHA-256 digest is stored in
SQLite. Use `Authorization: Bearer <bearer_token>` on future requests and
`DELETE /api/v1/session/current` to log out.

Malformed JSON, unexpected fields, or invalid types return HTTP 400. Missing
or incorrect credentials and locked users return HTTP 401 without disclosing
whether a username exists. No public endpoint enrolls users. Authenticated users can change their
own password (with the existing password) using the V27 self-service endpoint. Administrative password reset requires system-level
`security.manage` permission in the trusted library/CLI.

## Security boundary and limitations

The Qt server remains bound to 127.0.0.1; it is **not** safe to expose directly
on a public interface. Passwords and bearer tokens travel in HTTP requests:
use a hardened TLS reverse proxy with strict rate limits and no credential-
bearing request logging before enabling remote clients. The app itself does
not terminate TLS. Password derivation currently runs synchronously in the
Qt HTTP thread: isolate or offload that work before higher traffic deployment.

PBKDF2 is a broadly supported baseline, not memory-hard like Argon2id.
Multi-factor authentication, OIDC, forgotten-password recovery
flows, credential rotation policies, and automated account enrollment remain
future work. There is no PostgreSQL backend yet. The V11 schema migration does
not alter finalized project data or revision hash formats.
