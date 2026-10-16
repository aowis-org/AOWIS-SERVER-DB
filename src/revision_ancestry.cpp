#include <aowis/db/database.h>

#include <algorithm>

#include <QHash>
#include <QSet>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

// Traversal uses immutable revision identities and parent edges. A visited set
// bounds processing of shared ancestors and prevents loops in corrupted data.
// The current working revision may be a root; every parent must be finalized.
bool collectAncestors(const QUuid &start, const QUuid &project_uuid,
                      const std::function<std::optional<RevisionInfo>(const QUuid &, QString *)> &lookup,
                      QSet<QUuid> &ancestors, QHash<QUuid, QVector<QUuid>> &parents,
                      QString *error)
{
    QVector<QUuid> pending{start};
    while (!pending.isEmpty())
    {
        const QUuid current_uuid = pending.takeLast();
        if (ancestors.contains(current_uuid))
            continue;

        const std::optional<RevisionInfo> revision = lookup(current_uuid, error);
        if (!revision)
            return false;
        if (revision->project_uuid != project_uuid ||
            (current_uuid != start && !revision->finalized))
        {
            setError(error, QStringLiteral("Invalid cross-project or non-finalized revision ancestor"));
            return false;
        }
        ancestors.insert(current_uuid);
        parents.insert(current_uuid, revision->parent_revision_uuids);
        for (const QUuid &parent_uuid : revision->parent_revision_uuids)
        {
            if (parent_uuid == current_uuid)
            {
                setError(error, QStringLiteral("Revision cannot be its own parent"));
                return false;
            }
            if (!ancestors.contains(parent_uuid))
                pending.append(parent_uuid);
        }
    }
    return true;
}

} // namespace

std::optional<RevisionComparison> Database::compareRevisions(const OperationContext &actor,
    const QUuid &left_revision_uuid, const QUuid &right_revision_uuid, QString *error) const
{
    const std::optional<RevisionInfo> left = revisionUnchecked(left_revision_uuid, error);
    if (!left || !requirePermission(actor, QStringLiteral("project.read"),
                                    AccessScope::branch(left->branch_uuid), error))
        return std::nullopt;
    const std::optional<RevisionInfo> right = revisionUnchecked(right_revision_uuid, error);
    if (!right || !requirePermission(actor, QStringLiteral("project.read"),
                                     AccessScope::branch(right->branch_uuid), error))
        return std::nullopt;
    if (left->project_uuid != right->project_uuid)
    {
        setError(error, QStringLiteral("Revisions belong to different projects"));
        return std::nullopt;
    }

    QSet<QUuid> left_ancestors;
    QSet<QUuid> right_ancestors;
    QHash<QUuid, QVector<QUuid>> parents;
    const auto lookup = [this](const QUuid &uuid, QString *lookup_error) {
        return revisionUnchecked(uuid, lookup_error);
    };
    if (!collectAncestors(left_revision_uuid, left->project_uuid, lookup,
                          left_ancestors, parents, error) ||
        !collectAncestors(right_revision_uuid, right->project_uuid, lookup,
                          right_ancestors, parents, error))
        return std::nullopt;

    RevisionComparison result;
    result.project_uuid = left->project_uuid;
    result.left_revision_uuid = left_revision_uuid;
    result.right_revision_uuid = right_revision_uuid;
    result.left_is_ancestor_of_right = right_ancestors.contains(left_revision_uuid);
    result.right_is_ancestor_of_left = left_ancestors.contains(right_revision_uuid);

    QSet<QUuid> common = left_ancestors;
    common.intersect(right_ancestors);
    // An editable working revision cannot be a stable merge base. The only
    // way a working node can occur in both walks is when both tips are that
    // very same node. Exclude it; its finalized parents may still be bases.
    if (left_revision_uuid == right_revision_uuid && !left->finalized)
        common.remove(left_revision_uuid);
    QSet<QUuid> merge_bases = common;

    // All strict ancestors of ANY common node are dominated, not merely the
    // lowest-numbered common nodes. One multi-source traversal is O(V+E).
    QVector<QUuid> pending;
    for (const QUuid &uuid : common)
    {
        for (const QUuid &parent_uuid : parents.value(uuid))
            pending.append(parent_uuid);
    }
    QSet<QUuid> dominated;
    while (!pending.isEmpty())
    {
        const QUuid uuid = pending.takeLast();
        if (dominated.contains(uuid))
            continue;
        dominated.insert(uuid);
        merge_bases.remove(uuid);
        for (const QUuid &parent_uuid : parents.value(uuid))
            pending.append(parent_uuid);
    }
    result.merge_base_revision_uuids = merge_bases.values();
    std::sort(result.merge_base_revision_uuids.begin(), result.merge_base_revision_uuids.end(),
        [](const QUuid &a, const QUuid &b) { return a.toRfc4122() < b.toRfc4122(); });
    return result;
}

} // namespace aowis::db
