# V67 — PostgreSQL hydraulic Model round-trip

The optional live PostgreSQL backend-contract test now calls the **public**
`Database` hydraulic Model APIs. It uses the current schema in an isolated
PostgreSQL test namespace, not a separate PostgreSQL implementation of the
model or a historical database migration.

The typed integration scenario covers:

- A network with a reservoir, a junction, a pipe, and a pipe-attached demand
  point, including high-precision coordinates and Unicode metadata.
- `saveHydraulicNetwork()` and `loadHydraulicNetwork()` with byte-for-byte
  `ModelCodec<NetworkHydraulic>` round-trip comparison.
- Native PostgreSQL UUID columns in the materialized hydraulic membership
  index, its expected member count, and a populated reference index.
- Stale-generation rejection and a rejected invalid pipe endpoint without
  leaving an aborted PostgreSQL transaction or changing the model.
- A valid typed junction edit, then persistent group undo and redo.
- Finalized manifest SHA-256, branch creation from a finalized revision,
  successor revision hydration, integrity verification, and database reopen.

No SQL schemas, persisted formats, revision hashes, or public APIs change.
SQLite remains the default engine.

**The normal 48-test run does not exercise PostgreSQL.** To run this test,
provide a dedicated PostgreSQL test database, the Qt QPSQL driver and a user
with permission to create and drop schemas. This integration test creates
and drops its isolated schema and must never target a production database.

```sh
AOWIS_DB_TEST_PG_DB=aowis_test \
  ctest --test-dir build-linux-tests \
  -R '^aowis-db-backend-contract$' --output-on-failure -V
```

libpq uses `PGHOST`, `PGPORT`, `PGUSER` and the configured password source
for the connection. With `AOWIS_DB_TEST_PG_DB` unset, the backend-contract
test exercises SQLite and static PostgreSQL UUID conversions only.

The live PostgreSQL integration scenario has **not been executed** in the
authoring environment because Qt 6 and a PostgreSQL test server are absent.
