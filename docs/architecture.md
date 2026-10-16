# Persistence architecture and contracts (v2 identity foundation)

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
conflict. This is **snapshot-wide** locking for the foundation. Later,
per-object optimistic concurrency will allow unrelated edits in the same
working revision to coexist without conflicts.

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
to verify unchanged, including after schema migration. A legacy working revision
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
