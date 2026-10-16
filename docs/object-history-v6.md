# V6: grouped object operations and persistent undo/redo

SQLite schema version 5 (separate from the V5 Model codec version) adds
`object_groups`, `object_group_events`, and `revisions.object_journal_version`.
All IDs are UUIDv7 from AOWIS-SERVER-MODEL. Historical finalized V1/V2
revision hashes and V4 manifests are left byte-identical.

## Transaction boundary

`commitObjectGroup(actor,branch,expected_generation,edits)` applies an ordered
batch in one `BEGIN IMMEDIATE` transaction. The expected **branch revision
generation** must match, and every edit includes its expected **object version**.
Duplicate UUIDs, stale versions, type collisions, empty batches, or failures
at any position abort the *entire* batch. Every committed object change has an
immutable `object_events` record, which is linked to exactly one new `object_groups`
record by an ordered `object_group_events` entry. The returned UUID identifies
the overall user action. Creation and removal are represented by a live row
and deletion tombstone, respectively; deleted UUIDs are never silently reused.

Existing `putObject` and `deleteObject` calls form one-element groups and
retain their object-level expected-version contract. The typed
`saveHydraulicNetwork` operation forms a single group over all changed
entities, root indexes and deletions, or emits no group if nothing changed.
This keeps the V5 API stable while making all *new* object changes undoable.

## Undo and redo

`undoObjectGroup(actor,branch,expected_generation)` and `redoObjectGroup(...)`
operate on **the latest applicable group in the branch**, not independent
per-user histories. Both require model-write and history-read permissions.
They append a fresh, immutable group with action `undo`/`redo` and references
back to the UUID of the original `edit` group. The public result is that
original group UUID; it is not the compensating action UUID. Undo reverses
member events in reverse order; redo reapplies them in forward order.
Every change is checked against the live object state inside the same SQLite
write transaction. If state has diverged, the operation fails without
changing anything. New forward edits after an undo discard the redo stack
**logically**; they never delete historical redo events. Undo/redo can be
resumed after reopening the database.

Object versions keep increasing after an undo/redo, including when undoing a
creation leaves a tombstone. This is intentional for optimistic concurrency.
Calls require the expected revision generation: a competing writer cannot
silently change the undo/redo baseline. A UI should refresh its state on a
conflict, and never invent an inverse operation without validation.

Older V4 events already committed before this migration remain accessible in
`objectHistoryForRevision`, but they cannot be undone through the V6 grouped
API because they do not contain group identities. Only new V6 edits and their
compensations are eligible for grouped undo/redo.

## Integrity and compatibility

V5 `object_journal_version=1` exactly retains the existing V4 manifest. The
first new group sets this to 2 on the editable revision; the finalized
manifest then includes a deterministic, domain-separated group record with
ordered group UUIDs, actors, kinds, target IDs, timestamps, event UUIDs, and
member positions. That manifest is hashed by the existing V2 revision chain.
Finalized group metadata and membership are additionally protected by SQLite
immutability triggers. The next editable revision starts with a clean journal
while retaining the copied current object state. Branches begin with a clean
journal and their own UUIDs. Old finalized snapshot hashes require no rewrite.

The same core library is used by embedded and future HTTP clients. The
caller-provided `OperationContext` is trusted only at the embedded boundary;
HTTP must later bind it to authenticated credentials.

## Limitations

- Changes are reversible at the stored object-byte level. Semantic/model graph
  validation and automatic resolution of dependent edits are **not** implemented.
- In this stage the undo stack is branch-wide LIFO, not per-user selective undo.
- No automatic conflict merging: stale generation, stale object versions,
  or mismatched expected data result in failure.
- Existing pre-V6 ungrouped object changes cannot be converted reliably into
  user-intent transactions.
- The group journal is not a standalone system-administration audit log.
- External file contents are still not stored as versioned attachments.
