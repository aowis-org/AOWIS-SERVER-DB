# V64 — PostgreSQL domain-schema smoke test

The PostgreSQL adapter remains internal. SQLite is still the only backend
available through the public `Database` API; exposing PostgreSQL there is
premature until all domain operations have equivalent behavior.

## Schema correction

`role_assignments.scope_uuid` is a native PostgreSQL `uuid` column. The V63
schema mistakenly attached `CHECK(length(scope_uuid)=16)`, a SQLite BLOB check
that PostgreSQL cannot apply to `uuid`. The UUID type itself enforces its
representation, so the redundant check has been removed. The existing scope
kind/NULL check is retained.

The SQLite schema and persisted revision/audit hash formats are unchanged.
There is no database schema migration framework; development databases are
created fresh from the current schema.

## Optional live contract verification

If `AOWIS_DB_TEST_PG_DB` is set, `aowis-db-backend-contract` connects through
Qt's QPSQL driver. In one transaction, it creates a uniquely named temporary
**schema** (not a SQL temporary table), creates the current AOWIS tables and
triggers, tests native UUID and BYTEA storage, and then exercises:

- An attributed user, project, main branch and working revision.
- System-scoped role assignment (`scope_uuid IS NULL`) and project-scoped
  assignment (`scope_uuid` is a native PostgreSQL UUID).
- Missing-finalization-actor rejection, protected by a SAVEPOINT so that a
  failed PostgreSQL statement doesn't abort the whole test transaction.
- Successful finalization with a V2 attribution/history seal.
- Rejection of edits to the finalized snapshot, using another SAVEPOINT.
- Creating the successor working revision and linking its finalized parent.
- Rolling back *everything*, including schema creation and records.

This verifies database constraints and UUID behavior. It is **not yet a test
of the public `Database` API against PostgreSQL**; most domain queries still
need end-to-end portability testing before that interface can be enabled.

For example, with a dedicated disposable PostgreSQL test database and
appropriate libpq environment or service settings:

```sh
AOWIS_DB_TEST_PG_DB=aowis_test ctest --test-dir build-linux-tests \
  -R '^aowis-db-backend-contract$' --output-on-failure -V
```

The configured PostgreSQL user must have permission to create a schema. Do
not point this optional test at any production database. If the variable is
unset, the PostgreSQL live exercise is skipped and the normal SQLite contract
test still runs.
