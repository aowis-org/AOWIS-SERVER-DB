#include <aowis/db/database.h>
#include "object_merge_state.h"
#include "model_field_merge.h"

#include <algorithm>

#include <QByteArrayView>
#include <QCryptographicHash>

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

// Load live entity state only. An absent row and a deletion tombstone both
// represent absence for semantic three-way comparison. Tombstone versions
// remain in SQLite for optimistic concurrency and are never modified here.
bool readLiveObjects(const QSqlDatabase &database, const QUuid &revision_uuid,
                     QMap<QUuid, ModelObject> &objects, QString *error)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version FROM revision_objects "
            "WHERE revision_uuid=? AND deleted=0 ORDER BY object_uuid")))
    {
        setError(error, query.lastError().text());
        return false;
    }
    query.addBindValue(revision_uuid.toRfc4122());
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return false;
    }
    while (query.next())
    {
        const QUuid object_uuid = QUuid::fromRfc4122(query.value(0).toByteArray());
        if (object_uuid.isNull() || objects.contains(object_uuid))
        {
            setError(error, QStringLiteral("Invalid or duplicate object UUID in revision"));
            return false;
        }
        objects.insert(object_uuid, ModelObject{object_uuid, query.value(1).toString(),
            query.value(2).toByteArray(), query.value(3).toLongLong(), false});
    }
    if (query.lastError().isValid())
    {
        setError(error, query.lastError().text());
        return false;
    }
    return true;
}

std::optional<ModelObject> findObject(const QMap<QUuid, ModelObject> &objects,
                                      const QUuid &uuid)
{
    const QMap<QUuid, ModelObject>::const_iterator it = objects.constFind(uuid);
    if (it == objects.cend())
        return std::nullopt;
    return *it;
}

bool sameContent(const std::optional<ModelObject> &a, const std::optional<ModelObject> &b)
{
    if (!a || !b)
        return !a && !b;
    return a->kind == b->kind && a->content == b->content;
}

} // namespace

std::optional<QByteArray> mergePhysicalObjectDigest(const QSqlDatabase &database,
    const QUuid &revision_uuid, QString *error)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? ORDER BY object_uuid")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(revision_uuid.toRfc4122());
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Length-prefixed fields prevent ambiguous concatenations. All physical
    // rows are included (even deleted ones), unlike the semantic preview.
    const auto addField = [&hash](const QByteArray &value) {
        hash.addData(QByteArray::number(value.size()));
        hash.addData(QByteArrayView(":", 1));
        hash.addData(value);
    };
    while (query.next())
    {
        addField(query.value(0).toByteArray());
        addField(query.value(1).toString().toUtf8());
        addField(query.value(2).toByteArray());
        addField(QByteArray::number(query.value(3).toLongLong()));
        addField(QByteArray::number(query.value(4).toInt()));
    }
    if (query.lastError().isValid())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    return hash.result();
}

