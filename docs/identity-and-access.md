# Identity and authorization — SQLite foundation (schema v2)

## Security boundary

Every public project, branch, revision and snapshot method requires a
`OperationContext`. Callers cannot use the old actor-free API. Authorization is
performed in the DB core, not in HTTP; mutations are checked again after the
SQLite write transaction starts, so concurrent permission changes cannot race
with a project mutation. An unknown, invalid, or disabled actor gets no rights.

**Important:** `OperationContext` contains an identity, not an authentication
credential. Only the trusted embedding application may construct one. A future
HTTP server must derive it from verified credentials and must never accept an
actor UUID as proof of identity. There are no password/session/token APIs yet;
this library is **not ready to expose over HTTP**.

`bootstrapAdministrator(username)` is deliberately separate from normal
administration. It may be called by trusted, local initialization code only
when the `users` table is empty; it atomically inserts the first user and a
system administrator role assignment. The server must never expose this as a
remote endpoint. An imported v1 database requires this step before ordinary
operation. No implicit anonymous administrator is ever created.

## Persistent identity

All identities and relationship entities carry UUIDs. Newly created users,
groups, custom roles, custom permissions, memberships, role-permission grants,
and role assignments use `createUuidV7()` from AOWIS-SERVER-MODEL. Built-in
permissions and the built-in system-administrator role have stable, predefined
UUIDv7-format values, identical across installations.

Permissions have a unique stable string key independent of their UUID (for
example `project.model.write`). These are seeded in schema v2:

- `security.manage`: administer users, groups, roles, permissions, and grants.
- `project.create`: create projects (system scope).
- `project.read`: inspect projects, branches, revisions, and snapshot bytes.
- `project.branch.create`: create new branches.
- `project.model.write`: change the current working snapshot.
- `project.revision.finalize`: finalize the working revision.
- `project.permissions.manage`: reserved for future delegated administration.
- `project.history.read`: reserved for future operation-log browsing.
- `security.audit.read`: read and verify the system audit chain (schema V9).

`system.administrator` is assigned all built-in permissions by default.
`project.owner` is a stable built-in project-scoped role (all `project.*`
permissions except `project.create`) assigned automatically to the creator
inside the new project's creation transaction. Only a
system principal with `security.manage` can currently administer roles and
grants, preventing a project-scoped editor from escalating privileges. More
fine-grained delegation can be added later without changing identity IDs.

## Scopes and group membership

Role assignments have a UUID, user or group principal, role UUID, and a scope:
`system`, `project`, or `branch` plus the relevant resource UUID. Project-level
rights apply to its branches, but branch rights never apply to siblings. A
system grant applies to all resources. Group rights require active group
membership. Role assignment references and scopes are validated with SQLite
triggers; uniqueness and foreign keys prevent duplicate and dangling grants.

## Immutable provenance and hash encodings

New project, branch, and revision rows record their creating actor UUID.
Finalization stores the finalizing actor's UUID. New finalized revisions use
`AOWIS-REVISION-DAG-V2`, which hashes both creating and finalizing actor UUIDs
plus the creation timestamp, in addition to the existing project/branch/revision
metadata, snapshot digest,
and ordered parent UUID/hash pairs. V1 finalized revisions retain their exact
original hashes; verification dispatches by persisted `hash_format_version`.
Schema-v1 records have NULL actor fields (*unknown*), rather than claiming a
fabricated historical identity. If a working v1 revision is finalized after
migration, its unknown creator is represented by the null UUID in V2 hashing,
while the finalizing actor is recorded normally.

This section describes the historical V2 identity foundation. Subsequent
stages implement model-edit history, grouped undo/redo, HTTP sessions, and a
separate V9 security-administration audit journal. Group removal, permission
revocation, self-service login and external identity providers are still not
implemented. See `security-audit-v20.md` for scope and limitations.

As before, SQLite administrators can bypass triggers and modify the database.
The integrity chain is designed to reveal ordinary tampering, not to defend
against a determined database owner who can recompute digests.
