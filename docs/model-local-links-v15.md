# V15 — Dependency-local pump and valve endpoint validation

Extend the bounded V14 structural path to **existing** hydraulic pumps and
valves. The path uses the membership index for their owning network and
checks each new endpoint against an indexed node entity (junction, tank, or
reservoir) in that *same* network. Null endpoints remain legal for incomplete
engineering drafts, as in the full Model reference validator.

This fast path only applies if the source object UUID and type remain stable
and all its other outgoing references (pump curves, efficiency/price/speed
patterns, valve characteristic/headloss curves) stay **identical**. Such
changes still use full-network validation. In particular, the fast path never
assumes a newly specified pattern/curve is valid simply because the endpoints
are valid.

Pump and valve endpoint changes have no additional incoming model dependency:
controls reference the unchanged link UUID, and demand-point pipe attachments
refer only to `hydraulic.pipe`. The V14 pipe and demand-point dependency checks
continue to run for pipe endpoint changes and coordinated attachment edits.

Within one write transaction, the V12 reference delta writer synchronizes
only the touched links' changed reference rows. A failed validation rolls back
the object data, immutable journal records, revision generation, and derived
references. Undo and redo use the same path. Large batches, insertions,
deletions, changed root indices, unknown kinds, and other structural edits
continue through the full validator. No database migration or change to any
finalized hash is necessary.

`aowis-db-model-local-links` tests accepted endpoint edits, link-vs-node,
cross-network and dangling target rejection, unchanged curve references, unsupported changed
curve references (full-validator fallback), grouped edits, undo/redo, final
model reconstruction, and integrity verification after restart.
