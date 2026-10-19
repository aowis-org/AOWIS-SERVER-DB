# Versioned model storage and integrity

AOWIS-SERVER-DB is designed as a **Distributed Versioned Model Database**.
Its current storage implementation keeps complete logical historical model
states without storing complete physical model copies at every revision. This
reference describes the **implemented** on-disk representation; repository
replication remains a separate [planned capability](distributed-repositories.md).

## Storage model

- **Model object identity:** a stable 128-bit UUID, generated using
  `createUuidV7()` for new model entities. The UUID survives edits.
- **Content identity:** a SHA-256 digest of an immutable, canonical record.
- **Revision identity:** a permanent revision UUID, distinct from its content
  hash. Revision metadata and ordered parent links form a version graph.
- **Finalized snapshot identity:** a 32-byte immutable tree root mapping object
  UUIDs to content hashes. Identical subtrees and records can be referenced by
  many revisions, branches and projects.
- **Working state:** SQL model rows and derived hydraulic membership/reference
  indexes, maintained transactionally for editing and validation. They are
  **not** historical model copies.

Each finalized object-mode revision references a complete logical model. It can
be read directly from its root: old events or intermediate revisions need not
be replayed to obtain the state of a given pipe or network. Immutable objects
and shared tree nodes are internal storage primitives, not user-facing
revision, snippet or branch entities.

### Content-addressed objects

The `content_objects` table contains `sha256` (32-byte key), `kind` (`blob` or
`tree`) and `payload` (exact immutable bytes). Its identity is SHA-256 of:

1. ASCII `AOWIS-CONTENT-V1` followed by NUL (17 bytes).
2. One byte of kind: `0x01` (`blob`) or `0x02` (`tree`).
3. Payload length as unsigned 64-bit big-endian.
4. The exact payload bytes.

The identifier does not depend on a SQL driver's UUID encoding, host machine,
project, revision or timestamp. Write-once constraints prevent accidental
changes; collisions/reused identities are checked against existing content.
Reads verify content hashes. SQL update/delete guards do not protect against a
privileged database administrator altering storage and recomputing hashes.

### Shared snapshot index

`src/merkle_tree.cpp` implements a persistent, prefix-compressed, 16-way UUID
radix tree. The key is a 16-byte RFC 4122 UUID, interpreted as 32 hexadecimal
nibbles. The value is the 32-byte hash of a `blob`. A complete model is
identified by the hash of its root `tree` node. The empty model uses a
canonical empty root, not SQL NULL.

Each node is canonically encoded as:

1. ASCII `AOWIS-MERKLE-RADIX-V1` followed by NUL (22 bytes).
2. One-byte prefix length (0–32 nibbles).
3. Prefix nibbles packed high nibble first; odd-length padding is zero.
4. One-byte node flag (`0x00` internal, `0x01` leaf).
5. A leaf's 32-byte `blob` hash (internal nodes contain no value).
6. Two-byte big-endian child bitmap, nibble positions 0–15.
7. Present child `tree` hashes in ascending nibble order.

A leaf consumes all 32 key nibbles and has no children; a nonempty internal
node has at least two children, with unary paths compressed. Encoding is
independent of insertion order or database backend.

`lookup()` follows a single UUID path; `lookupMany()` batches shared prefix
traversals; `entries()` verifies and enumerates an entire root. `apply()`
performs copy-on-write updates of changed paths. `build()` produces the same
canonical root by building the complete tree bottom-up, inserting immutable
content in bounded batches. The maximum traversal depth is bounded by the UUID
length, not by the number of historical revisions. Full model reads still need
to process the required model objects.

### Canonical model records

The blob associated with an object UUID is:

1. ASCII `AOWIS-MODEL-RECORD-V1` followed by NUL (22 bytes).
2. Object-kind UTF-8 byte length (unsigned 64-bit big-endian) and bytes.
3. Object version (unsigned 64-bit big-endian).
4. Deleted flag (one byte, `0` or `1`).
5. For live records, content length (unsigned 64-bit big-endian) and exact
   encoded model bytes.

