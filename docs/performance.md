# Database performance measurements

AOWIS stores granular hydraulic objects, their reference indexes, and an
append-only edit journal. Large networks must be measured with actual database
operations before optimizing object copying or finalization.

## Opt-in network persistence benchmark

Build the benchmark explicitly (it is **not** part of the normal CTest
suite and has no pass/fail timing thresholds):

```bash
cmake -S . -B build-linux-tests -G Ninja -DBUILD_TESTING=ON
cmake --build build-linux-tests --target aowis-db-network-benchmark
./build-linux-tests/aowis-db-network-benchmark 1000
./build-linux-tests/aowis-db-network-benchmark 10000
```

Increase to `100000` only when you are prepared for a substantially larger
benchmark. In default SQLite mode, each run creates and destroys an isolated
temporary database; it does not access the configured AOWIS database or modify
your projects.

The synthetic workload contains one reservoir, N junctions and N connected
pipes, all with unique UUIDs. Output gives milliseconds for an initial model
save, model load, unchanged resave, one-junction edit, revision finalization,
branch creation and integrity verification, plus the SQLite file size after
closing the database. The construction of the in-memory test network is not
included in the database-operation timings.

### Revision-history growth (opt-in)

Measure **many finalized revisions** instead of just one. Each new revision
changes one existing junction, finalizes the entire project and creates its
next working revision. This intentionally isolates revision copying costs from
full-network imports and avoids artificial no-op finalizations.

```bash
cmake --build build-linux-tests --target aowis-db-network-benchmark
./build-linux-tests/aowis-db-network-benchmark 1000 --revisions 10
./build-linux-tests/aowis-db-network-benchmark 1000 --revisions 50
./build-linux-tests/aowis-db-network-benchmark 10000 --revisions 10
```

For the high-end network case, begin with one revision before running many:

```bash
./build-linux-tests/aowis-db-network-benchmark 100000 --revisions 1
```

For each run, record these results:

- `finalized_revisions`: number of finalized main-branch revisions.
- `revision_object_rows`: physical object rows across **all** revisions,
  including the two working heads (`main` and the `alternative` branch).
- `revision_samples_number_edit_ms_finalize_ms`: `revision:edit_ms:finalize_ms`
  tuples, separated by semicolons. The first tuple describes the original
  edit/finalize and later tuples describe incremental revisions.
- `repeat_finalize_total_ms`, `repeat_finalize_max_ms`: finalization timing
  for revisions 2 through N, excluding the first baseline finalization.
- `sqlite_file_bytes` / `postgres_schema_bytes`: physical storage used after
  the benchmark closes its main connection (SQLite) or its final writes
  (PostgreSQL).
- `verify_history_ms`: full-history verification time after N finalizations.

The benchmark creates an independent `alternative` branch from the first
finalized revision **after** completing the main-branch history. The extra
working head and its object rows are deliberately included in the total
storage measurements; compare runs of the same shape. After the last
iteration, the benchmark reloads the latest working model and checks the
final edited junction to catch incorrect revision inheritance.

The revision count is bounded to 1..100 to avoid accidental runaway benchmark
runs. There are no pass/fail performance thresholds; run multiple times under
similar host load and compare results empirically. The benchmark remains
separate from CTest and must not run against operational databases.

### PostgreSQL: same workload, disposable schema

To compare backend behavior without using your configured AOWIS database,
start the existing disposable PostgreSQL 17 Docker setup and run the same
benchmark via one command:

```bash
./tools/benchmark_network_postgres.sh 1000
./tools/benchmark_network_postgres.sh 10000
./tools/benchmark_network_postgres.sh 1000 --revisions 10
```

The helper requires Docker Engine/Compose v2 and an existing configured CMake
`build-linux-tests` directory. It builds the opt-in benchmark if needed,
waits for PostgreSQL to become healthy, and removes the container and its data
on success **or failure**. Each invocation uses a new Docker Compose project;
the test database is exposed only on a random localhost port. The benchmark
creates a uniquely named schema, opens it using the production database core,
then drops the schema when finished.

For an already-running **dedicated disposable test database** (not production),
use the benchmark directly with libpq connection settings:

```bash
AOWIS_DB_TEST_PG_DB=aowis_test PGHOST=127.0.0.1 PGPORT=5432 \
    PGUSER=aowis_test ./build-linux-tests/aowis-db-network-benchmark 1000 --postgres
```

Provide credentials through `PGPASSWORD`, `.pgpass`, or libpq service
configuration as appropriate; never put them in the command line or commit
them. `--postgres` intentionally refuses to run without
`AOWIS_DB_TEST_PG_DB`. The benchmark drops **only** its own generated schema,
not other schemas. `postgres_schema_bytes` counts table, TOAST and index
storage within that schema (not the whole PostgreSQL cluster). This is a
different storage-size measure from the SQLite file, so compare cautiously.

### Interpretation

These are end-to-end **single-run** wall-clock measurements, not microbenchmarks.
They include SQL, object serialization, model validation, transaction commits
and journal writes. Results depend on hardware, filesystem, SQLite settings,
Qt build type and operating-system caches. Record build type, hardware and
network size when comparing results. Run more than once before drawing
conclusions; do not assume small-network timings extrapolate linearly.

`single_entity_edit_ms` uses the granular object API; `unchanged_save_ms`
uses the whole-network save API. They intentionally measure different workloads.
SQLite file bytes count the final database file after closing the connection,
not transient WAL peaks or process memory. PostgreSQL schema bytes count its
relation storage only, not cluster overhead or WAL. Benchmarking concurrent editors and peak memory remains separate follow-up
work. The optional `--revisions` workload measures history growth, but not
peak RAM or simultaneous editing throughput.

## Profile first-seal and branch phases (opt-in)

For profiling **the expensive initial Merkle seal**, rebuild the benchmark
explicitly, then run only one finalized revision:

```bash
cmake --build build-linux-tests --target aowis-db-network-benchmark
./build-linux-tests/aowis-db-network-benchmark 10000 --revisions 1 --profile \
    2> /tmp/aowis-merkle-sqlite-profile.txt
```

Use the same isolated PostgreSQL benchmark with an inherited environment flag:

```bash
AOWIS_DB_PROFILE_PHASES=1 ./tools/benchmark_network_postgres.sh 1000 --revisions 1 \
    2> /tmp/aowis-merkle-postgres-profile.txt
```

The profile reports to **stderr** in machine-readable lines such as
`aowis_db_profile op=merkle_first_seal phase=scan_and_encode_objects elapsed_us=... count=...`.
It is disabled by default. Neither profiling nor these commands change the
schema, hashing format, transaction semantics, or regular test suite.

Important phase groups:

- `finalize` / `finalize_transaction`: authorization and state lookup, Merkle
  root sealing, snapshot/history hashing, creation of successor metadata,
  set-based working-head transfer, removal of old working rows, and commit.
- `merkle_first_seal`: object-row scan and deterministic record encoding;
  immutable blob storage; Merkle-tree construction. On later revisions the
  corresponding name is `merkle_incremental_seal` and only changed UUIDs
  are processed.
- `content_blobs` / `content_nodes`: content SHA-256 hashing/deduplication,
  SQL insertion, and integrity-checked readback of **reused** records only.
  The count is the number of input, unique, or reused records, depending on
  the phase.
  Content hashing uses the already-required OpenSSL EVP SHA-256 implementation
  when available, with Qt SHA-256 as a fallback. The domain-separated preimage
  and canonical hashes are identical for both implementations; the test suite
  pins independent binary test vectors. No hash or database format changed.
- `merkle_bulk_build`: UUID sorting and validation, blob-hash verification,
  canonical Merkle node construction, and node persistence.
- `create_branch` / `branch_materialization`: new branch metadata, Merkle
  traversal, verified blob decoding, new working-object SQL insertion,
  hydraulic-index rebuild, and commit.

Timings are **monotonic microseconds**, not CPU times. Nested operations are
reported both as a parent span and as more detailed child spans; **do not add
parent and child timings together**. The benchmark's usual `finalize_ms` and
`create_branch_ms` remain the comparable end-to-end wall-clock figures.

