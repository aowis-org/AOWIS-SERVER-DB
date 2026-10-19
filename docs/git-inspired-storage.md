# Git-inspired content-addressed revision storage

## Purpose

AOWIS stores engineering models, not a history of file-diff patches. Every
finalized revision should be a **complete logical snapshot**, but consecutive
snapshots should share unchanged content instead of copying all model objects
and reference indexes. The architecture is inspired by Git's immutable blobs,
trees, and commits, adapted for UUID-addressed engineering entities.

This is a pre-release redesign: no support for old development SQL schemas or
old hash formats is required. Existing development databases can be recreated.
The refactor must not weaken history, undo/redo, authorization, merge, or
cross-backend correctness.

## What the benchmarks established (2026-10-19)

The old revision-copy path writes the full `revision_objects` payload and
hydraulic membership/reference rows on every new working revision. Measured:

| Backend | Pipes | Finalized revisions | Physical database size | Typical finalize |
| --- | ---: | ---: | ---: | ---: |
| SQLite | 1,000 | 10 | 30.5 MB | tens of ms |
| SQLite | 1,000 | 50 | 126.9 MB | tens of ms |
| SQLite | 10,000 | 10 | 302 MB | hundreds of ms |
| PostgreSQL | 1,000 | 10 | 29.5 MB (schema) | hundreds of ms |

These are observations on one machine, not promises of future improvements.
The new design must be benchmarked under identical workloads.

## Sub-roadmap

1. **Immutable content store — complete.** Introduce
   backend-neutral domain-separated SHA-256 identities, deduplicated immutable
   payload storage (`blob` and future `tree` nodes), SQL write-once protection,
   integrity-verified reads, and SQLite/PostgreSQL tests. Do not change revision
   behavior yet. Existing test cases must continue passing.
2. **Persistent Merkle index — implemented as an internal primitive.** A
   canonical, path-compressed UUID-keyed 16-way Merkle radix tree maps keys to
   immutable blob hashes. Internal and leaf nodes are content-addressed. A
   lookup walks a known root; a mutation returns a new root, writing only
   affected nodes. Supports missing keys, updates, deletes, empty snapshots,
   deterministic batches and complete ordered scans, with cross-backend tests.
   Tree depth is bounded by UUID length, not revision count.
3. **Revision root integration and physical cutover — implemented.** The
   finalized Merkle root holds exact UUID-to-record hashes (including object
   versions and tombstones), and immutable blobs contain canonical records.
   Finalization computes its snapshot and history seals, deletes the outgoing
   revision's temporary `revision_objects`, `hydraulic_network_members`, and
   `hydraulic_network_references` rows after copying the validated working
   rows to its new editable successor in the same write transaction. A
   transaction-local `sealing` state allows both working heads to coexist
   briefly without violating the unique-working-revision constraint. Only
   the final revision state and Merkle root are retained historically.
   All changes occur in one SQL transaction; failures roll everything back.
   Historical single-object reads, bulk loads, typed hydraulic reads, merge
   input streams, and branch creation use Merkle roots exclusively. Finalized
   revisions have no SQL object or hydraulic-index materialization. Integrity
   verification checks every reachable immutable node/blob, reconstructs the
   canonical manifest from Merkle records, and independently recomputes
   journal/group data. For generation-zero merges, the working-head SQL state
   is compared against its parent root to detect unjournaled edits. No schema
   migration or compatibility layer for pre-release databases is provided.
4. **Hydraulic index tuning.** Only working-heads retain derived membership
   and reference indexes; no finalized row duplication remains. Benchmark the
   reconstruction and full-network load paths on SQLite and PostgreSQL before
   considering additional persistent/shared indexes.
5. **Benchmark, stress and simplify.** Repeat 1k/10k/100k-network and 10/50/100-
   revision workloads with and without changes. Measure bytes per additional
   revision, object lookup latency at arbitrary historical revisions, complete
   network load, finalization, branch creation, merge, undo/redo, verification,
   peak memory, and integrity rejection. Remove obsolete copy helpers and
   unused columns once the new path is proven.

Each stage should be independently testable and require no unsupported
compatibility layer. Stages 3–4 may need coordinated changes to avoid a
half-migrated persistence schema in the final patch.

## Follow-up: verification and working-head performance

