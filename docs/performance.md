# Database performance measurements

AOWIS stores granular hydraulic objects, their reference indexes, and an
append-only edit journal. Large networks must be measured with actual database
operations before optimizing object copying or finalization.

## Opt-in network persistence benchmark

Build the benchmark explicitly (it is **not** part of the normal 51-test CTest
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

### PostgreSQL: same workload, disposable schema

To compare backend behavior without using your configured AOWIS database,
start the existing disposable PostgreSQL 17 Docker setup and run the same
benchmark via one command:

```bash
./tools/benchmark_network_postgres.sh 1000
./tools/benchmark_network_postgres.sh 10000
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
relation storage only, not cluster overhead or WAL. Benchmarking concurrent
editors, history growth, and peak memory remain separate follow-up work.

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
