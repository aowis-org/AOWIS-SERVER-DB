## SQLite schema during development

AOWIS-SERVER-DB currently creates one complete SQLite schema from
`src/sqlite_schema.cpp`. There is no SQL schema-version counter, historical
upgrade sequence, or migration/backfill support. Discard disposable development
DB files when the schema changes. Existing nonempty databases are validated,
not automatically rewritten.

The V1/V2 *revision hash formats* mentioned below identify immutable
cryptographic encodings, not SQL schema versions. They remain necessary for
verifying finalized engineering history.

# Persistence architecture and contracts (v5 typed hydraulic persistence)

## Stable vocabulary

Project -> Branch -> Revision. All three carry UUIDv7s allocated through the
AOWIS-SERVER-MODEL function `createUuidV7()`. UUIDs are represented as 16-byte
RFC 4122 BLOBs in SQLite, UUID values in C++, and canonical text at future
REST boundaries. Revision numbers are only local to a branch, never identities.

A project's history is a directed graph of finalized revision snapshots.
Branch names are unique within each project, but a branch can start from a
finalized revision owned by another branch of the same project.

## Working revision concurrency

Transactions use `BEGIN IMMEDIATE` so a finalization sees a consistent head,
generation, snapshot, and parent. Every working revision has a generation
counter. A mutation must give the generation it read; a mismatch means a
conflict. Legacy snapshot operations use the revision-wide generation, whereas V4
object-mode mutations use per-object optimistic versions (while still
advancing the global revision generation for finalization).

SQLite `foreign_keys=ON` and SQL triggers protect finalized revision rows and
snapshots. Do not grant write access to arbitrary SQL clients expecting these
to constitute a privilege boundary: administrators can remove the triggers.

## Hash formats V1 and V2

The exact persisted snapshot bytes have SHA-256 digest `D`.

The V1 chain hash is SHA-256 over these bytes, concatenated in this exact order:

1. ASCII `AOWIS-REVISION-DAG-V1` followed by a NUL byte (22 bytes total).
2. Project UUID, branch UUID, revision UUID (16 bytes each).
3. Revision number, unsigned 64-bit big-endian.
4. Finalization timestamp, Unix milliseconds, unsigned 64-bit big-endian.
5. Title as UTF-8 with unsigned 64-bit big-endian byte length.
6. Commit message as UTF-8 with the same length prefix.
7. Snapshot digest D (32 bytes).
8. Parent count, unsigned 64-bit big-endian.
9. For each parent in `parent_position` order: parent UUID (16 bytes) and
   that finalized parent's chain hash (32 bytes). No parents = count zero.

V2 uses the same encoding with two changes: the prefix becomes ASCII
`AOWIS-REVISION-DAG-V2` plus NUL (22 bytes), and the 16-byte creating actor UUID
and the 16-byte finalizing actor UUID, followed by the unsigned 64-bit
big-endian revision creation timestamp, are inserted immediately after the
revision UUID. Newly finalized revisions use V2; original finalized V1 revisions continue
to verify unchanged across reopen and long-lived project history. A legacy working revision
has unknown (null UUID) creator but gets a real finalizer UUID.

The schema permits multiple ordered parents, so future merge commits need no
ancestry schema migration. Neither encoding may be changed retroactively.

Snapshot storage has a format_version column, initially 1. Its contents are
currently opaque; this is **not yet a canonical AOWIS Model format**. Before
production model persistence, define a complete canonical serializer including
all project metadata, entities, graph relationships, units, attachment bytes,
array ordering, floating-point representation, and canonicalization rules.
Hash only the exact persisted representation, not a generated JSON dump or
SQLite database pages. Copy immutable bytes into every newly-created working
revision until copy-on-write object references become available.

## Evolution boundaries

- `aowis-db-core` owns lifecycle, transactions, validation, and hash checking.
- The future `aowis-db-http` exposes the same core functions, not new rules.
- SQLite and PostgreSQL must uphold the same constraints.
- REST clients and embedded clients must receive identical conflict behavior.
- Migrations must be transactional and never re-hash finalized snapshots.
- Work toward revision-object references for incremental edits and immutable,
  shared model objects, rather than repeatedly replacing huge snapshots.
- Schema-v2 user/group/role/permission identities and DB-core authorization are
  documented in [identity-and-access.md](identity-and-access.md).
- HTTP authentication and user session/credential management are not implemented.
- Full edit-operation and security audit histories come after this foundation.

## SQLite operation history

Migration V2 -> V3 adds `operation_events` and `revisions.operation_history_sha256`.
Every accepted snapshot write creates a UUIDv7 event with actor UUID, revision,
branch, project, monotonic event sequence (= revision edit generation), UTC
milliseconds, and complete before/after snapshots. `undo` and `redo` are
**compensating events** referencing a prior `replace` event; no event is deleted.
All three actions commit the state and event atomically under `BEGIN IMMEDIATE`
and reject stale expected generations.