After the first physical cutover, the 2026-10-19 measurements confirmed very
large storage savings but showed that repeatedly rebuilding the editable SQL
head and rechecking the same immutable blobs was expensive. SQLite working-head
materialization now inserts 64 object rows per statement (384 parameters,
including deleted identities), rather than executing one statement per row.
PostgreSQL retains its JSONB batch path.

Full-project verification uses **request-local** caches for SHA-256-verified
Merkle node payloads and blob identities across finalized revisions. Tree
path/shape checks still run for each root, and journal, history, authorship,
parent links, and final chain hashes are still independently checked for every
revision. Caches are destroyed at the end of the verification call and are
never reused across calls, so external database corruption is not silently
hidden by long-lived verification state. This reduces repeated physical reads
of unchanged, structurally shared content. The working-head materialization
and derived hydraulic-index rebuild remain further optimization candidates.

## Stage 1: content store contract

The internal table `content_objects` has three columns:

- `sha256`: binary 32-byte primary key.
- `kind`: `blob` or `tree`.
- `payload`: exact non-null bytes stored once.

An immutable content object's identity is SHA-256 of the following **binary**
preimage (not SQL text, not a Qt-serialized wrapper):

1. ASCII `AOWIS-CONTENT-V1` followed by one NUL byte (17 bytes).
2. One byte: `0x01` for `blob`, `0x02` for `tree`.
3. Payload length as unsigned 64-bit big-endian.
4. The exact payload bytes, including any zeros.

No timestamp, revision UUID, project UUID, backend-dependent UUID encoding, or
compression is in the preimage. Equal payloads of equal kinds yield equal
hashes across revisions, projects and backends. Different kinds yield
different hashes. Content is never rewritten in place. Writes use a unique
primary key and `ON CONFLICT DO NOTHING`, then verify the stored bytes before
reporting success. Read operations recompute the digest, failing on corruption.

Both SQL backends block `UPDATE` and `DELETE` using triggers. This is a guard
against accidental application mistakes, not against a privileged DB admin.
The store uses the caller's transaction; it never commits, rolls back, or
chooses an actor. Permission checks take place at the higher database-service
boundary, before content roots are changed.

**No garbage collection in Stage 1.** Storage must not delete unreferenced
objects while finalized revisions may refer to them. Future GC needs a reachability
traversal from all live revision roots (and any other retained references) and
must be enabled only when reachable data cannot be lost.

The `tree` kind stores Stage 2's canonical Merkle radix nodes (see the next
section). No delta chains are needed to realize structural sharing; optional
compression can be evaluated later, keeping logical identity independent of
physical representation.

## Stage 2: persistent UUID Merkle radix tree — implemented

Internal implementation: `src/merkle_tree.h` and `src/merkle_tree.cpp`.
The tree is now the authoritative persistence path for finalized object-mode
revisions. Editable working heads use relational tables for efficient mutations.

### Key and value semantics

- Key: exactly one 128-bit model-entity UUID, ordered by its 16 RFC 4122
  bytes. Each key is expanded into 32 high-to-low hexadecimal nibbles.
- Value: exactly one 32-byte hash of an existing immutable `blob` content
  object. Changing an entity never changes its UUID, only the referenced blob
  digest. Deleting a key removes its mapping from the next snapshot; prior
  snapshots still reference it.
- Root: exactly one 32-byte hash of a `tree` content object. Empty snapshots
  are represented by the canonical empty tree node, **not a null root**.
- The caller owns the surrounding transaction and is responsible for choosing
  which returned root to persist as the working revision's state.

### Canonical node bytes

A node is represented by **one** immutable `tree` payload with this binary
encoding (no Qt stream format, no host-endian fields):

1. ASCII `AOWIS-MERKLE-RADIX-V1` and one NUL byte (22 bytes).
2. Unpacked-prefix length as one byte, 0–32 **nibbles**.
3. Prefix nibbles packed two per byte, high nibble first; when odd, the last
   low nibble must be zero (canonical padding).
4. One byte: `0x00` for an internal node, `0x01` for a leaf.
5. For leaves, the 32-byte referenced Blob hash. Branches have no value.
6. Two bytes of child-presence bitmap, unsigned big-endian; bit `i` means
   child `i` exists for nibble `i` (0–15).
7. One 32-byte tree hash per present child, in ascending nibble order.

Leaves must consume all 32 key nibbles and have no children. Nonempty
branches must have at least two children; there are no redundant unary
branches. The only childless internal node is the canonical empty root.
Prefix compression eliminates long shared paths; insertion splits a compressed
path at the first different nibble, and deletion compacts redundant unary
paths. The encoding is canonical for any UUID-to-hash mapping, independent of
insertion or deletion order. Every stored node is identified by the existing
`content_store::digest(Kind::Tree, payload)` contract.

