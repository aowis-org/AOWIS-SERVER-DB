# V16 — Dependency-local hydraulic link resource validation

V15 validated pump and valve endpoint changes against the owning network's
node index, but sent changes to their curves and patterns through full-network
reconstruction. V16 extends the bounded transaction-local path to all
explicit reference fields of existing hydraulic links:

| Entity | Resource field | Required target kind |
| --- | --- | --- |
| Pipe | `material_uuid` | `hydraulic.pipe_material` |
| Pump | `head_curve_uuid` | `hydraulic.curve_pump_head` |
| Pump | `speed_pattern_uuid` | `hydraulic.pattern_time` |
| Pump | `efficiency_curve_uuid` | `hydraulic.curve_pump_efficiency` |
| Pump | `price_pattern_uuid` | `hydraulic.pattern_time` |
| Valve | `head_loss_curve_uuid` | `hydraulic.curve_valve_headloss` |
| Valve | `characteristic_curve_uuid` | `hydraulic.curve_valve_characteristic` |

Link endpoints still require a junction, reservoir or tank in the same network.
Null references remain valid for incomplete drafts, matching the full Model
validator. Every non-null reference is checked through the indexed membership
and revision object tables; a valid object UUID in a *different* network is
rejected. On success only changed reference rows are updated by the existing
transactional delta writer.

The local path is available only to existing, non-deleted links with identical
entity UUIDs, whose owning network is indexed and whose prior bytes are
available from the journal. It remains limited to small batches. Pipe endpoint
changes still revalidate their indexed, possibly untouched demand-point
attachments; pure material changes do not need this dependent-point scan. Invalid edits roll back state, journal, generation and index
updates. Undo and redo run through the same checks.

Insertions, deletions, network-root modifications, unknown model objects,
other structural changes, and unsupported groups continue to fall back to
full-network validation. This patch changes neither the SQLite schema nor the
hash format, and preserves the established V1–V7 migration chain.

The `aowis-db-model-local-resources` test checks accepted material/curve/pattern
changes, wrong-type, missing and cross-network resource references, grouped
edits, undo/redo, index consistency, transaction rollback, finalized integrity
and persistence across reopening the SQLite file.
