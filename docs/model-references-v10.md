# V10: derived reverse-reference index and conservative incremental validation

Database schema **7** adds `hydraulic_network_references` with an indexed
`(revision_uuid, target_uuid)` lookup. Each row identifies one source UUID,
referenced target UUID, hydraulic network UUID and field path. Root-level
configuration and nested MSX references use the network UUID as their source.
The index is maintained using the **same reference traversal** as the V7
model validator, so new reference checks also generate reverse-reference rows.

## Migration, immutability and isolation

- The V6 → V7 migration backfills all active and finalized hydraulic networks.
- Writes to a finalized revision's reference index are prohibited after backfill.
- Working revision creation and branch creation copy the index with object data.
- Validated network saves and structural edits replace affected networks' index
  rows within the same transaction as object changes and history records.
- Deleted network roots have their reference rows removed with membership rows.
- All index rows are scoped by revision UUID and owning network UUID.
- The index is **derived metadata**: no finalized snapshot, operation history or
  SHA-256 chain is rehashed during migration.

## Validation fast path

For a **single existing pipe or junction update**, the guard reads the immutable
latest object event's before bytes and compares all fields the reference
validator inspects for that entity. It may skip full-network deserialization
only if the entity UUID and all relevant references are unchanged. This speeds
up common edits such as pipe diameter, roughness or junction elevation.

Root edits, deletes, new entities, changed references, multi-object groups, and
other object types use the existing complete hydraulic network reconstruction
and validation. Failed edits still roll back their object rows and journals.
This is deliberately conservative: it does **not** claim that arbitrary field
changes can always be incrementally validated, and it does not replace solver
feasibility checks or detect direct privileged SQL tampering.

## Further optimization

The new target-keyed index enables dependent-object discovery without decoding
all objects. Future work can use these edges for fully incremental reference
validation, add strict schema-consistency checks, and optimize full-network
save comparisons (which still walk all revision objects).