The undo/redo stack is reconstructed from append-only events. An ordinary
replacement clears redo candidates; an undo/redo must exactly match the
recorded snapshot before it is performed. Events are always scoped to a
single working revision. Finalizing a revision seals the operation journal
and starts the next working revision with a fresh journal. Old finalized
revisions remain accessible via `historyForRevision`.

V3 stores a SHA-256 digest over its exact journal encoding: literal ASCII
`AOWIS-OPERATIONS-V1` followed by NUL; unsigned 64-bit event count; then
for each event in increasing sequence: event UUID, actor UUID, revision UUID,
unsigned 64-bit sequence and Unix millisecond timestamp, length-prefixed UTF-8
operation kind, target UUID or 16 zero bytes, one byte (0/1) to indicate
before-snapshot presence (and length-prefixed bytes if present), and similarly
one byte plus optional length-prefixed after-snapshot bytes.

Finalization saves that digest in `revisions.operation_history_sha256`, and
verification recalculates and checks it. **This additional journal digest is
not yet part of the historical V2 revision chain.** A later V3 chain encoding
will bind the two hashes together; never reinterpret V1 or V2 hashes or
retroactively assign fictitious edits to old revisions.

At present, entire opaque snapshots are copied for each event. This is a
correctness-first foundation, not yet suitable for large hydraulic models or
fine-grained simultaneous editing. Later replace this with per-entity change
sets, durable grouped transactions, Model-specific inverse commands, and an
independent security audit trail.

## SQLite model-object persistence

Migration V3 -> V4 adds `revisions.object_mode`, `revision_objects` and an
append-only `object_events` journal. Existing databases remain in legacy
snapshot mode (0), preserving V1 and V2 revision hashes byte-for-byte.

A fresh working revision with no legacy snapshot/history can switch to object
mode (1) at its first object write. The two formats cannot be mixed within
one revision. In object mode, each model object has its own immutable UUID,
kind identifier, bytes, per-object version and deletion tombstone. Edits use
`expected_version` (`-1` means never seen before); separate objects can be
updated against their own versions even when the branch generation changed.
The branch generation still advances for each successful edit and controls
finalization. Changes and journal entries are committed atomically. Historical
revisions cannot be modified by ordinary SQL without disabling triggers.

The `putObject` API stores externally provided **opaque canonical bytes**.
It does NOT yet offer a complete round-trip serializer for the structures in
AOWIS-SERVER-MODEL. Model validation, reference integrity, and per-entity
serialization remain required before production project storage.

For object-mode revisions, `snapshot()` provides a deterministic manifest:
`AOWIS-OBJECT-MANIFEST-V1` + NUL; SHA-256 of the operation journal; big-endian
64-bit number of object records (including deletion tombstones); then each
record sorted by its 16-byte UUID: UUID, length-prefixed UTF-8 kind, unsigned
64-bit version, deletion marker (0 or 1), and length-prefixed bytes only for
non-deleted records. The journal hash covers `AOWIS-OBJECT-JOURNAL-V1` + NUL; big-endian
64-bit journal-entry count; then for each entry ordered by revision-wide
sequence: event UUID, object UUID, actor UUID, unsigned big-endian sequence
and Unix-ms timestamp, length-prefixed kind and operation, unsigned
before_version+1, unsigned after_version, before-deleted marker, before-bytes
presence marker and optional length-prefixed before bytes, after-bytes
presence marker and optional length-prefixed after bytes.

Finalization persists the manifest once and its SHA-256 is fed into the
existing V2 revision chain. A verifier regenerates the manifest from the
persisted object rows and journal, compares it against both the stored
snapshot and finalized digest, then checks the previous revision hashes.
The existing V3 whole-snapshot history is unaffected. On finalization or
branch creation, object rows are copied into the successor's working state;
its operation journal starts empty. Large-scale copy-on-write sharing and
actual `NetworkHydraulic` field-level serializers are later optimizations.

**Important limitation:** object mutations have a durable history but not yet
reversible object-level undo/redo. Existing whole-snapshot undo/redo remains
available only for legacy snapshot-mode revisions. A subsequent stage must
add atomic model-aware operation groups and object-history undo/redo before
this foundation replaces GUI editing.

## Typed model persistence: V5

See [model-persistence.md](model-persistence.md). V5 reuses the existing V4
object store and V2 finalized revision chain. No database-schema migration or
historical revision re-hashing is required.

## Schema V5: grouped object journal (V6 feature)

Object groups associate one or more ordered immutable `object_events` with
one intent transaction. `revisions.object_journal_version` keeps all finalized
V4 object manifests byte-for-byte unchanged (default 1) while version 2
appends a deterministic group-history encoding. Newly finalized grouped edits
therefore bind authorship, undo/redo targeting, membership order and contents
into their SHA-256 snapshot digest and revision DAG hash. Historical hashes
are never modified. Undo and redo are append-only compensating actions with
optimistic concurrency checks. See [object-history-v6.md](object-history-v6.md).
