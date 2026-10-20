# Distributed repositories — architectural direction

**Status: planned; no repository synchronization protocol is implemented.**
Distribution is a primary product requirement for the AOWIS **Distributed
Versioned Model Database**, not a synonym for exposing the database over HTTP.

## Intended user workflow

An engineer should be able to clone an AOWIS project into an independent
SQLite-backed local repository, make and finalize model revisions without a
network connection, and later fetch or push changes to a PostgreSQL-backed
server repository. Other authorized repositories may independently hold and
exchange copies of that project's history. Every replica can have divergent
branches; none may overwrite another's work silently.

The server remains important for shared access, team authorization and
coordination, but it is **not** required to be online for local editing.
This is distinct from the currently implemented REST server, which connects
clients to a single database without copying a repository's history.

## Required invariants

- **Repository identity.** Each independently initialized SQLite database or
  PostgreSQL AOWIS schema has one persistent, write-protected UUIDv7, exposed
  by the local `Database::repositoryUuid()` API. Reopening a repository preserves
  this identifier. A future clone must receive a **fresh repository UUID** while
  retaining the transferred project/branch/revision UUIDs. This identifies a
  storage replica, not a user or an authentication credential. Remote-tracking
  reference names and trust negotiation remain to be specified.
- **Global identities.** Project, branch, revision and model-object UUIDs must
  retain their identities across replica boundaries. Revision numbers are
  branch-local, not global identities.
- **Portable revision manifests.** Finalized revisions can now be exported
  as canonical, backend-independent `AOWIS-REVISION-MANIFEST-V1` binary
  frames using `Database::exportRevisionManifest()`. The frame includes
  original project/branch/revision and author UUIDs; branch-local revision
  number; creation/finalization timestamps; title/message; snapshot and
  operation-history digests; the optional object Merkle root; ordered parent
  UUIDs AND parent chain digests; object-mode discriminator; and the exact
  existing revision-chain SHA-256. The transport codec validates all fields,
  rejects noncanonical framing, and recomputes the original
  `AOWIS-REVISION-DAG-V1` hash. It does **not** transfer snapshot bytes,
  events, Merkle nodes, author credentials or mutable SQL rows. It does not
  prove remote parent availability or underlying content integrity, and
  importing/publishing revisions is not implemented yet.
- **Read-only dependency discovery.** `Database::exportRevisionContentInventory()`
  can enumerate and verify all content-store tree and blob dependencies of a
  finalized **object-mode** revision, using sorted unique hashes. This does
  not cover opaque snapshot bytes, journals or ancestor revisions, and no
  import/transfer operation exists yet.
- **Immutable transfer.** Revisions point to immutable, content-addressed
  records and snapshot roots; identical content hashes must name identical
  verified bytes on SQLite and PostgreSQL.
- **Ancestry and divergence.** The revision graph may have different heads
  descended from a shared historical revision. Ordered parents, including
  merge parents, cannot be flattened into a single global revision counter.
- **Conditional branch publication.** Updating a remote head requires an
  atomic expected-head/fast-forward check or an explicitly authorized merge;
  a stale client must never silently replace newer history.
- **Atomic visibility.** Imported content, revision metadata and reference
  updates must become visible coherently. Interrupted transfers must be
  restartable without exposing half-published revisions as valid heads.
- **Reconstructable working models.** Replica-local relational working rows
  and derived hydraulic indexes are not the authoritative transfer format.
  Importing a finalized model must be able to materialize a compatible
  working state and validate the hydraulic model.
- **Authorization is not hashing.** Content checksums detect changed bytes;
  remote authentication, project/branch permissions, provenance, review and
  protected branches govern which histories may be accepted and published.
- **Storage lifecycle.** Reachability and cleanup policies must not discard
  data required by retained revisions, ongoing transfers or remote references.

## Intended operations

| Operation | Intended result | Status |
| --- | --- | --- |
| Clone | Create an independent project repository with selected history | Planned |
| Fetch | Import missing immutable objects and remote revision/branch metadata without changing local work | Planned |
| Push | Publish authenticated finalized revisions and conditionally advance an authorized remote branch | Planned |
| Pull | Fetch, then integrate remote changes through a defined merge or fast-forward workflow | Planned |
| Remote tracking | Display remote branch heads separately from editable local branches | Planned |
| Conflict resolution | Resolve divergent model edits using ancestry, validation and explicitly supported merge policies | Local merge foundation exists; distributed integration planned |

Transfer efficiency should exploit immutable content hashes and shared tree
structure so unchanged objects do not need to be sent repeatedly. The wire
format, missing-object negotiation, retry/resume behavior, hash algorithms,
partial clones and server trust/authorization policy are **not finalized**.
Do not assume the current SQL schema can itself be copied between backends or
that a PostgreSQL backup is a portable repository bundle.

## Relationship to existing implementation

The current implementation already has immutable finalized roots, a revision
DAG with parent links, optimistic checks, backend-independent content hashes
and local three-way merge machinery. These are foundations, **not a working
replication system**. In particular, local merge correctness is not a
substitute for validating externally supplied revisions, authentication,
ref-update races and partial-transfer recovery.

See [Canonical revision manifests](revision-manifests.md),
[Versioned storage](versioned-storage.md),
[Architecture](architecture.md), [History and merges](history-and-merges.md)
and [Identity and access](identity-and-access.md).