### Operations and complexity

- `emptyRoot()`: store or reuse the canonical empty root.
- `lookup(root, uuid)`: read only the nodes along that UUID's prefix path.
  Missing keys are distinct from errors through the optional error message.
- `update(root, Change)`: insert, replace, or remove a UUID mapping; return
  an immutable root. Unchanged values and missing-key deletes reuse the root.
- `apply(root, changes)`: deterministic batch sorted by UUID, rejecting
  duplicate UUIDs. This incremental path performs copy-on-write tree updates.
- `build(changes)`: constructs the **same canonical root** for a complete
  snapshot bottom-up in memory. Child nodes are encoded and hashed locally,
  then written through bounded immutable-content batches, avoiding thousands
  of PostgreSQL read/write round trips when sealing the initial full model.
  Duplicate UUIDs and missing content identities are rejected. It does not
  change the canonical node format or root hash contract. The caller publishes
  the **final** root inside its transaction.
- `entries(root)`: list all UUID/hash pairs in ascending key order and verify
  every reached tree node. This is intentionally a full-tree scan.

Point lookup and a single mutation visit at most **33 node levels** (32
nibbles plus the root; usually far fewer thanks to path compression).
There is no traversal from one revision to an earlier revision. Copying one
working revision's root is constant time at the index level. A full model load
still reads every model object. Initial full-root construction now avoids
per-key SQL tree updates; incremental batches with many changed UUIDs still
use repeated point updates and may benefit from future multipath batching.

The tree's Merkle root commits to its UUID→blob-hash map. Content hashes
commit to each object's bytes. Integrity checks on `content_store::get`
reject corrupted node bytes. Finalized revisions bind their Merkle root to the
revision integrity seal. Branches reconstruct their working hydraulic
indexes from sealed model data, without using historical SQL indexes.

As with the content store, there is no garbage collection or revision-root
publication in this stage. Nodes written by a failed caller operation remain
unreferenced unless the caller rolls back the transaction; no old roots are
rewritten or removed.

## Snapshot data model and lookup behavior

- Entity identity: immutable UUIDv7 model-object key, stable across edits.
- Content identity: SHA-256 of canonical immutable record bytes.
- Revision identity: permanent revision UUID, not its content hash.
- Finalized model root: SHA-256 of a persistent UUID-keyed Merkle tree.
- Working state: transactionally maintained SQL objects and derived indexes,
  discarded as soon as the revision is sealed.

The tree provides UUID point lookups and ordered bulk scans without replaying
history or walking parent revisions. Working-head hydraulic indexes remain
materialized for efficient validation and edits, but finalized revision index
rows are never retained. The content store is an internal component behind
the normal authorization and transactional history APIs.

## Stage 3: finalized Merkle roots and physical storage cutover

The `revisions.object_root_sha256` column is populated for finalized
object-mode revisions, and null for opaque snapshots and working revisions.
The same transaction builds the root, creates and seals the immutable manifest,
discards temporary SQL model/index rows and constructs the next editable head.
The revision-chain hash commits to the root and operation-history digest.

Each leaf is a `content_store::Kind::Blob` with the following canonical bytes:

1. ASCII `AOWIS-MODEL-RECORD-V1` followed by NUL (22 bytes).
2. Object-kind UTF-8 length and bytes (unsigned 64-bit big-endian).
3. Object version (unsigned 64-bit big-endian).
4. Deleted flag (one byte, 0 or 1).
5. If not deleted: content length (unsigned 64-bit big-endian) and exact bytes.

The UUID is the 16-byte **tree key**, not duplicated in the record blob.
Two revisions sharing identical object bytes, kind and version therefore
share a blob. Deletion retains a versioned tombstone under the same UUID.
The Merkle root commits to the entire UUID-to-record mapping. For a new
revision copied from a finalized parent, only UUIDs mentioned in that
working revision's `object_events` need new tree paths and blobs. Other
branches and old revisions retain their original roots. The first root,
or an object-mode revision without a sealed object-mode parent, is built
from the complete SQL object mapping.

