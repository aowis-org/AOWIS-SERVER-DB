# V9: indexed hydraulic network membership

V8 scanned and decoded every hydraulic root in a revision to locate an edited
entity's owning network. It also loaded every live model object in the revision
to reconstruct just that one affected network. Both costs grew with unrelated
networks, even for a single pipe edit.

SQLite schema version **6** adds the derived `hydraulic_network_members` table:

- `(revision_uuid, entity_uuid)` is unique: one network owner per revision.
- `(revision_uuid, network_uuid)` is indexed for selective network loading.
- Existing V5 roots (including finalized revisions) are backfilled atomically.
- Future branch creation and revision finalization copy memberships together
  with object snapshots.
- Root-index changes, entire-network deletion, and grouped undo/redo update
  memberships transactionally, after validation of the final model graph.
- A network's reconstruction reads just its root and referenced entities;
  a changed root may temporarily refer to new entities not yet in the index.

**Integrity:** membership rows are derived lookup data, not an additional
hash input. The existing immutable snapshot bytes, object journal and finalized
SHA-256 hashes remain unchanged. SQL triggers prevent updates to finalized
membership. Migration does not rewrite a finalized revision.

**Current limits:** validating a touched network still reconstructs that one
network and runs the full reference validator. It no longer scans unrelated
network roots. A future stage can introduce a persistent reverse-reference
index to validate just an object's dependencies, without losing cross-object
semantic checks. Full-network saves also still compare current objects in the
revision; that separate scaling path should be optimized after this one.