The object UUID is the tree key, not duplicated in the blob. Versioned
tombstones are preserved as records. Identical records may share content;
changes to an object leave its previous records and historical roots intact.

The typed hydraulic codec (`src/model_codec.cpp`) has its own deterministic
field ordering and format contract. See [Model persistence](model-persistence.md).

## Revisions and working-state lifecycle

Projects contain branches, and branches contain immutable finalized revisions
and a current editable working revision. Finalized object-mode revisions store
`object_root_sha256`; opaque snapshot mode also exists in the current API and
uses a different snapshot representation.

When a working revision is finalized:

1. The current model state is validated and its immutable content/root is
   recorded. Consecutive revisions can reuse their parent's root, replacing
   only object UUID paths touched by the journal.
2. The model root, edit/group-history digest, immutable revision metadata and
   ordered parent hashes are covered by the revision-chain seal.
3. The already validated working SQL objects and hydraulic indexes are copied
   to the new editable successor using set-based SQL inside the same
   transaction. A transaction-local `sealing` state permits this handoff
   without publishing two working revisions.
4. Materialized model and index rows belonging to the finalized predecessor
   are removed; the successor becomes the branch head. Commit or rollback
   covers the whole operation.

Creating an **independent branch** from a finalized revision instead resolves
its immutable root, checks model records, builds a new working object state
and rebuilds the hydraulic indexes. This is necessary to avoid sharing
mutable working rows across branches. Finalized historical `object()` and
`objects()` lookups, hydraulic network loads and merge inputs read from the
immutable tree, not historical SQL model/index rows.

## History and verification

Object-mode `snapshots.content` is a compact history seal, not a second network
copy: ASCII `AOWIS-REVISION-HISTORY-V1` followed by NUL, the object-event
journal SHA-256 and the edit-group-history SHA-256.

`Database::verifyProject()` verifies stored immutable content, reachable
snapshot nodes, history manifest, revision metadata/ancestry and chain hashes.
Its cache of already verified immutable content is **local to one verification
call**, never shared with later requests. This standard verification does
**not** establish that every journal event semantically replays to the stored
model. `Database::verifyProjectJournal()` additionally replays ordered object
events from each first parent (including merge journals relative to that
parent) and compares the result with the sealed object snapshot. It is an
expensive read-only forensic operation.

The current revision-chain byte format is documented in
[Revision history and merges](history-and-merges.md). The exact identifiers
above are **on-disk format identifiers**, not software release or patch
versions; their bytes must match the code. Content hashes verify integrity,
not author identity or authorization. An administrator able to replace all
objects and hashes falls outside the present threat model.

## Backend and performance characteristics

Both SQLite and PostgreSQL use the same canonical hashes and snapshot layout;
the physical SQL schema, connection behavior and UUID parameter binding differ.
New SQL working objects and immutable content are inserted in batches.
PostgreSQL uses JSONB batch operations where appropriate; SQLite uses
multi-row or prepared inserts. Initial snapshot construction is proportional
to the full model size. Incremental revision roots share unchanged paths.
Transferring the editable working state at finalization still performs
set-based work proportional to model size. Independent branch creation
reconstructs its own working model and hydraulic indexes.

The [performance guide](performance.md) documents repeatable benchmarks,
memory counters and profiling. We do not claim that finalization, branch
creation or a full network load takes constant time.

## Retention and future distribution

Immutable data is currently retained: general reachability-based garbage
collection is not implemented. GC will require a policy that protects all
reachable roots, in-progress operations and eventual remote transfer needs.

The format is intentionally backend-independent so SQLite and PostgreSQL
repositories can eventually exchange immutable objects and verified revision
history. **No clone/fetch/push/pull protocol, remote tracking or cross-repository
integrity negotiation is implemented yet.** See
[Distributed repositories](distributed-repositories.md).

There is one current pre-release schema per backend, with no migrations for
obsolete development databases. Preserve valuable data; recreate **only
disposable** development databases if their schema is incompatible.
