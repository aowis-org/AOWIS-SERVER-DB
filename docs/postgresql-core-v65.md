# V65 — PostgreSQL through the database core

The public `Database` facade now offers `openPostgres(database_name, schema_name)`
in addition to `openSqlite(file_path)`. Both use the same core operations,
security rules, transactions, immutable revision hashes, and security audit
hashes. There is no second PostgreSQL-specific implementation of the model
or history logic.

`database_name` is a PostgreSQL database name, **not** a libpq connection
string. The QPSQL driver uses PostgreSQL's usual connection environment
(e.g. `PGHOST`, `PGPORT`, `PGUSER`, `.pgpass`). The schema name is explicit:
`public` for ordinary deployments or another pre-existing schema. Schema names
are restricted to lowercase ASCII letters, digits, and underscores (maximum
63 characters; first character cannot be a digit). The selected schema must
exist and must be accessible. Failing schema validation closes the connection;
AOWIS must never silently create its tables in a different namespace.

Like SQLite, an empty selected schema is initialized with the **current**
schema and built-in authorization catalog in a single transaction. An existing
schema is verified rather than upgraded. No schema migration framework.

## Testing

The normal 48 tests remain SQLite-capable with no PostgreSQL installation.
Set `AOWIS_DB_TEST_PG_DB` only against a dedicated PostgreSQL **test** database
whose user may create and drop schemas:

```sh
AOWIS_DB_TEST_PG_DB=aowis_test \
  ctest --test-dir build-linux-tests \
  -R '^aowis-db-backend-contract$' --output-on-failure -V
```

The optional live test checks schema creation and rollback (V63/V64), then
creates a separate disposable schema and uses the actual public `Database`
methods to bootstrap an administrator, create a project, save a working
snapshot, reject a stale write, finalize, verify the project and security
audit hashes, and reopen the project. It finally drops only the uniquely named
test schema. Without that environment variable, these live tests do not run.

**Current scope:** PostgreSQL core workflow is *experimental* until this test
passes against a live PostgreSQL server and more domain/API paths receive
cross-backend parity tests. No HTTP server PostgreSQL selection, credentials
configuration INI, or general PostgreSQL production readiness is claimed.
