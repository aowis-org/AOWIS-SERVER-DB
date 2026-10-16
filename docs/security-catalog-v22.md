# V22: Security administration catalog

AOWIS-SERVER-DB now exposes an administrative **read-only** catalog for the
identities and grants already supported by the SQLite V10 security schema.
No migration, project revision hash change, or security audit format change is
necessary.

## C++ API

`Database` offers `securityUsers`, `securityGroups`, `securityRoles`,
`securityPermissions`, `securityGroupMembers`, `securityRolePermissions`, and
`securityRoleAssignments`. All methods require `security.manage` at system scope.
A project owner without that global permission cannot inspect the catalog.

`SecurityListOptions` supports stable UUID keyset pagination:

- `after_uuid`: exclusive lower-bound UUID; an unset/null UUID starts at the beginning.
- `limit`: 1–200 entries, default 100.
- `include_inactive`: include disabled users/groups or revoked relationships,
  default false. Roles and permissions don't currently have inactive states.

Results are always sorted by their record UUID. For the next page, supply the
last item's UUID. An empty page is not an error; use the `error` out parameter
to distinguish it from permission/SQL errors.

The membership/grant APIs deliberately retain original grant UUIDs and expose
revoked records only when requested. A regrant receives a fresh UUID, so an
administrative interface can distinguish grants from their replacements.

## REST API

All endpoints are authenticated `GET` routes under `/api/v1/admin/`:

| Path | JSON response array |
| --- | --- |
| `users` | `users` |
| `groups` | `groups` |
| `roles` | `roles` |
| `permissions` | `permissions` |
| `groups/{uuid}/members` | `memberships` |
| `roles/{uuid}/permissions` | `grants` |
| `principals/{user\|group}/{uuid}/assignments` | `assignments` |

Optional query parameters are `limit`, `after_uuid`, and `include_inactive`
(`true` / `false`). Duplicate or unexpected query parameters are rejected.
Identifiers are canonical UUID strings. 64-bit timestamps are returned as
strings to preserve exact values in JavaScript clients. Bearer tokens, session
digests and password/credential material are **never** returned by these routes.

Authentication failure returns HTTP 401, authenticated lack of `security.manage`
returns HTTP 403, and an invalid UUID/query returns HTTP 400. SQLite failures
are reported as generic HTTP 500 errors without exposing database details.

**Not included:** credential management, remote create/revoke administration,
full CRUD user interfaces, or an internet-facing deployment profile. Existing
C++ mutation operations and the trusted local administration CLI remain the
ways to administer privileges until HTTP mutation routes are designed and
secured separately.

## Validation

Tests: `aowis-db-security-catalog` and (with Qt HttpServer)
`aowis-db-http-security-catalog`. They cover listing, pagination, authorization,
revocation and regrant history, disabled accounts, persistence across reopen,
and audit-log nonmutation for reads.
