> **Historical V17 documentation.** V19 replaces the static-token development server
> with persistent bearer sessions. See [http-sessions-v19.md](http-sessions-v19.md).

# V17: optional HTTP REST foundation

`aowis-db-core` is unchanged. If Qt6 HttpServer and Network are available,
CMake also builds `aowis-db-http` and `aowis-db-server`. Disable them with
`-DAOWIS_DB_BUILD_HTTP=OFF` when packaging an embedded-only application.

## Authentication boundary

The `HttpRoutes` C++ adapter **requires** a host-provided `Authenticate`
callback. Every non-health route calls it before using the existing database
core authorization checks. There is no route to bootstrap an administrator,
create identity data, or impersonate an arbitrary UUID.

The standalone `aowis-db-server` executable is deliberately **development-only**:

- Binds exclusively to IPv4 loopback (127.0.0.1).
- Requires the environment variable `AOWIS_DB_HTTP_TOKEN` (minimum 32 bytes),
  which is checked against `Authorization: Bearer <token>`.
- Requires an existing actor UUID as an explicit command-line argument; it
  cannot register users or grant permissions.
- Does not provide TLS, token issuance/revocation, password authentication,
  refresh tokens, or network rate limits. Do not expose it to the internet or
  treat the static token as production authentication.

Example, **only after a trusted local application has created the user**:

```bash
export AOWIS_DB_HTTP_TOKEN='a-local-random-token-of-at-least-32-bytes'
./build-linux/aowis-db-server /path/to/aowis.sqlite EXISTING_USER_UUID 8080
curl -H "Authorization: Bearer $AOWIS_DB_HTTP_TOKEN" \
     http://127.0.0.1:8080/api/v1/projects
```

## Routes currently available

| Method | Path | Notes |
|---|---|---|
| GET | `/api/v1/health` | Public, no DB details |
| GET | `/api/v1/projects` | Filtered using core permissions |
| POST | `/api/v1/projects` | `{ "name": "..." }`, core authorization |
| GET | `/api/v1/projects/{uuid}` | Authorized single project |
| GET | `/api/v1/projects/{uuid}/branches` | Authorized branch list |
| GET | `/api/v1/branches/{uuid}/revisions` | Authorized revision list |
| GET | `/api/v1/revisions/{uuid}` | Authorized revision metadata |
| GET | `/api/v1/revisions/{revision_uuid}/objects/{object_uuid}` | Authorized object data (base64 payload and version) |
| POST | `/api/v1/branches/{uuid}/finalize` | `{ "title": "...", "message": "...", "expected_generation": "0" }` |

Revision numbers and edit generations are decimal **strings** in JSON to
retain 64-bit integer precision in JavaScript clients. Snapshot and chain
hashes are lowercase hexadecimal SHA-256 values. IDs are canonical UUID text
without braces.

The REST interface is deliberately incomplete. Object CRUD, typed model
serialization over HTTP, attachment transport, production authentication,
per-user sessions, PostgreSQL, and public hosting remain future work.

`aowis-db-http` is an optional dependency, not required by the DB core; a GUI
that embeds SQLite should link only `aowis-db-core`.