Use `--revisions 1` to study first-seal costs without the noise of many
incremental finalizations, then repeat with `--revisions 10` to see the
incremental path. The benchmark uses disposable databases and does not create
production data. The profile itself can add a little overhead; compare
unprofiled runs for performance conclusions. Send the `aowis_db_profile`
lines and standard benchmark output together for diagnosis.

## Profile a network save

Set `AOWIS_DB_PROFILE_SAVE=1` to print a single phase summary per successful
`saveHydraulicNetwork()` invocation. For example:

```bash
AOWIS_DB_PROFILE_SAVE=1 ./build-linux-tests/aowis-db-network-benchmark 10000
```

The `aowis_network_save_profile` line includes validation and serialization,
transactional lookups, object/journal writes, membership/reference indexing,
group-journal construction and commit. All times are wall-clock milliseconds;
query planning, driver overhead and cache effects are included. The diagnostics
are disabled by default. `fresh_insert=1` indicates an entirely new network
using prepared object/event statements on SQLite or bounded JSONB
batches on PostgreSQL. Updates to existing networks
continue through the ordinary version-checked object mutation path.

The journal still contains one immutable event per entity and one group per
user-visible save. Compare both speed and SQLite file size across identical
workloads before changing storage structures.

## Set-based network indexing and journal grouping

For fresh networks, membership rows are selected directly from the newly
committed object's event-sequence range, rather than inserting one row per
member. Fresh-network references skip the empty-index lookup and insert the unique
validated references in batches of up to 256. Each batch binds the revision
and network IDs once, then source UUID, target UUID and field per reference;
770 parameters remain below SQLite's historical 999-placeholder limit.
Both SQLite and PostgreSQL use the same `WITH ... VALUES ... INSERT SELECT`
statement. Existing-network edits retain the reference-delta logic, including
their 64-row insert path.

Grouped-history links are populated with an ordered `INSERT ... SELECT` using
`ROW_NUMBER()`, on SQLite and PostgreSQL. The insertion count must exactly
match the revision generation delta or the entire save is rolled back.
Event UUIDs, sequence numbers, journal payloads, hashes and revision behavior
are unchanged.

Profiling now separately prints `membership_ms` and `references_ms`, which
sum to `index_ms`. Compare those and `group_ms` to previous runs, ideally on
both 1,000- and 10,000-pipe workloads. Timings are not acceptance thresholds.

## Indexed reads when saving an existing network

An existing network's members are loaded using the ownership index
`hydraulic_members_by_network` joined to the composite object primary key,
instead of querying every member through many generated `IN (...)` lists.
Newly introduced UUIDs, the network root, and unexpectedly missing membership
rows are still looked up by their primary keys. This preserves conflict checks
for unrelated generic objects, cross-network ownership and tombstoned rows.

For established networks, the membership-index join itself proves ownership
of returned members because `(revision_uuid,entity_uuid)` is unique. Explicit
cross-network collision checks are therefore restricted to newly introduced
UUIDs and unexpectedly missing membership rows. Fresh networks still check
all incoming UUIDs. A missing index row never makes an already-stored UUID
safe to insert, and collision failures roll back the enclosing transaction.

The save profile reports `ownership_ms` (time spent on explicit cross-network
collision queries) and `fetch_ms` (time spent loading relevant stored objects).
`lookup_ms` includes those two phases plus the permission, revision, and root
lookups. An unchanged save must leave the revision generation and event count
unchanged; the indexed read changes no SQL rows or history semantics.

Compare multiple runs under similar CPU and disk load. A benchmark run in which
load, finalization, branching and history verification all slow down together
cannot by itself establish a regression in the save path.

## PostgreSQL initial network import

Fresh-network persistence on PostgreSQL uses bounded JSONB batches passed to
`aowis_insert_fresh_network_objects`. The server-side routine executes the
original per-entity object insertion, working-revision generation increment,
and immutable history event insertion in sequence. It preserves the existing
PostgreSQL immutability and revision triggers and runs inside the original
write transaction. SQLite retains its prepared-statement implementation, and
existing-network edits do not use this path. PostgreSQL's current schema must
include this routine (development schemas can be recreated from scratch).

This optimization avoids three client/server SQL executions per new entity.
Benchmark timings are hardware- and load-dependent; use the PostgreSQL runner
for an actual before/after measurement.

