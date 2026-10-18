#include <aowis/db/database.h>
#include "object_merge_state.h"
#include "model_field_merge.h"

#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

bool sameContent(const std::optional<ModelObject> &a, const std::optional<ModelObject> &b)
{
    if (!a || !b)
        return !a && !b;
    return a->kind == b->kind && a->content == b->content;
}

bool execute(QSqlQuery &query, QString *error)
{
    if (query.exec())
        return true;
    setError(error, query.lastError().text());
    return false;
}

// A new working head is copied byte-for-byte from its finalized parent.
// Guard against unjournaled SQL edits, including tombstone/version changes.
bool samePhysicalObjects(const QSqlDatabase &database, const StorageBackend &backend,
                         const QUuid &working_uuid, const QUuid &parent_uuid, QString *error)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT EXISTS(SELECT object_uuid,kind,content,version,deleted "
            "FROM revision_objects WHERE revision_uuid=? EXCEPT "
            "SELECT object_uuid,kind,content,version,deleted "
            "FROM revision_objects WHERE revision_uuid=?) OR "
            "EXISTS(SELECT object_uuid,kind,content,version,deleted "
            "FROM revision_objects WHERE revision_uuid=? EXCEPT "
            "SELECT object_uuid,kind,content,version,deleted "
            "FROM revision_objects WHERE revision_uuid=?)")))
    {
        setError(error, query.lastError().text());
        return false;
    }
    query.addBindValue(backend.bindUuid(working_uuid));
    query.addBindValue(backend.bindUuid(parent_uuid));
    query.addBindValue(backend.bindUuid(parent_uuid));
    query.addBindValue(backend.bindUuid(working_uuid));
    if (!execute(query, error) || !query.next())
        return false;
    if (query.value(0).toBool())
    {
        setError(error, QStringLiteral("Merge target no longer matches its finalized parent"));
        return false;
    }
    return true;
}
} // namespace

