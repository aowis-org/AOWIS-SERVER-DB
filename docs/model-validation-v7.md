# Hydraulic model reference validation (V7)

## Contract

`aowis::db::validateHydraulicNetwork(network)` provides a structured
`ModelValidationReport` (`issues`, `valid()`, and `summary()`). It is independent
of SQLite, authentication, and project state. The full-network
`Database::saveHydraulicNetwork` operation now calls it **before** beginning a
write transaction, rejecting invalid references without changing objects,
revision generation, or history.

Validation checks **non-null references** against the correct UUID namespace:

- Pipe, pump and valve endpoints must refer to hydraulic nodes.
- Pipe materials, pump curves/efficiency curves, valve curves, reservoir head
  patterns, tank volume curves, demand patterns, quality-source patterns,
  pump speed/price patterns and global patterns must resolve to the right type.
- Demand point attachments must refer to the right pipe/junction. Attached
  pipe positions must be finite and in the range `[0,1]`. When using
  `AssignedJunction`, an explicit assigned junction must be a pipe endpoint.
- Simple and rule controls must refer to existing node/link UUIDs; rule
  premises against `System` do not require a node/link object reference.
- Map label anchor nodes and explicitly selected report node/link IDs must
  belong to the network.
- MSX reaction species, parameter overrides, initial-quality and source
  references are checked against appropriate species, parameter, pipe, tank,
  node and MSX pattern UUID sets. Duplicate/null MSX species, parameter and
  pattern identities are reported.

Null references are intentionally allowed for **unfinished draft entities**,
including links without endpoints, provided other relationships remain
consistent. For example, a pipe-attached demand point using `AssignedJunction`
may keep its assigned junction only while that junction remains an endpoint of
the pipe. Removing that endpoint must also update or clear the assignment.
This is a *reference consistency* check,
not a hydraulic solver readiness test. Invalid numeric hydraulic parameters,
invalid simulation configurations, external attachments, expression parsing,
all nested MSX identity uniqueness and solver feasibility are not validated
here.

## Limitations and follow-up

The *whole-network save* is reference-checked atomically. Low-level `putObject`,
`commitObjectGroup`, grouped undo/redo, and the existing direct
`putHydraulicEntity` convenience method still operate on individual serialized
objects without a model-level transaction validator. Those methods may
introduce dangling references; do not expose them as untrusted unrestricted
model-edit endpoints until we add a shared **transaction-level model validator**
that checks the resulting revision state before commit.

Historical snapshots and SHA-256 formats are unchanged. The validator does not
rewrite existing finalized revisions and no SQL migration is needed.
