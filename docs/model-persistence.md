# V5 — Typed AOWIS hydraulic network persistence

## Supported AOWIS Model types

The DB's typed binary codec encodes the complete `NetworkHydraulic` model
structure reachable through its public fields. This includes 68 nested
struct types and 444 persisted structure fields from the currently pinned
AOWIS-SERVER-MODEL submodule. Field order and type are explicitly specified
in `src/model_codec.cpp`. No Qt meta-object reflection, raw struct memory,
lossy JSON numeric conversion, or EPANET serialization is involved.

`ModelCodec<T>::encode(value)` and `ModelCodec<T>::decode(bytes, &error)`
cover each of these structures. Payloads are prefixed with the `AMD5` binary
magic (`0x414d4435`), format number 1, and full Model struct type name.
Every field is then streamed in declaration order with Qt's QDataStream 6.0
format, big-endian byte order and double-precision floating point. Optional
values have explicit 0/1 presence bytes; lists have unsigned 32-bit element
counts and order-preserving item encoding; enums use signed 32-bit integers.
The decoder rejects wrong types, unsupported versions, truncated payloads,
trailing bytes and oversized model lists. **Future changes to this encoding
must introduce a new format and preserve the V5/format-1 reader.**

The CMake configure step checks `docs/model_codec_fields_v1.json` against the
Model submodule and verifies that every listed field is handled by both the
writer and reader. If the Model submodule gains, removes, or changes fields,
CMake will refuse to configure until the codec is evolved intentionally.

## Storage and API

`saveHydraulicNetwork(actor, branch_uuid, expected_generation, network)`
atomically creates or synchronizes one hydraulic network in the working
revision. The root `hydraulic.network` record contains serialized network
settings and **ordered lists of entity UUIDs**; each of its 18 top-level
entity collections is represented by separate versioned object records.
Nested values belonging to an entity (e.g., demand lists, pipe vertices,
demand-point meters and per-entity metadata) are stored in that entity's
payload. MSX definitions and non-entity configuration live in the network
root. Only actual modifications emit `object_events`.

When replacing a network, removed top-level entities become V4 tombstones.
The operation checks for null/duplicate UUIDs, for UUID conflicts with other
object kinds and for model entities referenced by other networks in the same
revision. A stale whole-revision generation causes the **entire save to fail**.

`loadHydraulicNetwork(actor, revision_uuid, network_uuid)` reconstructs a typed
`NetworkHydraulic` from a single revision, preserving entity list order and
checking each referenced object kind and embedded UUID. Finalized revisions
remain immutable; branch creation and finalization copy the already-persisted
objects without re-encoding them.

For an already referenced entity, `putHydraulicEntity<T>(database, actor,
branch_uuid, entity, expected_version)` applies a granular edit, using the
same V4 per-object optimistic concurrency and change journal. Adding/removing
entities, or updating list order, uses `saveHydraulicNetwork` to update the
network index atomically. `loadHydraulicEntity<T>` fetches a single entity.

## Integrity and limitations

The V4 manifest includes every stored entity's exact bytes, version, tombstone
and object-history journal digest. Finalized V2 chain hashes therefore cover
V5 serialized model values **without altering any pre-existing revision hash**.
There is no new SQLite schema migration: V5 uses existing V4 tables.

Model references are serialized without graph-level semantic validation.
For example, it remains possible to store a pipe referencing a non-existent
junction. External backdrop images are represented by the `file` string;
the referenced file's contents are not yet managed, snapshotted or hashed.
Persisting `Project` descriptions, other Model domains, asset attachments,
explicit human-meaningful grouped operations, and object-level undo/redo
requires later stages. `NetworkHydraulic` is currently the only full-model
aggregate handled here. Do not claim this stores an entire AOWIS project with
all associated assets.

A full-network save is atomic but involves linear scans of the referenced
object set and may be expensive for large networks. Granular entity updates
avoid whole-network writes, but the future GUI needs to group complex edits
into one transaction with referential validation. The record bytes are stable
for the pinned codec version, and schema compatibility is checked at build
time; do not silently regenerate an existing version for newer Model fields.
