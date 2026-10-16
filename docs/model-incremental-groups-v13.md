# V13 — Incremental validation for reference-neutral object groups

## Scope

The existing V10 shortcut avoids reloading an entire hydraulic network for
certain single-object pipe/junction scalar updates. V13 extends this to any
number of **already-indexed, existing entities** edited in the same transaction,
including undo/redo, when their validation-sensitive fields remain unchanged.
Different networks in one group are supported; no project-wide scan is needed.

After the transaction has written its object rows and immutable events, the
guard loads only each touched object's current bytes and its latest event's
`before_content`. Typed decoders must recognize both values and confirm the
original entity UUID. `hydraulicReferences()` runs on a one-entity *partial*
model strictly to collect outgoing references: it is never used to decide
whether a referenced UUID exists. For the shortcut, the ordered outgoing
reference triples (source, target, field) must be identical before and after.
The reference extractor is shared with the full validation/index-maintenance
code to avoid an independently maintained list of UUID-reference fields.

Demand points additionally require unchanged attachment type and allocation
mode. A changed pipe position must be finite and within [0,1]. Their assigned
junction validity also depends on pipe endpoints; any endpoint modification
changes its outgoing references, forcing full validation. Control rules are
currently excluded because a premise's Node-vs-Link discriminator changes the
expected target type even if its referenced UUID remains the same.

## Strict fallback

The fast path is capped at 32 touched entities; larger batches use the
existing bulk reconstruction path to avoid per-object query overhead.

The existing full-network validator still runs for:

- Network-root edits and membership/list changes.
- Insertions, deletions, changed UUID references, and malformed payloads.
- Unknown object kinds and rule-premise edits.
- Any unindexed touched object in a group, or any uncertain validation case.

Both the shortcut and fallback run inside the same `BEGIN IMMEDIATE` transaction,
so failure rolls back object rows, revision generation, and the immutable journal.
No new database tables, migration, revision hash formats, or public APIs are
needed. The existing V12 reference-index delta updater remains responsible for
structural edits.

This is deliberately an optimization for **reference-neutral groups**, not yet
a general localized structural validator. Truly changed references still
reconstruct and validate the affected network. Such an optimization would need
typed target-category checks and explicit dependency validation for demand-point
pipe-to-junction relationships, rule premises, and MSX constraints.

## Test

`aowis-db-model-incremental-groups` checks grouped scalar edits to pipes,
junctions, and demand points; undo/redo; stale generations; out-of-range pipe
attachment positions; a structurally invalid endpoint change; the corresponding
valid coordinated endpoint/attachment change; rejection of malformed bytes;
and finalized integrity verification.
