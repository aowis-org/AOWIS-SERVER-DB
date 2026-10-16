# V8 — Transactional validation of indexed hydraulic objects

After the V7 full-network save validation, low-level edits could modify entity
bytes or delete a junction referenced by an existing network. V8 checks the
resulting hydraulic graph **inside the same SQLite write transaction** for:

- `putObject` / `deleteObject` and the typed `putHydraulicEntity` wrapper;
- `commitObjectGroup` (checks the final group state, not intermediate writes);
- `undoObjectGroup` / `redoObjectGroup` (same checks after compensation).

A change to an object referenced by a `hydraulic.network` root causes its full
network to be reconstructed from the pending transactional database state and
validated with the V7 reference validator. Missing or corrupted serialized
entities and dangling or wrong-type references cause the transaction to roll
back, including the object event journal and generation counter. New root
indexes may not refer to objects already owned by other network roots.

Root-index changes cannot silently orphan live old members. Deleting a whole
network root is supported only when **all** its indexed entity objects have also
been deleted in the same transaction or already tombstoned; this permits undo
of an initial `saveHydraulicNetwork` group, and its later redo.

Generic/unindexed low-level objects are intentionally still supported; they do
not have a typed network context to validate. The raw object API should not be
used as a substitute for `saveHydraulicNetwork` when adding/removing network
entities or to claim complete type validation for unattached payloads.

This requires no schema or snapshot hash migration. Finalized revision content
remains byte-identical. Validation may reconstruct a whole affected network;
a future indexed membership table could improve performance for large projects.