For object-mode revisions, `snapshots.content` is now a compact history seal:
NUL-terminated ASCII `AOWIS-REVISION-HISTORY-V1` followed by SHA-256 of the
canonical object-event journal and SHA-256 of canonical edit-group records.
It does **not** serialize the complete model again. The revision's stored
`object_root_sha256` is the complete model-state identity, and the chain digest
binds both that root and the history seal. This keeps the historical
`snapshots` table independent of network size. All older pre-release object
manifest encodings and their version-selection column have been removed.

`verifyProject()` checks all reachable Merkle nodes and object blobs,
recomputes the event/group journal and reconstructs the canonical model
manifest independently, then verifies the recorded snapshot and revision-chain
SHA-256 digests. There is no historical SQL object copy against which to
compare. The write path always discards object and derived-index rows before
setting `state='finalized'`; future edits take place only in the new working
revision. Historical object reads, model loads, and merge inputs use the
content-addressed root. A generation-zero merge still checks the **working**
SQL model against its finalized parent root.

A newly forked branch materializes its independent working head from the
sealed root, verifying all referenced blobs and rebuilding derived hydraulic
indexes. On **finalization of an existing working branch**, however, the
source SQL state was already validated. The server now temporarily transitions
the predecessor to `sealing` inside the write transaction, inserts its successor
with state `working`, and transfers objects, membership and reference rows
with three `INSERT ... SELECT` statements. The old temporary SQL rows are
removed, the predecessor becomes permanently `finalized`, the parent edge is
recorded and the branch head advances before commit. No transaction commits
with a `sealing` revision, and failures roll back the entire operation.

This preserves immutable Merkle-only history without paying for a full tree
traversal, blob decoding and network-validation pass on every finalization.
The transactional handoff remains linear in the working model size, but has
constant SQL statement count and no per-object network round trips. It is not
historical storage duplication. A future persistent working-overlay architecture
could remove even this remaining O(N) cost if benchmarks justify it.
No migration or backward-compatibility mechanism is provided for pre-release
development databases.

### Stage 3E: Merkle-authoritative merge reads

The three-way merge now treats finalized revisions as immutable content-addressed
states. Its finalized base/right/left cursors enumerate ordered UUID/hash
entries and fetch model record blobs in batches of at most 256. They skip
versioned tombstones for *semantic* comparisons, while the physical preview
fingerprint still includes tombstones and version counters. Working revisions
continue streaming `revision_objects` rows. A preview can therefore compare
historical branches even if obsolete SQL copies are absent, without replaying
change journals and without making one SQL request per entity.

During merge commit the generation-zero working-head consistency guard checks
its complete SQL materialization against the finalized parent's Merkle root.
The optimistic preview-to-commit digest still validates the *working* head
under a write transaction. This check remains necessary after finalized SQL object rows are removed.

The merge cursor buffers at most 256 full object payloads per finalized source,
plus an ordered UUID/hash index for that snapshot. Long revision histories do
not increase lookup depth; very large snapshots still require benchmarking
of the entry-index memory footprint and PostgreSQL batch latency.


## Stage 3F: materializing working branches from immutable roots

`createBranch()` no longer uses a SQL `INSERT ... SELECT` from the historical
`revision_objects` table to initialize the branch's editable model objects.
Instead it enumerates the finalized base root, verifies each content-addressed
blob and record, and inserts exact UUID/kind/version/content/tombstone values
into the **new working revision only**, in bounded batches. PostgreSQL uses
`jsonb_to_recordset` for one insert per 256 records; SQLite reuses a prepared
statement within the caller's single write transaction. Failure rolls back the
entire branch creation. No previous snapshot or Merkle node is changed.

This is **working-head materialization**, not version-history storage:
historical objects already live only in immutable content-addressed Merkle
trees. Branch creation rebuilds hydraulic membership and reference indexes
from the validated **new working model** materialized from the Merkle root,
not nonexistent historical SQL indexes. Its initial membership index is empty,
so the normal working model loader fetches referenced entities using bounded
primary-key IN batches (300 UUIDs per query). It does not traverse and decode
all sealed blobs **again** just to derive indexes. Typed entity decoding and
hydraulic reference validation still run, so invalid network relationships
reject branch creation inside its transaction.

Finalization instead hands the existing working SQL rows to its successor
transactionally; unlike an independent branch, it can reuse validated working
indexes without another Merkle traversal. Building a fresh branch and initially
sealing a large full snapshot remain linear in network size; we benchmark them
separately rather than claim constant-time branch creation.

There is no migration or backward-compatibility layer for development DBs.
