# REST/HTTP interface

`aowis-db-http` is the optional Qt HttpServer adapter around the C++ database
core. `aowis-server-db` starts it on `127.0.0.1`, on the configured HTTP port
(default 8080). It does not implement TLS and should not be published directly
to the internet. All nonpublic operations resolve bearer credentials into an
authorized database operation context.

## Routes

The implementation in `src/http_routes.cpp` is authoritative for request and
response schemas. The route groups are:

| Routes | Purpose |
| --- | --- |
| `/api/v1/health` | Local health status, no login required |
| `/api/v1/session/login`, `/api/v1/session/recover` | Session creation and password recovery |
| `/api/v1/session/current`, `/api/v1/session/password`, `/api/v1/session/revoke-others` | Authenticated self-service |
| `/api/v1/sessions`, `/api/v1/sessions/{uuid}` | Session listing and revocation |
| `/api/v1/projects`, `/api/v1/projects/{uuid}` | Project listing, creation and lookup |
| `/api/v1/projects/{uuid}/branches`, `/api/v1/branches/{uuid}/revisions` | Branch and revision history |
| `/api/v1/revisions/{uuid}` | Revision details |
| `/api/v1/revisions/{uuid}/objects/{uuid}`, `/api/v1/revisions/{uuid}/networks/{uuid}` | Object and typed-network reads |
| `/api/v1/branches/{uuid}/networks/{uuid}` | Save a typed hydraulic network |
| `/api/v1/branches/{uuid}/object-groups`, `/api/v1/revisions/{uuid}/object-groups` | Grouped changes and history |
| `/api/v1/merge/compare`, `/api/v1/merge/preview` | Compare branches and preview merge conflicts |
| `/api/v1/branches/{uuid}/merge`, `/api/v1/branches/{uuid}/finalize` | Merge and finalization |
| `/api/v1/admin/...` | Authorized user, group, role and permission administration |

### Create a branch

`POST /api/v1/projects/{project_uuid}/branches` creates a new working branch
from an existing **finalized** revision in the same project. The request must
contain exactly these two JSON fields:

```json
{
  "name": "alternative-design",
  "base_finalized_revision_uuid": "019a4d09-8362-7bd5-9bc9-1df7c88118e4"
}
```

On success the server returns `201 Created` and the usual branch object:
`uuid`, `project_uuid`, `name`, `head_revision_uuid`, `created_by_uuid`.
Authentication is required (`401` if missing), and the actor needs
`project.branch.create` for the project (`403` otherwise) and `project.read`
for the base revision's branch. An invalid request returns `400`; duplicate
branch names, nonfinalized bases, cross-project bases and inaccessible bases
are rejected with `409`. No client-supplied creator identity is accepted.

The table is a navigation aid, not an independently versioned API schema;
consult `src/http_routes.cpp` for exact HTTP methods, dynamic route patterns,
request fields and error responses.

## Security and limitations

Routes enforce the same core authorization and optimistic conflict checks as
embedded callers. HTTP hardening, input bounds and pre-password-check login
throttling are present, but the executable remains a loopback-only development
and trusted-deployment server. Production use needs separately verified TLS
termination, resource limits, request isolation, monitoring and backup/recovery.

The `/api/v1/` prefix is a real HTTP protocol namespace; it is not a development
patch identifier and must not be renamed casually.
