# Revision history, edit journal and merges

## Revisions and ancestry

A project may have multiple branches. Each branch normally has one working
revision; older finalized revisions are immutable. A finalized revision has
an ordered list of parent revisions, so a merge commit can carry two parents.
The revision graph is acyclic and cannot cross project boundaries.

A finalized snapshot is verified with a SHA-256 content digest and a
cryptographic chain digest including immutable metadata and the hashes of its
parents. The original historical chain encoding and its actor-attributed
successor remain readable. Keep their format identifiers and byte layout
unchanged, including `AOWIS-REVISION-DAG-V1` and `AOWIS-REVISION-DAG-V2`.
These are stored compatibility contracts, not feature-stage names.

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
