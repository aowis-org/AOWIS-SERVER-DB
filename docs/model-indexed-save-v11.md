# V11 — Scoped full-network saves

## Problem addressed

The full-network synchronization operation previously selected every
`revision_objects` row in the working revision and decoded every unrelated
`hydraulic.network` root to check for conflicting ownership. In a multi-project,
multi-network database, one network edit therefore loaded data from all other
networks in the same revision. It also deleted and recreated the entire
`hydraulic_network_members` index for an ordinary scalar edit.

## Implementation

`saveHydraulicNetwork()` now operates inside the same `BEGIN IMMEDIATE`
transaction as before, with the same authorization and revision-generation
checks, using the existing V6/V7 indexes:

1. Read only the requested network root by `(revision_uuid, object_uuid)` to
   obtain its previous entity UUID list.
2. Construct the UUID set in the supplied, already-validated network model.
3. Check indexed ownership of each incoming UUID through
   `hydraulic_network_members` (`PRIMARY KEY(revision_uuid, entity_uuid)`),
   including the proposed root UUID. An entity owned by another network is
   rejected without decoding that network.
4. Read only incoming and previously-owned objects from `revision_objects`
   using `(revision_uuid, object_uuid)` primary-key lookups, in batches of at
   most 300 IDs (compatible with older SQLite parameter limits).
5. Compare the requested serialized bytes and versions; write only changed
   objects and tombstones for removed members.
6. Rewrite `hydraulic_network_members` only if the membership **set** changed.
   Reordering the root's entity lists or changing scalar attributes cannot
   force a membership-index rebuild.
7. Maintain the reverse-reference index and append the same atomic operation
   group as before, only when something actually changed.

Unrelated generic objects still reserve their UUIDs: a network cannot claim a
UUID already present in `revision_objects` even if that record is not part of a
hydraulic network. Similarly, tombstones remain reserved and must satisfy the
same version/collision rules as before.

**This does not change the schema, serialized Model format, hash chain, or
finalized revision data.** Existing V7 indexes are derived and remain subject
to the same transaction-level guarantees.

## Remaining scaling costs

The public `saveHydraulicNetwork()` API accepts the complete in-memory
`NetworkHydraulic`, so serialization and semantic validation necessarily
visit its entities. Each changed entity still creates an append-only history
event, and the reverse-reference table is still rebuilt for the affected
network following a changed full-network save. Future optimization should
compare reference sets or apply reference deltas and introduce an efficient
incremental synchronization interface for changes that originate as individual
entity edits.

## Regression coverage

`aowis-db-model-save-indexed` verifies large-network batching, no-op saves,
scalar saves without membership rewrites, removal tombstones, other-network
isolation, UUID ownership/collision rejection, primary-key-backed SQLite query
plans, finalization, and database reopen with valid SHA-256 history.
