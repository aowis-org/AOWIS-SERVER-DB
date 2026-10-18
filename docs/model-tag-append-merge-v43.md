# V43 — Conservative append-only metadata tag merging

AOWIS Model stores `EntityMetadata::tags` as an **ordered `QStringList`**.
V42 treated it as one indivisible merge field. V43 recognizes one safe
subcase of concurrent changes: two branches independently **append distinct
new tags** while preserving the original list byte-for-byte and in order.

## Rule

Given a common finalized ancestor's tag list `base`, two divergent lists
`left` and `right` can be combined automatically only when:

1. Both lists begin with exactly `base`, with the same elements in the same
   positions; both sides must actually append at least one item.
2. New tags are nonempty, do not repeat any baseline tag, and are unique
   across **both** appended suffixes (including within either suffix).
3. The surrounding typed objects pass the existing canonical decoding,
   payload-UUID, size-limit and byte-exact round-trip checks.

The result is the exact original prefix followed by **the complete left
suffix, then the complete right suffix**. Ordering inside each suffix is
preserved. This is deterministic for the ordered left/right merge inputs;
swapping the merge sides can change the order of appended tags, which is why
the merge is not described as an unordered set union.

If the append-only rule fails, `previewObjectMerge()` continues to report
`Conflict` with `metadata.tags` among `conflicting_fields`. Callers can choose
`Left` or `Right` through the existing field-resolution API or resolve the
whole entity. Reordering, removals, editing existing tags, duplicate additions,
empty added tags, and overlapping suffixes are **never** silently combined.
When both branches produce an identical complete list, the existing standard
three-way comparison accepts that identical field edit without this rule.
One-sided tag edits remain unaffected.

## Scope and integrity

Only `EntityMetadata::tags` uses the append-only exception. All other ordered
lists, including network demands, pipe vertices, rule actions and curve points,
remain atomic. No new REST endpoints, C++ public interfaces, schema migrations,
or simulation requirements are introduced. All V39–V42 authorization,
transaction rollback, history, hash-chain and reference-validation guarantees
remain in place. Reference/structural validation is **not** an EPANET run.

`aowis-db-object-merge-tags` covers the automatic combination, ordering,
independent metadata edits, the conservative rejection paths, explicit
resolutions, SQLite rollback, finalization, and persisted project integrity.
