# Database performance measurements

AOWIS stores granular hydraulic objects, their reference indexes, and an
append-only edit journal. Large networks must be measured with actual database
operations before optimizing object copying or finalization.

## Opt-in SQLite network benchmark

Build the benchmark explicitly (it is **not** part of the normal 51-test CTest
suite and has no pass/fail timing thresholds):

```bash
cmake -S . -B build-linux-tests -G Ninja -DBUILD_TESTING=ON
cmake --build build-linux-tests --target aowis-db-network-benchmark
./build-linux-tests/aowis-db-network-benchmark 1000
./build-linux-tests/aowis-db-network-benchmark 10000
```

Increase to `100000` only when you are prepared for a substantially larger
benchmark. Each run creates and destroys an isolated temporary SQLite database;
it does not access the configured AOWIS database or modify your projects.

The synthetic workload contains one reservoir, N junctions and N connected
pipes, all with unique UUIDs. Output gives milliseconds for an initial model
save, model load, unchanged resave, one-junction edit, revision finalization,
branch creation and integrity verification, plus the SQLite file size after
closing the database. The construction of the in-memory test network is not
included in the database-operation timings.

### Interpretation

These are end-to-end **single-run** wall-clock measurements, not microbenchmarks.
They include SQL, object serialization, model validation, transaction commits
and journal writes. Results depend on hardware, filesystem, SQLite settings,
Qt build type and operating-system caches. Record build type, hardware and
network size when comparing results. Run more than once before drawing
conclusions; do not assume small-network timings extrapolate linearly.

`single_entity_edit_ms` uses the granular object API; `unchanged_save_ms`
uses the whole-network save API. They intentionally measure different workloads.
Database file bytes count the final SQLite file after closing the connection,
not transient WAL peaks or process memory. Benchmarking PostgreSQL, concurrent
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
using reusable prepared object/event statements. Updates to existing networks
continue through the ordinary version-checked object mutation path.

The journal still contains one immutable event per entity and one group per
user-visible save. Compare both speed and SQLite file size across identical
workloads before changing storage structures.
