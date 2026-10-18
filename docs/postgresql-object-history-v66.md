# V66 — PostgreSQL object and history parity

The existing optional **live** PostgreSQL contract test now exercises the
public `Database` API in a second, object-mode project within its disposable
PostgreSQL schema. The same object/group/undo/redo implementation is used for
both database engines. It also normalizes the UUID value transferred between object-event queries
and grouped-history inserts using `StorageBackend::readUuid()` and
`bindUuid()`. No separate domain implementation, schema changes, or hash
changes are introduced.

The V66 scenario checks:

- Binary object UUID/content round-trips, with embedded NUL and non-UTF8 bytes.
- Atomic grouped creation of two objects and the ordered event journal.
- A failed multi-object edit rolls back the earlier edit in that transaction.
- Stale revision generation rejection.
- A valid grouped edit and tombstone, then persistent undo and redo.
- Snapshot manifest SHA-256, finalization, immutable historical reads, and
  successor working-revision object copying.
- Close/reopen parity of the finalized model, history, and integrity chain.

**Live verification is opt-in**, not part of the ordinary 48 SQLite-only test
runs. It requires Qt's QPSQL plugin and a PostgreSQL test database with
permission to create/drop schemas. Run this against a disposable database:

```sh
AOWIS_DB_TEST_PG_DB=aowis_test \
  ctest --test-dir build-linux-tests \
  -R '^aowis-db-backend-contract$' --output-on-failure -V
```

When the environment variable is unset, the test does not exercise a live
PostgreSQL connection. Passing the ordinary suite alone must not be treated as
PostgreSQL compatibility validation. A PostgreSQL server is not present in the
authoring environment, so this new live scenario is not yet verified.
