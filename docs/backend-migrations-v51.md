# V51 — SQLite DDL isolation for V6–V9

This patch follows V48–V50 without changing the SQLite user schema version
(currently V13), Model serialization, UUID representation, revision ancestry,
or immutable hash formats. It extracts historical SQLite-specific DDL from four
domain translation units to `src/sqlite_schema_migrations.cpp`.

## Split migration phases

- V6: `applyMigrationSchema(6)` creates `hydraulic_network_members` and its
  index. The existing Model-layer migration then decodes every hydraulic
  network root and backfills membership rows. Only after that succeeds does
  `completeMigrationSchema(6)` install immutable-membership triggers.
- V7: `applyMigrationSchema(7)` creates `hydraulic_network_references` and its
  index; the existing Model-layer backfill computes the reference rows using
  typed Model validation and persistence. `completeMigrationSchema(7)` then
  installs immutable-reference triggers.
- V8: `applyMigrationSchema(8)` creates session tables, indexes and triggers.
- V9: `applyMigrationSchema(9)` creates the immutable security-audit table,
  index and triggers. The domain service still seeds and validates the stable
  `security.audit.read` permission and the administrator grant.

Every call requires a caller-owned `BEGIN IMMEDIATE` transaction. In V6/V7,
both schema phases and the data backfill execute within **one** transaction;
on failure, the domain layer rolls the entire version transition back. The
adapter does not commit, roll back or set the schema version.

## Verification and tests

The migration SQL strings were compared with the original V6–V9 source and
verified unchanged. An independent Python SQLite schema comparison applied
original and extracted V1–V9 schema DDL to separate temporary in-memory
connections and compared their `sqlite_master` entries. All 76 non-internal
schema entries matched exactly. This verification used empty model data, so it
does not replace C++ integration tests of non-empty membership backfills.

`backend_contract_test.cpp` now exercises V6/V7 pre- and post-backfill phases,
V8/V9 schema creation, missing guards before backfill, successful guard
installation, unsupported versions and forced mid-V7 migration failure with
rollback and retry.

## Next step

V10 is a special security-grant table rebuild with legacy data migration and
compatibility behavior for downgraded schema-version test fixtures. Extract
its SQLite physical migration separately, then move V11–V13 credentials,
recovery and session-redaction DDL. Do not rewrite historical migrations or
remove their compatibility behavior. Later backend work must also abstract
SQLite-only domain queries, UUID binding, and concurrency semantics before
claiming PostgreSQL portability.
