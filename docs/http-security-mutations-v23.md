# V23: Audited REST security administration mutations

This extends the V22 read-only security catalog with **authenticated,
security.manage-only** mutation endpoints. The application-defined
`HttpRoutes::Authenticate` callback resolves bearer credentials to trusted
operation contexts. The client cannot choose its actor or bypass the existing
SQLite security and audit logic.

The new endpoints require `security.manage` **at system scope** (not project
ownership). They call the existing audited C++ database methods and do not
implement SQL or access-control rules of their own.

| Method | `/api/v1/` path | Exact JSON body | Result |
| --- | --- | --- | --- |
| POST | `admin/users` | `{"username":"..."}` | User with UUID |
| POST | `admin/groups` | `{"name":"..."}` | Group with UUID |
| POST | `admin/roles` | `{"key":"..."}` | Role with UUID |
| POST | `admin/permissions` | `{"key":"..."}` | Permission with UUID |
| POST | `admin/groups/{group_uuid}/members` | `{"user_uuid":"..."}` | Membership UUID |
| DELETE | `admin/memberships/{membership_uuid}` | Empty | Revoked UUID |
| POST | `admin/roles/{role_uuid}/permissions` | `{"permission_uuid":"..."}` | Grant UUID |
| DELETE | `admin/permission-grants/{grant_uuid}` | Empty | Revoked UUID |
| POST | `admin/principals/{user\|group}/{principal_uuid}/assignments` | `{"role_uuid":"...","scope_kind":"system\|project\|branch","scope_uuid":null\|"..."}` | Assignment UUID |
| DELETE | `admin/role-assignments/{assignment_uuid}` | Empty | Revoked UUID |
| POST | `admin/users/{user_uuid}/disable` | Empty | User UUID |
| POST | `admin/users/{user_uuid}/enable` | Empty | User UUID |

`scope_uuid` must be JSON `null` only for `scope_kind=system`, and a canonical
UUID string for project or branch scopes. Every new role assignment/grant or
group membership receives a fresh UUIDv7. Revocations never physically delete
grant records; previously revoked records remain visible using the V22 catalog
`include_inactive=true` filter.

Creation returns HTTP 201. Revocation and user-state changes return 200.
Missing authentication returns 401 and insufficient privilege returns 403.
Malformed or extra request fields, bad UUIDs, unexpected query parameters, and
nonempty revocation bodies return 400. Conflicting/invalid mutations, including
duplicate active grants and attempts to remove the last administrator, return
409. Backend diagnostic details are deliberately excluded from HTTP responses.

No schema migration, revision hash, or audit format change is needed. The audit
journal records the authenticated actor and the affected entity in the same
transaction as each successful mutation. Failed operations must not add audit
entries. Session issuance, bootstrap, and credential provisioning remain **local
administrator operations**, not HTTP endpoints. The HTTP server remains
loopback-only until a separate production transport/security review.

## Test

With Qt HttpServer enabled, run `./compile_linux_tests.sh` and check
`aowis-db-http-security-mutations`. Existing tests remain unchanged.
