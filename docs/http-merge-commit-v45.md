# V45 — Atomic REST merge commits

`POST /api/v1/branches/{target_branch_uuid}/merge` is an authenticated,
write-capable counterpart to V44's read-only preview. The target branch's
working revision is the left side, and its **finalized** parent remains the
first parent of the resulting merge commit. The right side must be a finalized
revision from another branch of the same project, diverged from the left parent.

Example JSON body:

```json
{
  "expected_working_revision_uuid": "00000000-0000-7000-8000-000000000001",
  "expected_generation": "2",
  "right_finalized_revision_uuid": "00000000-0000-7000-8000-000000000002",
  "title": "Merge survey corrections",
  "message": "Merge the reviewed hydraulic model edits",
  "resolutions": [
    {"object_uuid": "00000000-0000-7000-8000-000000000003", "side": "right"},
    {"object_uuid": "00000000-0000-7000-8000-000000000004",
     "field_resolutions": [
       {"field_name": "elevation_m", "side": "left"},
       {"field_name": "metadata.description", "side": "right"}
     ]}
  ]
}
```

`resolutions` is mandatory but may be an empty array for conflict-free merges.
For each conflicting entity specify **either** `side` (whole-entity choice) **or**
`field_resolutions` (every conflicting field, exactly once), never both. Choices
are exact, case-sensitive `left` / `right`. Clients obtain conflict names from
`GET /api/v1/merge/preview`; V40–V43 field-aware merge behavior is retained.

HTTP 201 returns `finalized_revision_uuid`, `new_working_revision_uuid`,
`snapshot_sha256` and `chain_sha256`. Invalid JSON, fields, UUIDs, duplicate
choices, unknown keys, noncanonical generations, queries, or oversized bodies
return 400. Unauthenticated requests receive 401. Permission failures,
unsupported sources, missing/unknown conflicts, stale expected heads,
validation errors, and transactional failures return the same generic 409;
private resource or authorization details are not disclosed. The endpoint does
not accept an actor UUID: the authenticated server callback supplies it.

Request JSON is capped at 64 KiB; at most 256 per-entity resolutions, 64
choices per entity, and 1,024 field choices overall. Titles must have nonwhite
content (up to 256 characters); messages are limited to 4,096 characters.
The exact known keys are mandatory, and 64-bit generations are decimal strings
to avoid JSON number precision loss.

The database core, not the HTTP handler, enforces `project.model.write` and
`project.revision.finalize` on the target, `project.read` on both source
branches and the common finalized merge base, expected generation/UUID,
physical object-state consistency, three-way conflict validation, grouped
history, network **structural** checks, and atomic SHA-256 finalization.
A stale preview is advisory, never an authorization or generation bypass.
On failure the existing local edits and undo/redo history are preserved.
Neither an EPANET hydraulic simulation nor water-quality simulation runs.

The endpoint executes a merge synchronously. For very large networks, bounded
preview computation and cancellation/timeout controls remain future work.
