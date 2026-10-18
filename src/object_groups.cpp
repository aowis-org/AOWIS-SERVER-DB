#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include "storage_backend.h"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
void fail(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

bool prepare(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.prepare(sql))
        return true;
    fail(error, query.lastError().text());
    return false;
}

bool run(QSqlQuery &query, QString *error)
{
    if (query.exec())
        return true;
    fail(error, query.lastError().text());
    return false;
}

struct StoredEdit
{
    QUuid object_uuid;
    QString kind;
    QByteArray before_bytes;
    QByteArray after_bytes;
    bool before_null = true;
    bool after_null = true;
    bool before_deleted = true;
};

} // namespace

std::optional<QUuid> Database::writeObjectGroupInTransaction(const RevisionInfo &revision,
                                 const OperationContext &actor, const QString &action,
                                 const QUuid &target_uuid, qint64 before_generation,
                                 QString *error)
{
    const std::optional<RevisionInfo> after = revisionUnchecked(revision.uuid, error);
    if (!after || after->finalized || after->generation <= before_generation)
    {
        fail(error, QStringLiteral("Empty or invalid object group"));
        return std::nullopt;
    }
    const QUuid group_uuid = createUuidV7();
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "INSERT INTO object_groups(uuid,revision_uuid,actor_uuid,action,target_group_uuid,"
            "first_sequence,last_sequence,occurred_at_ms) VALUES(?,?,?,?,?,?,?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(group_uuid));
    query.addBindValue(this->backend_->bindUuid(revision.uuid));
    query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
    query.addBindValue(action);
    query.addBindValue(target_uuid.isNull() ? QVariant() : this->backend_->bindUuid(target_uuid));
    query.addBindValue(before_generation + 1);
    query.addBindValue(after->generation);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!run(query, error))
        return std::nullopt;

    QSqlQuery member_events(this->database_);
    if (!prepare(member_events, QStringLiteral(
            "SELECT uuid FROM object_events WHERE revision_uuid=? AND sequence>? AND sequence<=? "
            "ORDER BY sequence"), error))
        return std::nullopt;
    member_events.addBindValue(this->backend_->bindUuid(revision.uuid));
    member_events.addBindValue(before_generation);
    member_events.addBindValue(after->generation);
    if (!run(member_events, error))
        return std::nullopt;
    qint64 position = 0;
    while (member_events.next())
    {
        QSqlQuery insert(this->database_);
        if (!prepare(insert, QStringLiteral(
                "INSERT INTO object_group_events(group_uuid,position,object_event_uuid) VALUES(?,?,?)"), error))
            return std::nullopt;
        // Convert through the backend: SQLite returns RFC-4122 BLOBs, while
        // PostgreSQL returns native UUID values. Never forward a raw driver
        // QVariant from one query as an unvalidated UUID parameter.
        const QUuid event_uuid = this->backend_->readUuid(member_events.value(0));
        if (member_events.value(0).isNull() || event_uuid.isNull())
        {
            fail(error, QStringLiteral("Invalid object-event UUID in group journal"));
            return std::nullopt;
        }
        insert.addBindValue(this->backend_->bindUuid(group_uuid));
        insert.addBindValue(position);
        insert.addBindValue(this->backend_->bindUuid(event_uuid));
        if (!run(insert, error))
            return std::nullopt;
        ++position;
    }
    if (position != after->generation - before_generation)
    {
        fail(error, QStringLiteral("Group does not cover all committed object edits"));
        return std::nullopt;
    }
    QSqlQuery format(this->database_);
    if (!prepare(format, QStringLiteral(
            "UPDATE revisions SET object_journal_version=2 WHERE uuid=? AND state='working'"), error))
        return std::nullopt;
    format.addBindValue(this->backend_->bindUuid(revision.uuid));
    if (!run(format, error) || format.numRowsAffected() != 1)
        return std::nullopt;
    return group_uuid;
}

