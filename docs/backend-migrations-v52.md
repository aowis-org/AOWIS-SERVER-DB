# V52 — SQLite migration isolation, V10–V13

Continuing V48–V51, the remaining version transitions now call
`StorageBackend::applyMigrationSchema` instead of executing SQLite migration
SQL in domain translation units. **The database user schema stays at V13.**
No public APIs, UUID formats, Model serialization, or finalized hash formats
are changed.

## V10 — security grant lifecycle

The SQLite adapter owns the historical V10 physical rebuild. In one
**caller-owned transaction**, it detects the three legacy grant tables,
removes legacy triggers, renames the old tables, creates the new revocable
membership/permission/assignment tables, copies UUIDs and grant records,
then drops the old tables. This must be atomic: incomplete migration cannot
leave identities, memberships, or authorization data partially rewritten.

Some older test fixtures downgrade `user_version` after a successful V10
upgrade. The adapter preserves the existing fast path: if all three grant
tables already expose `revoked_at_ms`, it restores the relevant lifecycle
indexes/triggers with `IF NOT EXISTS` and does not duplicate rows. A
partially upgraded catalog remains an error. The domain layer retains
responsibility for `BEGIN IMMEDIATE`, version advancement, `COMMIT`, and
`ROLLBACK`.

## V11–V13 — credentials and redaction

- V11: creation of `user_password_credentials` (historical `IF NOT EXISTS`).
- V12: password recovery token table, index, and immutable lifecycle triggers.
- V13: replacement of the V8 session identity trigger by the token-redaction
  guard. Both drops and the replacement run within the same transaction.

The strings for V10 lifecycle DDL, V11, V12 and V13 have been preserved
byte-for-byte from the original modules. The V10 rename and row-copy SQL
literals are also unchanged.

## Checks and limitations

`backend_contract_test.cpp` exercises V10 migration with nonempty grant
fixtures, forced partial DDL failure followed by rollback and retry, grant
UUID preservation, the idempotent already-upgraded path, V11 credential table,
V12 recovery creation/rollback/retry and V13 trigger replacement with a forced
failure that restores the original V8 guard.

Independent SQLite execution additionally checked SQL equality, catalog
schema consistency (88 non-internal entries), and V10/V12/V13 rollback and
retry behavior. The C++ regression suite must still run on the Qt-equipped
build host.

After this change, **SQLite schema migrations V1–V13 are isolated** behind
the backend. This does not imply PostgreSQL portability of domain SQL;
subsequent work should target SQL dialect/data binding differences and
transaction semantics before starting the PostgreSQL adapter.
