# V44 — Read-only revision comparison and merge-preview HTTP API

Remote GUI clients can now inspect the existing C++ ancestry and three-way
object-diff facilities without creating a revision or changing an object.
The endpoints require the normal authenticated bearer session; actor IDs in
query parameters are never accepted.

* `GET /api/v1/merge/compare?left_revision_uuid=UUID&right_revision_uuid=UUID`
  returns `project_uuid`, `left_revision_uuid`, `right_revision_uuid`,
  `left_is_ancestor_of_right`, `right_is_ancestor_of_left`, and an **array**
  of `merge_base_revision_uuids` (including the possible multiple-base case).
* `GET /api/v1/merge/preview?left_revision_uuid=UUID&right_revision_uuid=UUID`
  returns `project_uuid`, `base_revision_uuid`, left/right revision UUIDs,
  **decimal-string** left/right generations and conflict count, and an
  ordered `changes` array. Each changed entity has `object_uuid`, `kind`,
  `classification` (`left_only`, `right_only`, `both_same`, `fields_merged`,
  or `conflict`), `base_present`, `left_present`, `right_present`, and
  `conflicting_fields` (strings including dotted names such as
  `metadata.description`). Presence values refer to live entities, not
  tombstone rows. No binary payloads or authentication credentials are sent.

Exactly the two documented UUID parameters are mandatory. Duplicate, unknown,
missing or malformed inputs are rejected with HTTP 400. Unauthenticated requests
receive 401. Inaccessible, cross-project or unsupported comparisons produce the
same generic 404, to avoid leaking private object or branch information.
The core `compareRevisions()` checks both tips; `previewObjectMerge()` also
requires read access to the finalized merge base, potentially on a third branch.

The REST preview limits the summary to 1,024 changed entities, 16 MiB of
canonical payloads from their three compared states, and 1 MiB serialized
JSON. Larger results return HTTP 409 without partial data. Starting with V46,
three UUID-ordered SQLite cursors stream the base and two source revisions;
unchanged entities are discarded rather than retained in three full maps.
The changed-entity and changed-payload budgets are applied **before** constructing
field-level merge results, while the JSON limit is checked before response
serialization. The C++ merge-commit API retains its unrestricted diff capability.

This bounds retained preview data, not total SQL scan time, SQLite page-cache
usage, or the size of one entity supplied by the database. Very large networks
still need resumable/paginated comparisons and resource limits at the HTTP
server/reverse proxy. These limits are not an execution-time quota.

The results are advisory: they are *not* a merge commit authorization token.
A subsequent merge must recheck the expected revision UUID, generation, object
state, and permissions inside its SQLite write transaction. No REST endpoint
for committing merges is exposed in this milestone. Structural consistency
checks remain separate from EPANET hydraulic simulation.