## Merkle-root construction and reproducible revision benchmarks

The first object-mode revision has no finalized parent root to reuse. It now
builds its canonical Merkle radix tree in memory and inserts immutable nodes
and blobs in batches; incremental revisions continue using copy-on-write updates
for their changed UUIDs. Both paths must produce identical root hashes for
identical model states.

**Always rebuild the benchmark executable after changing the storage engine.**
The regular CTest build can leave this opt-in benchmark binary outdated:

```bash
cmake --build build-linux-tests --target aowis-db-network-benchmark
./build-linux-tests/aowis-db-network-benchmark 1000 --revisions 50
./build-linux-tests/aowis-db-network-benchmark 10000 --revisions 10
./tools/benchmark_network_postgres.sh 1000 --revisions 10
```

`revision_object_rows` counts retained *working* SQL rows under Merkle
cutover, not all historical model objects. The SQLite database file size is a
physical allocation measurement and may include reusable pages. Compare
multiple network sizes, revision counts, and backends before attributing
performance changes to a particular optimization.

### Git-inspired working-head performance (October 2026)

After switching finalized revisions to shared Merkle roots, moving the SQL
working head at finalization gave much faster *subsequent* revisions:

- SQLite, 1,000 pipes: 50 subsequent finalizations in about 2.8 s total.
- SQLite, 10,000 pipes: 10 subsequent finalizations in about 6.1 s total.
- PostgreSQL, 1,000 pipes: 10 subsequent finalizations in about 8.6 s total.

The first seal and **independent branch creation** still rebuild substantial
model state. Before the branch-index optimization, branch creation took 1.0 s
for 1,000 pipes and 12.0 s for 10,000 pipes on SQLite; 1.0 s for 1,000 pipes
on PostgreSQL. During branch creation we now reuse the working object rows
already reconstructed and hash-verified from the Merkle root, instead of
reading all finalized immutable objects a second time. Missing working
network members are retrieved with bounded SQL batches. This is an optimization
of redundant I/O, **not** an O(1) branch-creation guarantee. Re-run the
original disposable benchmarks and compare `create_branch_ms` directly.


### First-seal insertion optimization

During first-seal construction, immutable content is hashed and inserted in
batches using `INSERT ... ON CONFLICT DO NOTHING RETURNING sha256`. Only
conflicting **pre-existing** hashes are fetched again and checked for exact
content and SHA-256 integrity. Newly inserted payloads already came from the
current transaction's verified hash inputs; rereading them immediately added
avoidable database traffic and rehashing. Normal reads and later history
verification still check all content independently.

The first-seal Merkle builder uses blob hashes returned directly by the
successful blob insert in that same transaction, so it skips one redundant
full-set blob-reference read. Standalone `merkle_tree::build()` still validates
arbitrary blob references. The Merkle encoding, root hashes, and persistent
schema are unchanged.

When `--profile` is enabled, `content_blobs` and `content_nodes` report
`verify_reused_content` (the number of pre-existing items verified) rather
than verifying every newly inserted item again. A first seal on a fresh
database should typically show zero reused items.

### Avoid double-hashing immutable Merkle nodes

The initial Merkle builder must calculate every node SHA-256 before its
parent can be encoded. Passing raw node bytes to the general batch inserter
previously hashed those same bytes **again**, even though the hash had just
been calculated during bottom-up construction.

`content_store::PreparedContent` now binds a payload to the digest computed
at construction; callers cannot provide a separate hash. The bulk builder
uses that digest both for parent links and for batched immutable insertion.
Reused SQL rows still undergo exact-content and SHA-256 verification; fresh
rows are protected by the existing transaction. Public `putMany()` continues
to calculate the hashes of arbitrary caller payloads, while `putManyPrepared()`
reuses prepared identities. The stored bytes, hash domain, Merkle roots, and
schema are unchanged.

With `--profile`, compare `merkle_bulk_build`'s `construct_hash_nodes` and
`content_nodes`'s `deduplicate_prepared` against the former
`content_nodes` `hash_and_deduplicate` phase. The latter no longer repeats
SHA-256 work on all Merkle nodes. First-seal time can still vary with CPU,
storage and concurrent build/test load.