std::optional<Finalization> Database::mergeObjectRevisions(const OperationContext &actor,
    const QUuid &target_branch_uuid, const QUuid &expected_working_revision_uuid,
    qint64 expected_generation, const QUuid &right_finalized_revision_uuid,
    const QVector<ObjectMergeResolution> &resolutions,
    const QString &title, const QString &message, QString *error)
{
    if (target_branch_uuid.isNull() || expected_working_revision_uuid.isNull() ||
        right_finalized_revision_uuid.isNull() || expected_generation < 0)
    {
        setError(error, QStringLiteral("Invalid merge target or expected generation"));
        return std::nullopt;
    }
    const std::optional<BranchInfo> branch_info = branchUnchecked(target_branch_uuid, error);
    const std::optional<RevisionInfo> working = branch_info
        ? revisionUnchecked(branch_info->head_revision_uuid, error) : std::nullopt;
    if (!working || working->uuid != expected_working_revision_uuid || working->finalized ||
        working->generation != expected_generation || working->parent_revision_uuids.size() != 1)
    {
        setError(error, QStringLiteral("Merge target is stale or not a direct child"));
        return std::nullopt;
    }
    const QUuid left_uuid = working->parent_revision_uuids.at(0);
    const std::optional<RevisionInfo> left = revisionUnchecked(left_uuid, error);
    const std::optional<RevisionInfo> right = revisionUnchecked(right_finalized_revision_uuid, error);
    if (!left || !right || !left->finalized || !right->finalized ||
        left->project_uuid != working->project_uuid || right->project_uuid != working->project_uuid ||
        right->branch_uuid == target_branch_uuid || left_uuid == right_finalized_revision_uuid)
    {
        setError(error, QStringLiteral("Merge requires distinct finalized revisions of the same project"));
        return std::nullopt;
    }
    // The *working* head is the left content input. Its local edits must
    // participate in conflict detection; its finalized parent remains the
    // first cryptographic parent of the eventual merge commit.
    const std::optional<ObjectMergePreview> preview = previewObjectMerge(
        actor, expected_working_revision_uuid, right_finalized_revision_uuid, error);
    if (!preview)
        return std::nullopt;
    if (preview->left_generation != expected_generation ||
        preview->right_generation != right->generation)
    {
        setError(error, QStringLiteral("Merge source changed during preview"));
        return std::nullopt;
    }
    const std::optional<RevisionComparison> ancestry = compareRevisions(
        actor, left_uuid, right_finalized_revision_uuid, error);
    if (!ancestry)
        return std::nullopt;
    if (ancestry->left_is_ancestor_of_right || ancestry->right_is_ancestor_of_left)
    {
        setError(error, QStringLiteral("Merge sources do not diverge; fast-forward is not supported here"));
        return std::nullopt;
    }
    QMap<QUuid, ObjectMergeResolution> selected;
    for (const ObjectMergeResolution &choice : resolutions)
    {
        if (choice.object_uuid.isNull() || selected.contains(choice.object_uuid) ||
            (choice.side != ObjectMergeSide::Left && choice.side != ObjectMergeSide::Right))
        {
            setError(error, QStringLiteral("Invalid or duplicate conflict resolution"));
            return std::nullopt;
        }
        selected.insert(choice.object_uuid, choice);
    }
    QVector<ObjectEdit> edits;
    QSet<QUuid> touched;
    for (const ObjectMergeChange &change : preview->changes)
    {
        const bool conflict = change.kind == ObjectMergeKind::Conflict;
        const QMap<QUuid, ObjectMergeResolution>::const_iterator resolution =
            selected.constFind(change.object_uuid);
        if (conflict != (resolution != selected.cend()))
        {
            setError(error, QStringLiteral("Every conflict requires exactly one explicit resolution"));
            return std::nullopt;
        }
        std::optional<ModelObject> field_merged;
        if (conflict && !resolution.value().field_resolutions.isEmpty())
        {
            // Opaque, deletion and kind-change conflicts cannot be resolved
            // by individual fields. The preview's field names are authoritative.
            if (change.conflicting_fields.isEmpty() || !change.base ||
                !change.left || !change.right ||
                change.base->kind != change.left->kind ||
                change.left->kind != change.right->kind ||
                resolution.value().field_resolutions.size() != change.conflicting_fields.size())
            {
                setError(error, QStringLiteral("Field choices require a typed field conflict"));
                return std::nullopt;
            }
            QMap<QString, bool> field_choices;
            for (const ObjectMergeFieldResolution &field : resolution.value().field_resolutions)
            {
                if (field.field_name.isEmpty() || !change.conflicting_fields.contains(field.field_name) ||
                    field_choices.contains(field.field_name) ||
                    (field.side != ObjectMergeSide::Left && field.side != ObjectMergeSide::Right))
                {
                    setError(error, QStringLiteral("Unknown or duplicate merge field resolution"));
                    return std::nullopt;
                }
                field_choices.insert(field.field_name, field.side == ObjectMergeSide::Right);
            }
            const std::optional<ModelFieldMerge> merged = mergeModelFields(
                change.object_uuid, change.left->kind, change.base->content,
                change.left->content, change.right->content, field_choices);
            if (!merged || !merged->conflicting_fields.isEmpty() || merged->content.isEmpty())
            {
                setError(error, QStringLiteral("Could not synthesize resolved model fields"));
                return std::nullopt;
            }
            field_merged = ModelObject{change.object_uuid, change.left->kind,
                merged->content, change.left->version, false};
        }
        const bool take_merged = change.kind == ObjectMergeKind::FieldsMerged ||
            field_merged.has_value();
        const bool take_right = change.kind == ObjectMergeKind::RightOnly ||
            (conflict && resolution.value().field_resolutions.isEmpty() &&
             resolution.value().side == ObjectMergeSide::Right);
        const std::optional<ModelObject> &chosen = field_merged
            ? field_merged : (change.kind == ObjectMergeKind::FieldsMerged
                ? change.automatically_merged : change.right);
        if (take_merged && !chosen)
        {
            setError(error, QStringLiteral("Missing synthesized field merge content"));
            return std::nullopt;
        }
        if ((!take_right && !take_merged) || sameContent(change.left, chosen))
            continue;
        // Existing object identity rules prohibit changing kind for a UUID.
        // Never silently delete/recreate to work around those constraints.
        if (change.left && chosen && change.left->kind != chosen->kind)
        {
            setError(error, QStringLiteral("Changing object kind during merge is not supported"));
            return std::nullopt;
        }
        ObjectEdit edit;
        edit.object_uuid = change.object_uuid;
        edit.kind = chosen ? chosen->kind : QString();
        if (chosen)
            edit.content = chosen->content;
        edits.append(edit);
        touched.insert(change.object_uuid);
    }
    if (!selected.isEmpty())
    {
        // Reject unknown UUIDs and attempts to resolve nonconflicting edits.
        qsizetype expected_count = 0;
        for (const ObjectMergeChange &change : preview->changes)
            if (change.kind == ObjectMergeKind::Conflict)
                ++expected_count;
        if (selected.size() != expected_count)
        {
            setError(error, QStringLiteral("Conflict resolutions contain unknown object UUIDs"));
            return std::nullopt;
        }
    }

    if (!beginWrite(error))
        return std::nullopt;
    const auto fail = [this]() -> std::optional<Finalization> {
        rollback();
        return std::nullopt;
    };
    if (!requirePermission(actor, QStringLiteral("project.model.write"),
                           AccessScope::branch(target_branch_uuid), error) ||
        !requirePermission(actor, QStringLiteral("project.revision.finalize"),
                           AccessScope::branch(target_branch_uuid), error) ||
        !requirePermission(actor, QStringLiteral("project.read"),
                           AccessScope::branch(left->branch_uuid), error) ||
        !requirePermission(actor, QStringLiteral("project.read"),
                           AccessScope::branch(right->branch_uuid), error))
        return fail();
    const std::optional<RevisionInfo> merge_base =
        revisionUnchecked(preview->base_revision_uuid, error);
    if (!merge_base || !merge_base->finalized ||
        !requirePermission(actor, QStringLiteral("project.read"),
                           AccessScope::branch(merge_base->branch_uuid), error))
        return fail();

    const std::optional<BranchInfo> latest_branch = branchUnchecked(target_branch_uuid, error);
    const std::optional<RevisionInfo> latest = latest_branch
        ? revisionUnchecked(latest_branch->head_revision_uuid, error) : std::nullopt;
    if (!latest || latest->uuid != expected_working_revision_uuid || latest->generation != expected_generation ||
        latest->finalized || latest->parent_revision_uuids.size() != 1 ||
        latest->parent_revision_uuids.at(0) != left_uuid)
    {
        setError(error, QStringLiteral("Merge target changed since preview"));
        return fail();
    }
    const std::optional<int> mode = objectMode(latest->uuid, error);
    if (!mode || *mode != 1)
    {
        if (mode && *mode != 1)
            setError(error, QStringLiteral("Merge target must use canonical objects"));
        return fail();
    }
    // Check the full physical state under the write lock against the state
    // captured with the preview. This catches non-journal SQL modifications
    // even if they bypass revision generation counters.
    const std::optional<QByteArray> locked_state = mergePhysicalObjectDigest(
        this->database_, *this->backend_, latest->uuid, error);
    if (!locked_state || *locked_state != preview->left_physical_sha256)
    {
        if (locked_state)
            setError(error, QStringLiteral("Merge target object state changed since preview"));
        return fail();
    }
    // Preserve V38's stricter invariant for generation-zero heads: no
    // unjournaled physical difference from the finalized parent is allowed.
    if (latest->generation == 0 &&
        !samePhysicalObjects(this->database_, *this->backend_, latest->uuid, left_uuid, error))
        return fail();
    QSqlQuery lookup(this->database_);
    if (!lookup.prepare(QStringLiteral(
            "SELECT version,kind FROM revision_objects WHERE revision_uuid=? AND object_uuid=?")))
    {
        setError(error, lookup.lastError().text());
        return fail();
    }
    for (ObjectEdit &edit : edits)
    {
        lookup.bindValue(0, this->backend_->bindUuid(latest->uuid));
        lookup.bindValue(1, this->backend_->bindUuid(edit.object_uuid));
        if (!execute(lookup, error))
            return fail();
        edit.expected_version = -1;
        if (lookup.next())
        {
            edit.expected_version = lookup.value(0).toLongLong();
            if (edit.content && lookup.value(1).toString() != edit.kind)
            {
                setError(error, QStringLiteral("Object kind cannot change on merge or tombstone resurrection"));
                return fail();
            }
        }
        lookup.finish();
        if (!modifyObjectInTransaction(actor, target_branch_uuid, edit.object_uuid,
                                       edit.kind, edit.content, edit.expected_version, error))
            return fail();
    }
    if (!validateTouchedHydraulicNetworks(latest->uuid, touched, error))
        return fail();
    if (!edits.isEmpty() && !writeObjectGroupInTransaction(*latest, actor, QStringLiteral("edit"),
                                                            QUuid(), expected_generation, error))
        return fail();
    QSqlQuery parent(this->database_);
    if (!parent.prepare(QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) VALUES (?,1,?)")))
    {
        setError(error, parent.lastError().text());
        return fail();
    }
    parent.addBindValue(this->backend_->bindUuid(latest->uuid));
    parent.addBindValue(this->backend_->bindUuid(right_finalized_revision_uuid));
    if (!execute(parent, error))
        return fail();
    const std::optional<RevisionInfo> after = revisionUnchecked(latest->uuid, error);
    if (!after)
        return fail();
    const std::optional<Finalization> result = finalizeInWriteTransaction(actor.actor_uuid,
        target_branch_uuid, after->generation, title, message, error);
    if (!result || !commit(error))
        return fail();
    return result;
}

} // namespace aowis::db
