# Performance measurement and profiling

This guide describes the current benchmark executable and diagnostics for
AOWIS-SERVER-DB. It is a **measurement procedure**, not a changelog or a
record of optimization patches. Immutable shared snapshots reduce historical
storage duplication, but complete model operations and independent branch
creation still perform work proportional to model size.

## Build and run the network benchmark

The opt-in `aowis-db-network-benchmark` is separate from CTest. Build it
**explicitly after source changes**, because the normal test target may not
rebuild this optional executable:

```bash
cmake -S . -B build-linux-tests -G Ninja -DBUILD_TESTING=ON
cmake --build build-linux-tests --target aowis-db-network-benchmark
./build-linux-tests/aowis-db-network-benchmark 1000 --revisions 50
./build-linux-tests/aowis-db-network-benchmark 10000 --revisions 10
```

The synthetic model contains one reservoir, N junctions and N connected pipes.
Each successive revision edits a junction and finalizes it. The benchmark
measures database work, not fixture creation. SQLite runs use an isolated
temporary database and do not modify configured projects.

For a small baseline or a larger stress workload:

```bash
./build-linux-tests/aowis-db-network-benchmark 1000 --revisions 1
./build-linux-tests/aowis-db-network-benchmark 100000 --revisions 1
```

`--revisions` accepts 1–100. A 100,000-pipe model consumes substantial time,
CPU, disk and memory; start with a single revision.

## PostgreSQL benchmark

The benchmark script starts a disposable PostgreSQL container and schema,
executes the same workload, and cleans up:

```bash
./tools/benchmark_network_postgres.sh 1000 --revisions 10
./tools/benchmark_network_postgres.sh 10000 --revisions 1
```

Docker is required for the disposable runner. Alternatively, the binary can
use a **dedicated disposable** existing PostgreSQL test database selected by
`AOWIS_DB_TEST_PG_DB` and the libpq connection environment, with `--postgres`.
It must never target production. PostgreSQL size measurements refer to the
test schema's relations, indexes and TOAST data, not the entire cluster, WAL
or any other database.

## Reported fields

| Output field | Interpretation |
| --- | --- |
| `initial_save_ms` | Saving the complete initial model |
| `load_ms`, `unchanged_save_ms` | Loading and resaving without model changes |
| `single_entity_edit_ms` | An edit to one existing junction |
| `finalize_ms` | Initial finalization, including immutable root creation |
| `create_branch_ms` | Materializing an independent branch from a finalized state |
| `verify_history_ms` | Ordinary finalized-history integrity checks |
| `repeat_finalize_total_ms`, `repeat_finalize_max_ms` | Later finalizations over additional revisions |
| `repeat_edit_total_ms` | Sum of later individual edit durations |
| `revision_samples_number_edit_ms_finalize_ms` | Per-revision edit/finalization breakdown |
| `revision_object_rows` | SQL working-object rows retained at the end, **not** all historical objects |
| `sqlite_file_bytes` | On-disk SQLite file bytes, including potentially reusable allocated pages |
| `postgres_schema_bytes` | PostgreSQL test-schema relation storage |
| `verify_journal_ms` | Optional forensic journal replay with `--deep-verify` |

These are end-to-end wall-clock observations. Results depend on the host,
filesystem, caches, model complexity, backend and contention. Run comparable
workloads multiple times and avoid simultaneous heavy jobs. A faster single
operation does not establish improved total throughput or concurrency.

## Memory and deep-integrity checks

On Linux, `--memory` emits current and peak process RSS at execution
checkpoints, using `/proc/self/status`; it includes Qt, benchmark fixture
copies, SQLite client code and other allocations in the process:

```bash
./build-linux-tests/aowis-db-network-benchmark 10000 --revisions 3 --memory --deep-verify \
    2>&1 | tee /tmp/aowis-db-stress-10k.log
./build-linux-tests/aowis-db-network-benchmark 100000 --revisions 1 --memory --deep-verify \
    2>&1 | tee /tmp/aowis-db-stress-100k.log
```

`--deep-verify` invokes forensic journal replay against each finalized model
in addition to ordinary history verification. It can require full logical
model reconstruction across many revisions; do not treat its running time as
ordinary finalization overhead.

**On PostgreSQL**, benchmark process RSS does not include the separately
running database server. Measure server/container memory independently.

## Phase profiling

Use `--profile` to enable detailed diagnostics (`AOWIS_DB_PROFILE_PHASES`,
`AOWIS_DB_PROFILE_SAVE` and `AOWIS_DB_PROFILE_LOAD`) for immutable content,
root construction, transactions, model saves and loads:

```bash
./build-linux-tests/aowis-db-network-benchmark 10000 --revisions 1 --profile \
    2>&1 | tee /tmp/aowis-db-profile.log
./tools/benchmark_network_postgres.sh 1000 --revisions 1 --profile \
    2>&1 | tee /tmp/aowis-db-postgres-profile.log
```

`aowis_db_profile` reports phases such as immutable-content batching, tree
construction, root finalization, SQL working-head operations and branch
materialization. `aowis_network_save_profile` separates validation,
serialization, lookups, writes, membership/reference indexing, grouped history
and commit. `aowis_network_load_profile` includes membership lookup, direct
object lookup and decoding. For a single save outside the benchmark, set
`AOWIS_DB_PROFILE_SAVE=1` in the environment.

Profile timings are diagnostic and may add overhead. Some spans nest inside
others: **do not add a parent and its child spans together**. Use unprofiled
`finalize_ms` and `create_branch_ms` for end-to-end comparisons.

## Expected workload behavior

- The first finalized object-mode state builds its complete immutable root.
  Subsequent revisions can reuse unchanged content and UUID-keyed tree paths.
- Repeated finalization transactionally hands SQL working objects and derived
  hydraulic indexes to the successor. The handoff still handles the complete
  working state with set-based SQL.
- Creating an independent branch reads the finalized model and builds its own
  working state and indexes, with structural and reference validation.
- Historical point lookups follow the immutable snapshot index and do not
  replay earlier revisions. Full history or whole-network verification must
  inspect more content.
- PostgreSQL import and large-model reads use bounded/set-based SQL paths
  where appropriate. Driver/network overhead and server query plans remain
  measurable costs.

Do not infer specific latency, storage reduction or scaling guarantees without
running an equivalent benchmark. See [Versioned storage](versioned-storage.md)
for the storage format and [Testing](testing.md) for correctness tests.
