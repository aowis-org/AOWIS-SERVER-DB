# Hydraulic Model persistence

The database stores `NetworkHydraulic` data through a deterministic, typed
codec and granular model objects. `src/model_codec.cpp` implements the encoding;
`src/model_persistence.cpp` maps the model to persistent objects, membership
indexes and reverse references. The public API is declared in
`include/aowis/db/model_persistence.h` and `include/aowis/db/database.h`.

## Model schema coverage

At CMake configuration time, `tools/check_model_codec.py` compares the checked-
out AOWIS-SERVER-MODEL structures against `docs/model_codec_fields.json`, then
checks that both the codec writer and reader cover the same ordered fields.
`tools/check_model_field_merge.py` checks that merge comparison rules cover
those fields. A mismatch stops configuration instead of silently dropping
model data. Update the manifest and the codec deliberately and together after
reviewing Model changes.

The binary encoding uses fixed Qt `QDataStream` settings, deterministic field
ordering, exact numeric values and stable object identifiers. The on-disk codec
has its own format identifier and must preserve deterministic bytes across
SQLite and PostgreSQL for storage integrity and eventual replica exchange.

## Objects and references

Hydraulic networks contain typed reservoirs, junctions, tanks, pipes, pumps,
valves, demand points, curves, patterns, controls, metadata and configuration.
Objects have UUIDs, object kinds, expected-version counters and deletion state.
The persistence layer maintains derived network membership and reverse-reference
indexes for validating edits efficiently. Referenced objects cannot simply be
removed while they remain in use; mutations are checked transactionally.

The network update path supports indexed saves and incremental object groups.
Dependency-local checks cover structural changes to pipes and demand points,
link endpoint changes, and referenced hydraulic resources. Untouched entities
should not require an entire network to be rewritten on every edit.

An edit group commits the changed objects, updated indexes and recorded history
as one database transaction. Optimistic version checks detect stale writers.
Undo and redo are journaled compensating changes rather than removal of old
history entries. See [History and merges](history-and-merges.md).

## Boundaries

Current typed round-trip support is centered on the hydraulic network domain.
Do not assume a saved revision already packages every other future AOWIS model
domain, external raster backdrop, terrain source or attached file. A serialized
path to an external asset does not guarantee the asset's contents are preserved.
Large-network storage and performance still require dedicated benchmarking.

Hashes of frozen project states use exact deterministic bytes, not SQLite
pages, incidental JSON order or a PostgreSQL physical backup. UUID byte
encodings used for hashed manifests and hydraulic comparison keys are distinct
from driver-specific parameter binding; do not substitute Qt SQL UUID values
for canonical RFC-4122 bytes inside integrity formats.