std::optional<ObjectMergePreview> Database::previewObjectMerge(const OperationContext &actor,
    const QUuid &left_revision_uuid, const QUuid &right_revision_uuid, QString *error) const
{
    // A single read transaction produces a coherent SQLite snapshot even if
    // another connection modifies either editable working head concurrently.
    QSqlQuery transaction(this->database_);
    if (!transaction.exec(QStringLiteral("BEGIN")))
    {
        setError(error, transaction.lastError().text());
        return std::nullopt;
    }
    const auto fail = [this]() -> std::optional<ObjectMergePreview> {
        QSqlQuery rollback(this->database_);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return std::nullopt;
    };

    const std::optional<RevisionComparison> ancestry = compareRevisions(
        actor, left_revision_uuid, right_revision_uuid, error);
    if (!ancestry)
        return fail();
    if (ancestry->merge_base_revision_uuids.size() != 1)
    {
        setError(error, QStringLiteral(
            "Object merge preview requires exactly one finalized merge base; "
            "multiple bases need recursive merging"));
        return fail();
    }
    const QUuid base_uuid = ancestry->merge_base_revision_uuids.first();

    // compareRevisions checks both tips; the base might belong to a third
    // branch with independent read permissions. Do not leak its contents.
    const std::optional<RevisionInfo> base = revision(actor, base_uuid, error);
    if (!base || !base->finalized || base->project_uuid != ancestry->project_uuid)
    {
        if (base && error != nullptr)
            *error = QStringLiteral("Merge base is not a finalized revision in this project");
        return fail();
    }
    const std::optional<RevisionInfo> left_revision = revision(actor, left_revision_uuid, error);
    const std::optional<RevisionInfo> right_revision = revision(actor, right_revision_uuid, error);
    if (!left_revision || !right_revision)
        return fail();
    const std::optional<int> base_mode = objectMode(base_uuid, error);
    const std::optional<int> left_mode = objectMode(left_revision_uuid, error);
    const std::optional<int> right_mode = objectMode(right_revision_uuid, error);
    if (!base_mode || !left_mode || !right_mode)
        return fail();
    if (*base_mode != 1 || *left_mode != 1 || *right_mode != 1)
    {
        setError(error, QStringLiteral(
            "Object merge preview requires object-mode base and source revisions; "
            "legacy opaque snapshots cannot be compared by entity"));
        return fail();
    }

    QMap<QUuid, ModelObject> base_objects;
    QMap<QUuid, ModelObject> left_objects;
    QMap<QUuid, ModelObject> right_objects;
    if (!readLiveObjects(this->database_, base_uuid, base_objects, error) ||
        !readLiveObjects(this->database_, left_revision_uuid, left_objects, error) ||
        !readLiveObjects(this->database_, right_revision_uuid, right_objects, error))
        return fail();

    QSet<QUuid> ids;
    for (QMap<QUuid, ModelObject>::const_iterator it = base_objects.cbegin();
         it != base_objects.cend(); ++it)
        ids.insert(it.key());
    for (QMap<QUuid, ModelObject>::const_iterator it = left_objects.cbegin();
         it != left_objects.cend(); ++it)
        ids.insert(it.key());
    for (QMap<QUuid, ModelObject>::const_iterator it = right_objects.cbegin();
         it != right_objects.cend(); ++it)
        ids.insert(it.key());
    QVector<QUuid> sorted_ids = ids.values();
    std::sort(sorted_ids.begin(), sorted_ids.end(),
              [](const QUuid &a, const QUuid &b) { return a.toRfc4122() < b.toRfc4122(); });

    // Captured inside the same read transaction as the three-way comparison:
    // an unjournaled update cannot silently alter the working head between
    // this preview and a subsequent write transaction.
    const std::optional<QByteArray> physical = mergePhysicalObjectDigest(
        this->database_, left_revision_uuid, error);
    if (!physical)
        return fail();

    ObjectMergePreview result;
    result.left_physical_sha256 = *physical;
    result.project_uuid = ancestry->project_uuid;
    result.base_revision_uuid = base_uuid;
    result.left_revision_uuid = left_revision_uuid;
    result.right_revision_uuid = right_revision_uuid;
    result.left_generation = left_revision->generation;
    result.right_generation = right_revision->generation;
    for (const QUuid &uuid : sorted_ids)
    {
        const std::optional<ModelObject> baseline = findObject(base_objects, uuid);
        const std::optional<ModelObject> left = findObject(left_objects, uuid);
        const std::optional<ModelObject> right = findObject(right_objects, uuid);
        const bool left_changed = !sameContent(baseline, left);
        const bool right_changed = !sameContent(baseline, right);
        if (!left_changed && !right_changed)
            continue;

        ObjectMergeKind kind = ObjectMergeKind::Conflict;
        if (!left_changed)
            kind = ObjectMergeKind::RightOnly;
        else if (!right_changed)
            kind = ObjectMergeKind::LeftOnly;
        else if (sameContent(left, right))
            kind = ObjectMergeKind::BothSame;
        ObjectMergeChange change{uuid, kind, baseline, left, right, {}, std::nullopt};
        if (kind == ObjectMergeKind::Conflict && baseline && left && right &&
            baseline->kind == left->kind && left->kind == right->kind)
        {
            const std::optional<ModelFieldMerge> fields = mergeModelFields(
                uuid, left->kind, baseline->content, left->content, right->content);
            if (fields)
            {
                change.conflicting_fields = fields->conflicting_fields;
                if (fields->conflicting_fields.isEmpty())
                {
                    kind = ObjectMergeKind::FieldsMerged;
                    change.kind = kind;
                    change.automatically_merged = ModelObject{
                        uuid, left->kind, fields->content, left->version, false};
                }
            }
        }
        if (kind == ObjectMergeKind::Conflict)
            ++result.conflict_count;
        result.changes.append(std::move(change));
    }
    QSqlQuery commit(this->database_);
    if (!commit.exec(QStringLiteral("COMMIT")))
    {
        setError(error, commit.lastError().text());
        return fail();
    }
    return result;
}

} // namespace aowis::db
