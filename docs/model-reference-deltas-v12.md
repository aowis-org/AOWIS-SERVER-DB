# V12 — Delta-maintained hydraulic reverse references

## Change

The existing reverse-reference index (`hydraulic_network_references`, SQLite
schema V7) is **derived metadata**, not part of the finalized model hash.
Previously, every changed full-network save, and every validated grouped
structural edit, deleted every row for the affected network and inserted the
entire reference set again. A scalar edit could therefore cause substantial
SQLite index churn even though no relationship changed.

`Database::replaceHydraulicReferences()` now compares the persisted reference
set to `hydraulicReferences(network)` and applies only the set difference.
Each key uniquely combines the fixed-width source/target UUIDs and the field
path. Duplicate references are intentionally represented once, consistent with
the existing SQL primary key. No rows are updated when the sets are identical.
On a structural change, unchanged rows remain in place; obsolete references
are deleted and new ones inserted. The existing target UUID lookup index is
maintained by SQLite. A failure at any step rolls back the enclosing object
transaction, including its edit journal, object data, and reference changes.

The same implementation serves full-network writes and the existing grouped
undo/redo guard. Migration V6→V7 also still uses it for initial backfill.
There is **no schema migration**, serialization-format change, hash change, or
change to the public C++ API.

## What this does not optimize

The diff still traverses the affected complete network to collect references
and reads that network's existing reference rows. The improvement is the
elimination of unnecessary index writes, not O(1) edit validation or automatic
semantic conflict resolution. The conservative full-network validation fallback
for structural/grouped edits remains.

## Regression coverage

`aowis-db-model-reference-deltas` uses temporary SQL triggers to prove that
scalar full-network saves, grouped edits, undo, and redo produce *no* reference
row writes when the graph is unchanged. A targeted trigger protects an
unchanged edge while a real pipe-endpoint/demand-point reassignment is saved,
ensuring a true delta rather than a conditional full rebuild. An injected
insert failure must atomically roll back both the graph and its history.
Finalization, child revision inheritance, reopening, and revision-chain
integrity are also exercised.
