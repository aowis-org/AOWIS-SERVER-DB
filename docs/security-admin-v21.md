# V21 — Security grant lifecycle and reactivation

SQLite schema **V10** extends the identity and audit foundations to allow
revocation and later re-granting of the same membership, role assignment or
role-to-permission grant without destroying historical UUIDs.

## Lifecycle rules

- `group_memberships`, `role_permissions`, and `role_assignments` keep their
  existing UUIDs, plus nullable `revoked_at_ms` / `revoked_by_uuid` fields.
- Revocation is a one-time, immutable state change, rather than a row deletion.
  Triggers forbid deleting a grant or changing its identity or attribution.
- Unique constraints cover **active** grants only. Re-granting a revoked
  relation allocates a **new UUIDv7** while retaining the revoked predecessor.
- The authorization evaluator ignores revoked memberships, permission grants
  and role assignments immediately. This applies to direct and group grants.
- `enableUser` reactivates an existing disabled account and preserves its UUID.
  Disabling a user atomically revokes all of their outstanding bearer sessions;
  enabling does **not** resurrect those sessions. An administrator must issue
  a fresh session for the reactivated user.
- The final active system administrator cannot be removed by security
  mutations: the transaction is rejected and rolls back, with no audit event.

Each successful mutation is accompanied by an immutable audit-chain event
in the **same SQLite transaction**:

| Mutation | Audit action |
|---|---|
| Remove group membership | `group.member.remove` |
| Revoke role-to-permission grant | `role.permission.revoke` |
| Revoke role assignment | `role.assignment.revoke` |
| Reactivate user | `user.enable` |
| Disable user, including token invalidation | `user.disable` |

Re-granting uses the existing `addGroupMember`, `grantRolePermission`, and
`assignRole` APIs, which emit their corresponding original audit actions.
The audit chain retains its V1 format. Existing AOWIS project hashes and
models are unchanged.

## Local administration

These operations require the trusted local operator and the `security.manage`
permission at system scope:

```bash
./build-linux/aowis-db-admin remove-member DB_FILE ADMIN_UUID MEMBERSHIP_UUID
./build-linux/aowis-db-admin revoke-role-permission DB_FILE ADMIN_UUID GRANT_UUID
./build-linux/aowis-db-admin revoke-role-assignment DB_FILE ADMIN_UUID ASSIGNMENT_UUID
./build-linux/aowis-db-admin enable-user DB_FILE ADMIN_UUID USER_UUID
./build-linux/aowis-db-admin disable-user DB_FILE ADMIN_UUID USER_UUID
```

The API methods accept `OperationContext` and enforce the same policy as other
security mutations. Public HTTP endpoints are deliberately **not** added here.

## Migration and verification

Upgrading V9 to V10 runs transactionally, preserving the original UUIDs,
relationships, and active grants. Legacy V1–V8 migration paths still converge
on V10. A V10 database that is missing grant lifecycle columns or triggers is
rejected on open instead of silently allowing revoked permissions to reappear.

Limitations: user and role listing, renaming, external login/identity-provider
integration, detailed audit event payloads, and user/permission management UI
are later milestones. Local administration still assumes a trusted operator
with database file access.