std::optional<QUuid> Database::commitObjectGroup(const OperationContext &actor,
                  const QUuid &branch_uuid, qint64 expected_generation,
                  const QVector<ObjectEdit> &edits, QString *error)
{
    if (edits.isEmpty())
    {
        fail(error, QStringLiteral("Object group cannot be empty"));
        return std::nullopt;
    }
    QSet<QUuid> ids;
    for (const ObjectEdit &edit : edits)
    {
        if (edit.object_uuid.isNull() || ids.contains(edit.object_uuid))
        {
            fail(error, QStringLiteral("Duplicate or null object UUID in group"));
            return std::nullopt;
        }
        ids.insert(edit.object_uuid);
    }
    if (!beginWrite(error))
        return std::nullopt;
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> r = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!r || r->finalized || r->generation != expected_generation)
    {
        fail(error, QStringLiteral("Object group revision generation conflict"));
        rollback();
        return std::nullopt;
    }
    bool ok = true;
    for (const ObjectEdit &edit : edits)
    {
        if (!modifyObjectInTransaction(actor, branch_uuid, edit.object_uuid, edit.kind,
                                       edit.content, edit.expected_version, error))
        {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = validateTouchedHydraulicNetworks(r->uuid, ids, error);
    std::optional<QUuid> id;
    if (ok)
        id = writeObjectGroupInTransaction(*r, actor, QStringLiteral("edit"), QUuid(), expected_generation, error);
    if (id)
        ok = commit(error);
    if (!ok || !id)
    {
        rollback();
        return std::nullopt;
    }
    return id;
}

QVector<ObjectGroup> Database::objectGroupsForRevision(const OperationContext &actor,
                                  const QUuid &revision_uuid, QString *error) const
{
    QVector<ObjectGroup> result;
    const std::optional<RevisionInfo> r = revisionUnchecked(revision_uuid, error);
    if (!r || !requirePermission(actor, QStringLiteral("project.history.read"),
                                 AccessScope::branch(r->branch_uuid), error))
        return result;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,actor_uuid,action,target_group_uuid,first_sequence,last_sequence,occurred_at_ms "
            "FROM object_groups WHERE revision_uuid=? ORDER BY first_sequence"), error))
        return result;
    query.addBindValue(this->backend_->bindUuid(revision_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
    {
        ObjectGroup group;
        group.uuid = this->backend_->readUuid(query.value(0));
        group.revision_uuid = revision_uuid;
        group.actor_uuid = this->backend_->readUuid(query.value(1));
        group.action = query.value(2).toString();
        group.target_group_uuid = query.value(3).isNull() ? QUuid() : this->backend_->readUuid(query.value(3));
        group.first_sequence = query.value(4).toLongLong();
        group.last_sequence = query.value(5).toLongLong();
        group.occurred_at_ms = query.value(6).toLongLong();
        QSqlQuery links(this->database_);
        if (!prepare(links, QStringLiteral(
                "SELECT object_event_uuid FROM object_group_events WHERE group_uuid=? ORDER BY position"), error))
            return {};
        links.addBindValue(this->backend_->bindUuid(group.uuid));
        if (!run(links, error))
            return {};
        while (links.next())
            group.object_event_uuids.append(this->backend_->readUuid(links.value(0)));
        result.append(group);
    }
    return result;
}

std::optional<QUuid> Database::undoObjectGroup(const OperationContext &actor,
                 const QUuid &branch_uuid, qint64 expected_generation, QString *error)
{
    return compensateObjectGroup(actor, branch_uuid, expected_generation, false, error);
}

std::optional<QUuid> Database::redoObjectGroup(const OperationContext &actor,
                 const QUuid &branch_uuid, qint64 expected_generation, QString *error)
{
    return compensateObjectGroup(actor, branch_uuid, expected_generation, true, error);
}

std::optional<QUuid> Database::compensateObjectGroup(const OperationContext &actor,
                  const QUuid &branch_uuid, qint64 expected_generation, bool redo,
                  QString *error)
{
    if (!beginWrite(error))
        return std::nullopt;
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> r = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!r || r->finalized || r->generation != expected_generation ||
        !requirePermission(actor, QStringLiteral("project.model.write"),
                           AccessScope::branch(branch_uuid), error))
    {
        fail(error, QStringLiteral("Undo/redo denied or revision generation conflict"));
        rollback();
        return std::nullopt;
    }
    const QVector<ObjectGroup> groups = objectGroupsForRevision(actor, r->uuid, error);
    QVector<QUuid> applied;
    QVector<QUuid> undone;
    QHash<QUuid, ObjectGroup> original;
    for (const ObjectGroup &group : groups)
    {
        if (group.action == QStringLiteral("edit"))
        {
            applied.append(group.uuid);
            undone.clear();
            original.insert(group.uuid, group);
        }
        else if (group.action == QStringLiteral("undo"))
        {
            if (applied.isEmpty() || applied.last() != group.target_group_uuid)
            {
                fail(error, QStringLiteral("Invalid persistent undo chain"));
                rollback();
                return std::nullopt;
            }
            applied.removeLast();
            undone.append(group.target_group_uuid);
        }
        else if (group.action == QStringLiteral("redo"))
        {
            if (undone.isEmpty() || undone.last() != group.target_group_uuid)
            {
                fail(error, QStringLiteral("Invalid persistent redo chain"));
                rollback();
                return std::nullopt;
            }
            undone.removeLast();
            applied.append(group.target_group_uuid);
        }
    }
    const QVector<QUuid> &stack = redo ? undone : applied;
    if (stack.isEmpty())
    {
        fail(error, QStringLiteral("No object group available to undo/redo"));
        rollback();
        return std::nullopt;
    }
    const QUuid target = stack.last();
    const ObjectGroup group = original.value(target);
    if (group.object_event_uuids.isEmpty())
    {
        fail(error, QStringLiteral("Missing original object group"));
        rollback();
        return std::nullopt;
    }
    QVector<StoredEdit> edits;
    QSet<QUuid> touched_uuids;
    for (const QUuid &event_id : group.object_event_uuids)
    {
        QSqlQuery query(this->database_);
        if (!prepare(query, QStringLiteral(
                "SELECT object_uuid,kind,before_content,after_content,before_deleted "
                "FROM object_events WHERE uuid=?"), error))
        {
            rollback();
            return std::nullopt;
        }
        query.addBindValue(this->backend_->bindUuid(event_id));
        if (!run(query, error) || !query.next())
        {
            fail(error, QStringLiteral("Missing original object event"));
            rollback();
            return std::nullopt;
        }
        touched_uuids.insert(this->backend_->readUuid(query.value(0)));
        edits.append(StoredEdit{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                 query.value(2).toByteArray(), query.value(3).toByteArray(),
                 query.value(2).isNull(), query.value(3).isNull(), query.value(4).toBool()});
    }
    // For undo, reverse the order of constituent object edits. Each object's
    // actual state must match the expected state before the inverse is applied.
    for (qsizetype i = 0; i < edits.size(); ++i)
    {
        const StoredEdit &edit = redo ? edits.at(i) : edits.at(edits.size() - 1 - i);
        QSqlQuery current(this->database_);
        if (!prepare(current, QStringLiteral(
                "SELECT version,deleted,content FROM revision_objects "
                "WHERE revision_uuid=? AND object_uuid=?"), error))
        {
            rollback();
            return std::nullopt;
        }
        current.addBindValue(this->backend_->bindUuid(r->uuid));
        current.addBindValue(this->backend_->bindUuid(edit.object_uuid));
        if (!run(current, error) || !current.next())
        {
            fail(error, QStringLiteral("Undo/redo object missing"));
            rollback();
            return std::nullopt;
        }
        const qint64 actual_version = current.value(0).toLongLong();
        const bool actual_deleted = current.value(1).toBool();
        const QByteArray actual_content = current.value(2).toByteArray();
        const bool expected_deleted = redo ? edit.before_deleted : edit.after_null;
        const QByteArray expected_content = redo ? edit.before_bytes : edit.after_bytes;
        if (actual_deleted != expected_deleted || (!expected_deleted && actual_content != expected_content))
        {
            fail(error, QStringLiteral("Undo/redo object conflict"));
            rollback();
            return std::nullopt;
        }
        const bool desired_deleted = redo ? edit.after_null : edit.before_deleted;
        const QByteArray desired_bytes = redo ? edit.after_bytes : edit.before_bytes;
        current.finish();
        const std::optional<QByteArray> desired_content = desired_deleted
            ? std::optional<QByteArray>{} : std::optional<QByteArray>{desired_bytes};
        if (!modifyObjectInTransaction(actor, branch_uuid, edit.object_uuid, edit.kind,
                                       desired_content, actual_version, error))
        {
            rollback();
            return std::nullopt;
        }
    }
    if (!validateTouchedHydraulicNetworks(r->uuid, touched_uuids, error))
    {
        rollback();
        return std::nullopt;
    }
    const std::optional<QUuid> action_id = writeObjectGroupInTransaction(
        *r, actor, redo ? QStringLiteral("redo") : QStringLiteral("undo"),
        target, expected_generation, error);
    if (!action_id || !commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return target;
}

} // namespace aowis::db
