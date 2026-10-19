# Revision history, edit journal and merges

## Revisions and ancestry

A project may have multiple branches. Each branch normally has one working
revision; older finalized revisions are immutable. A finalized revision has
an ordered list of parent revisions, so a merge commit can carry two parents.
The revision graph is acyclic and cannot cross project boundaries.

A finalized snapshot is verified with a SHA-256 content digest and a
cryptographic chain digest covering immutable metadata, the complete
operation-history digest, and the hashes of its ordered parents.

The current pre-release chain input is SHA-256 of, in order:

1. ASCII `AOWIS-REVISION-DAG-V1` plus a trailing NUL (22 bytes).
2. Project, branch, revision, creator and finalizer UUIDs (five RFC 4122
   16-byte values).
3. Creation time, revision number, finalization time (unsigned 64-bit,
   big-endian; times are Unix milliseconds).
4. Title and message as UTF-8, each prefixed with an unsigned 64-bit,
   big-endian byte length.
5. Snapshot SHA-256 (32 bytes), operation-history SHA-256 (32 bytes),
   and 32-byte model-object Merkle root (all zeros for opaque-snapshot mode).
6. Ordered parent count (unsigned 64-bit big-endian), then parent UUID and
   chain SHA-256 for each `parent_position` in ascending order.

There is only one accepted revision-chain encoding; old pre-release encodings
are deliberately unsupported. The operation-history digest is also
recalculated independently during verification. For object-mode revisions,
verification traverses the sealed Merkle root and compares every immutable
content blob with the materialized SQL object rows. Changing a journal entry or
its stored seal cannot go undetected without recomputing the chain hash.

**Pre-release:** Schema and hash encoding changes may require recreating
throwaway development databases. We do not support automatic migrations or
compatibility with previous development-only hashes until the first release.

## Journal and undo/redo

Working revisions record append-only operations, including the acting user,
affected model-object UUID, original and resulting data, timestamps and order.
Related modifications can form one atomic edit group. Persistent undo and redo
append compensating operations and preserve the original events for audit and
documentation. Concurrent edits are checked against object versions and
structural dependencies. An operation that would overwrite a newer edit must
fail instead of silently undoing someone else's work.

Finalization seals the revision's contents and journal in the stored integrity
record. Correcting a finalized design means committing a new change in a newer
working revision, not editing the old revision or truncating history.

## Branch comparison and merging

The merge engine compares common ancestry and changes on the source and target
branches. It supports preview, field-level comparison, explicit conflict
resolution for supported entity fields, structured metadata, and selected
append-only tag changes. Successful merge commits are atomic and refer to both
parents; failed/conflicting merges leave the working state unchanged.

Known limitations include restricted merge-base handling and cases where two
structurally related hydraulic edits conflict even when they touch distinct
entities. A conflict preview is not a license to skip model validation.
More elaborate recursive merges and offline synchronization are not complete.

REST exposes read-only compare/preview and authenticated merge-commit routes;
see [HTTP API](http-api.md). Security administration is recorded in a separate
audit history; see [Identity and access](identity-and-access.md).
