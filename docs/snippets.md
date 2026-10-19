# Reusable network snippets — product design

**Status: planned; no snippet library or scenario-composition API is
implemented in AOWIS-SERVER-DB.** A snippet is reusable network content, not a
special kind of project branch or revision.

## Core workflow

An engineer selects a group of connected network elements and saves them as
a named snippet: for example, a pump station with pumps, valves, pipes,
junctions, controls, curves, patterns and defined connection points. Snippets
can be stored in libraries, duplicated, edited into variants and inserted into
other networks **across projects and branches**, subject to authorization.
The initial scope should be reliable copying of selected hydraulic entities.

An independent insertion:

1. Reads a specific, immutable snippet definition/version.
2. Allocates **new UUIDs** for created entities in the target network.
3. Remaps all internal entity references to the new UUIDs.
4. Presents external dependencies, connection ports and incompatible target
   references for explicit resolution. A source-network UUID must not
   silently bind to an unrelated target entity.
5. Transforms portable geometry into the destination coordinate system and
   validates units, elevations, connectivity, constraints and dependencies.
6. Inserts all entities and relationships as **one authorized, atomic,
   undoable edit group**, recording provenance and source-to-destination
   identity mapping.

Modifying a snippet definition does **not** silently modify existing inserted
copies. Snippet versions and copied entity identities are different concepts.
The snippet storage format and exact library/permissions APIs are not fixed.

## Variants and changes to existing entities

A duplicated snippet variant can express an upgrade of content already present
in a target network. For example, a baseline pumping station snippet may have
an alternate pump curve and a newly added isolation valve. To *apply* that
variant to an existing station rather than insert another station, the system
must know which existing entities correspond to which snippet entities.

That requires stable snippet-internal ancestry, explicit target bindings,
attribute-level differences or equivalent operation semantics, preconditions,
conflict detection and deletion/reference rules. **Copying a snippet with new
UUIDs** and **applying a variant to existing UUIDs** are different application
modes, even if they use the same reusable snippet definition.

The design should support these modes without adding an unrelated second
"Change Set" content system unless a concrete requirement demands it.

## Future planning scenarios

A possible higher-level scenario is a **recipe** referencing an immutable
baseline model revision plus a pinned, ordered selection of snippet
applications/variants and engineering assumptions (such as demand or topology
activation). Selecting another variant, changing application order, or
including/excluding a snippet recomposes an effective model; it does **not**
change the immutable baseline revision. A derived state could subsequently be
materialized as a regular new revision if permanent editing or audit is needed.

Reliable scenario composition requires deterministic evaluation, version pins,
explicit target mapping, dependency ordering, merge/conflict handling and
hydraulic validation. **None of this scenario workflow is implemented yet.**

## Relationship to distributed version control

Snippet libraries can eventually be shared across repositories. A snippet
version could use immutable content identities, while each destination still
allocates fresh UUIDs for independent instances. Distribution of snippets
requires ownership, authorization, version pinning and dependency transfer;
it must not be confused with replicating project branches.

See [Distributed repositories](distributed-repositories.md),
[Versioned storage](versioned-storage.md) and
[Model persistence](model-persistence.md).
