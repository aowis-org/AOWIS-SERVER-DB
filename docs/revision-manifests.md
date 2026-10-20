# Canonical finalized revision manifests

`AOWIS-REVISION-MANIFEST-V1` is a read-only, backend-independent binary
representation of **one finalized revision's metadata**. It is not an SQL
backup, a repository clone, or proof that the receiving repository has the
parent revisions, model content, journal events, or authorization grants.

`Database::exportRevisionManifest(actor, revision_uuid)` requires
`project.read` on the originating branch and rejects working revisions.
`decodeRevisionManifest()` validates the bytes in isolation; it does not
trust or import them into any database.

## Exact binary layout

All UUIDs are exactly 16 RFC-4122 bytes. All integral fields are unsigned
64-bit big-endian, restricted to the nonnegative signed-64-bit range for
revision number and timestamps. Text is length-prefixed strict UTF-8
(`u64 byte_length`, then exactly that many bytes), with no normalization.
No SQL-native types, JSON reserialization, host byte order or local paths
enter this representation.

| Order | Field | Representation |
| --- | --- | --- |
| 1 | Version magic | ASCII `AOWIS-REVISION-MANIFEST-V1` followed by NUL |
| 2 | Object mode | byte `0` (opaque) or `1` (Merkle model) |
| 3 | Project, branch, revision UUID | three 16-byte UUIDs |
| 4 | Creator and finalizer UUID | two 16-byte UUIDs (zero UUID represents absent identity in existing chain contract) |
| 5 | Created time, branch-local revision number, finalized time | three `u64` |
| 6 | Title and message | two length-prefixed strict UTF-8 fields |
| 7 | Snapshot SHA-256, operation-history SHA-256 | two 32-byte digests |
| 8 | Model Merkle root | 32 bytes; **all zero** for opaque snapshots |
| 9 | Revision chain SHA-256 | 32 bytes |
| 10 | Ordered parent count | one `u64` |
| 11 | Each parent in its original DAG order | 16-byte UUID and 32-byte parent revision-chain SHA-256 |

The codec refuses duplicate parents, self-parents, malformed lengths, trailing
bytes, invalid UTF-8, invalid mode, oversized fields and frames, and any
revision-chain digest mismatch. The current safety limits are 16 MiB per
frame, 8 MiB per text field and at most 64 parents. This is a bounded
metadata-only format, not a container for model data.

## Integrity contract

The `chain_sha256` is **not** a SHA-256 of the transport frame. It is the
unchanged `AOWIS-REVISION-DAG-V1` digest computed over project/branch/revision
identities, authors, timestamps, title/message, snapshot digest, operation
history digest, Merkle-root digest (or 32 zero bytes for opaque mode), and
ordered `(parent UUID, parent chain digest)` pairs. Local finalization and
project verification use this same digest function.

A valid manifest proves internal metadata/hash consistency only, **not**
content availability or repository trust. A future import must separately
verify: original parent revisions and their exact chain hashes; the snapshot
and immutable object content; operation-history/group journal; project,
branch and author provenance; hydraulic constraints; and receiving-user
permissions before atomically publishing any branch reference. No imported
UUID automatically confers local permissions.

## Verified model content inventory (Stage 5.3a)

`Database::exportRevisionContentInventory(actor, revision_uuid)` is a
read-only `project.read`-protected operation for **finalized object-mode**
revisions. It verifies the canonical manifest and all reachable Merkle tree
nodes and model-record blobs, returning sorted unique content-store SHA-256
identities (`tree_sha256` and `blob_sha256`) alongside the revision UUID and
root hash. Duplicate object payloads occupy just one inventory entry. Both
SQLite and PostgreSQL use the same hash identities. Missing or altered
immutable content causes failure.

The inventory is an intermediate dependency-discovery API, **not** a clone
bundle, wire format, completeness proof for full history, or importer. It
does not include parent revisions, operation journals, history groups,
opaque snapshot bytes, remote trust, or destination authorization. A future
replication protocol must transfer and verify those separately, paginate
large inventories, and atomically publish validated history. The current
method materializes its list of hashes in memory but verifies model blobs
in bounded batches.

## Portable operation-journal and parent dependencies (Stage 5.3b)

`Database::exportRevisionOperationJournal(actor, revision_uuid)` exports
**the exact byte preimage** of the existing `operation_history_sha256`, in
`AOWIS-OPERATIONS-V1` format. The same encoder is used for local finalization,
full verification and export: no alternative serialization or revised chain
hash has been introduced. Each entry includes its event, actor and revision
UUID, sequence, timestamp, kind, target event UUID, and nullable before/after
snapshot payloads. The framing is `AOWIS-OPERATIONS-V1` + NUL, big-endian
`u64` count, and the original ordered entries; the entry layout is the existing
history digest input, not an independent new wire version. Export is
read-only and requires **both** `project.read` and `project.history.read`,
because previous snapshots may contain sensitive historical data.

`verifyRevisionOperationJournal(frame, revision_uuid, expected_sha256)` checks
the committed SHA-256 and strict field framing, including revision ownership,
UUID uniqueness, event order, flags, UTF-8, truncation and trailing data. It
limits a standalone transport frame to 256 MiB; larger histories will require
future chunking. This check does not authenticate who supplied the manifest.
It does not replay the operation journal or prove that its before/after images
match an imported snapshot; those checks belong to the future importer.

`exportRevisionParentManifests()` exports the **immediate** parent manifests
in the original DAG order, rechecks their project and recorded chain hashes,
and independently enforces `project.read` on each parent branch. No missing
parent is silently omitted. Replication must walk the DAG recursively to
obtain a complete ancestry closure.

**Still outstanding:** the separate object-event/group journal referenced by
`AOWIS-REVISION-HISTORY-V1`, finalized opaque snapshot bytes, and a portable
container/transfer protocol. This stage does not yet provide a complete
clone bundle or apply any remote writes.
