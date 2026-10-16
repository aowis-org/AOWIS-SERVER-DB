# V18 REST model and grouped operations

The optional Qt HTTP adapter now exposes typed hydraulic-network transport and
atomic generic object groups, without adding new DB persistence or permission
rules. The existing `aowis-db-core` APIs remain authoritative.

## Typed hydraulic network transport

- `GET /api/v1/revisions/{revision_uuid}/networks/{network_uuid}` reads a full
  validated hydraulic network, including all Model fields and child entities.
- `PUT /api/v1/branches/{branch_uuid}/networks/{network_uuid}` updates or
  creates a full network atomically, with generation-based compare-and-swap.

PUT body:

```json
{
  "codec": "aowis.model.network_hydraulic.v1",
  "expected_generation": "0",
  "content_base64": "<base64-encoded ModelCodec<NetworkHydraulic> bytes>"
}
```

The content must decode with the existing versioned binary Model codec and
its embedded UUID must equal the URL UUID. It must pass the same Model
validation and authorization as a local `saveHydraulicNetwork` call. The PUT
result contains `network_uuid`, and `revision_uuid` and `generation` when the
actor also has revision read access. No lossy JSON double conversion is used.

The network GET response contains the same `codec`, the network UUID, revision
UUID, and `content_base64`.

## Generic atomic object groups

- `POST /api/v1/branches/{branch_uuid}/object-groups` commits 1–128 edits in
  one SQLite transaction, with generation and per-object CAS checks.
- `GET /api/v1/revisions/{revision_uuid}/object-groups` returns the durable
  group history (including event IDs, actor UUID, sequence range and action).
- `POST /api/v1/branches/{branch_uuid}/undo` and `/redo` apply the existing
  persistent shared-branch undo/redo engine. Both take `expected_generation`.

Example group edit:

```json
{
  "expected_generation": "3",
  "edits": [
    {
      "uuid": "00000000-0000-7000-8000-000000000001",
      "kind": "test.payload",
      "expected_version": "-1",
      "content_base64": "aGVsbG8="
    },
    {
      "uuid": "00000000-0000-7000-8000-000000000002",
      "kind": "test.payload",
      "expected_version": "-1",
      "content_base64": "d29ybGQ="
    }
  ]
}
```

Deletion uses `"delete": true` instead of `kind` and `content_base64`.
The server rejects generic edits that target existing hydraulic object UUIDs,
regardless of whether the caller supplies a misleading kind or omits it on
DELETE. Generic edits cannot declare a `hydraulic.*` kind. To create, remove,
or modify hydraulic entities, use the typed full-network endpoint until an
explicit validated typed-entity REST API is implemented.

A successful group POST returns status 201 and a `group_uuid`. Undo and redo
return the UUID of the group being compensated. Calls affecting multiple
objects increment the revision generation once **per changed object**, not
once per HTTP request. Thus the returned generation must be used for the next
optimistic write. A stale or invalid core mutation returns 409; inaccessible
read resources return 404. UUIDs and 64-bit counters are strings.

## Boundaries

This is still a **loopback-only development API** with host-supplied bearer
identity verification, not a production authentication system. JSON body
limits are 8 MiB for network and group mutations, with 6 MiB decoded network
or grouped bytes, up to 1 MiB per generic object. No pagination or streaming
is provided yet for network snapshots or history. No SQLite schema or hash
changes are required. The underlying binary Model codec is a protocol
contract for SDK clients, not a human-authored JSON model format.
