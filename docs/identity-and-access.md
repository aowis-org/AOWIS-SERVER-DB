# Identity, authorization and security audit

AOWIS database operations are attributed to user identities. Users, groups,
roles, permissions, memberships, grants and assignments have UUIDs. Built-in
permissions also have stable machine-readable keys and predefined IDs across
installations. The database core enforces authorization for embedded and REST
clients rather than relying on the HTTP route alone.

## Principals, roles and resource scopes

Users can belong to groups; both users and groups can receive role assignments.
Roles contain permissions. Role assignments can be scoped to the whole system,
a project, or a branch. Project permissions apply to their branches; branch
permissions do not grant access to sibling branches. Inactive users or group
memberships must not gain rights through stale assignments.

`system.administrator` is the built-in system administrator role. `project.owner`
is a built-in project-scoped role automatically assigned to a newly created
project's creator. The permission vocabulary includes project creation,
project reads, branch/revision edits and finalization, history access,
permission management and security audit access. Consult the built-in catalog
in `src/builtin_identity_ids.h` and the database schema for the definitive
keys and grants.

## Authentication

Remote access uses authenticated bearer sessions. The server resolves the
session token into a trusted operation context; clients cannot choose their
own acting user UUID. Local password credentials, login, password change,
recovery, revocation and bounded session capacity are implemented. Token
redaction removes spent secrets while retaining appropriate administrative
records. Login throttling runs before expensive password verification.

The local `aowis-db-admin` program is a *trusted operator tool*, not a remote
login client. Its administrator UUID identifies the responsible actor; local
access to the selected database is part of the trusted deployment boundary.
See the command reference in the repository [README](../README.md).

## Audit and integrity

Security administration events have a separate append-only audit log and
integrity verification. Model edits and finalized revision ancestry have their
own histories. This separation is intentional: changing a user's rights is
not an engineering model revision. Ordinary SQL guards discourage mutations
to finalized records, but database owners with sufficient privileges can
remove those guards and reconstruct history.

Do not expose the local HTTP listener directly to the public internet. The
included server binds to loopback and does not terminate TLS. PostgreSQL
credentials should come from libpq facilities such as `.pgpass` and service
configuration, not a checked-in AOWIS INI file.
