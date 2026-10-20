#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include "storage_backend.h"
#include <aowis/db/revision_manifest.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
QByteArray uuidBytes(const QUuid &uuid)
{
    return uuid.toRfc4122();
}

void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

bool prepare(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.prepare(sql))
        return true;
    setError(error, query.lastError().text());
    return false;
}

bool run(QSqlQuery &query, QString *error)
{
    if (query.exec())
        return true;
    setError(error, query.lastError().text());
    return false;
}

void appendInt64(QByteArray &bytes, quint64 number)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.append(static_cast<char>((number >> shift) & 0xffU));
}

void appendField(QByteArray &bytes, const QByteArray &value)
{
    appendInt64(bytes, static_cast<quint64>(value.size()));
    bytes.append(value);
}

struct HistoryRow
{
    HistoryEvent event;
    QByteArray before;
    QByteArray after;
    bool after_present = false;
};

std::optional<QVector<HistoryRow>> loadRows(const QSqlDatabase &database,
                                             const StorageBackend &backend,
                                             const QUuid &revision_uuid, QString *error)
{
    QVector<HistoryRow> result;
    QSqlQuery query(database);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,actor_uuid,sequence,occurred_at_ms,kind,target_event_uuid,"
            "before_present,before_snapshot,after_snapshot,after_present FROM operation_events "
            "WHERE revision_uuid=? ORDER BY sequence"), error))
        return std::nullopt;
    query.addBindValue(backend.bindUuid(revision_uuid));
    if (!run(query, error))
        return std::nullopt;
    while (query.next())
    {
        HistoryRow row;
        row.event.uuid = backend.readUuid(query.value(0));
        row.event.revision_uuid = revision_uuid;
        row.event.actor_uuid = backend.readUuid(query.value(1));
        row.event.sequence = query.value(2).toLongLong();
        row.event.occurred_at_ms = query.value(3).toLongLong();
        row.event.kind = query.value(4).toString();
        row.event.target_event_uuid = backend.readUuid(query.value(5));
        row.event.had_previous_snapshot = query.value(6).toBool();
        row.before = query.value(7).toByteArray();
        row.after = query.value(8).toByteArray();
        row.after_present = query.value(9).toBool();
        if (row.event.had_previous_snapshot)
            row.event.before_sha256 = QCryptographicHash::hash(row.before, QCryptographicHash::Sha256);
        if (row.after_present)
            row.event.after_sha256 = QCryptographicHash::hash(row.after, QCryptographicHash::Sha256);
        result.append(row);
    }
    return result;
}

struct Stacks
{
    QVector<QUuid> applied;
    QVector<QUuid> redo;
    QHash<QUuid, HistoryRow> replacements;
};

std::optional<Stacks> replay(const QVector<HistoryRow> &rows, QString *error)
{
    Stacks result;
    for (const HistoryRow &row : rows)
    {
        if (row.event.kind == QStringLiteral("replace"))
        {
            result.applied.append(row.event.uuid);
            result.redo.clear();
            result.replacements.insert(row.event.uuid, row);
        }
        else if (row.event.kind == QStringLiteral("undo"))
        {
            if (result.applied.isEmpty() || result.applied.constLast() != row.event.target_event_uuid)
            {
                setError(error, QStringLiteral("Invalid undo history ordering"));
                return std::nullopt;
            }
            result.applied.removeLast();
            result.redo.append(row.event.target_event_uuid);
        }
        else if (row.event.kind == QStringLiteral("redo"))
        {
            if (result.redo.isEmpty() || result.redo.constLast() != row.event.target_event_uuid)
            {
                setError(error, QStringLiteral("Invalid redo history ordering"));
                return std::nullopt;
            }
            result.redo.removeLast();
            result.applied.append(row.event.target_event_uuid);
        }
        else
        {
            setError(error, QStringLiteral("Unsupported operation kind"));
            return std::nullopt;
        }
    }
    return result;
}

} // namespace

// An untrusted snapshot write is never performed directly: it must pass
// permission and generation checks, and use this helper inside the SAME SQLite
// transaction as the immutable history record.
bool Database::applySnapshotOperation(const RevisionInfo &revision, const QUuid &actor_uuid,
                                      const std::optional<QByteArray> &new_snapshot,
                                      const QString &kind, const QUuid &target_uuid,
                                      QString *error)
{
    QSqlQuery old(this->database_);
    if (!prepare(old, QStringLiteral("SELECT content FROM snapshots WHERE revision_uuid=?"), error))
        return false;
    old.addBindValue(this->backend_->bindUuid(revision.uuid));
    if (!run(old, error))
        return false;
    std::optional<QByteArray> previous;
    if (old.next())
        previous = old.value(0).toByteArray();
    old.finish();
    if (previous == new_snapshot)
    {
        setError(error, QStringLiteral("Operation does not change the snapshot"));
        return false;
    }

    QSqlQuery query(this->database_);
    bool ok = false;
    if (new_snapshot)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO snapshots(revision_uuid,format_version,content) VALUES (?,1,?) "
            "ON CONFLICT(revision_uuid) DO UPDATE SET content=excluded.content"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(revision.uuid));
            query.addBindValue(*new_snapshot);
            ok = run(query, error);
        }
    }
    else
    {
        ok = prepare(query, QStringLiteral("DELETE FROM snapshots WHERE revision_uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(revision.uuid));
            ok = run(query, error);
        }
    }
    if (!ok)
        return false;

    ok = prepare(query, QStringLiteral(
        "UPDATE revisions SET generation=generation+1 WHERE uuid=? AND state='working' AND generation=?"), error);
    if (ok)
    {
        query.addBindValue(this->backend_->bindUuid(revision.uuid));
        query.addBindValue(revision.generation);
        ok = run(query, error) && query.numRowsAffected() == 1;
    }
    if (!ok)
    {
        setError(error, QStringLiteral("Working revision generation changed"));
        return false;
    }

    ok = prepare(query, QStringLiteral(
        "INSERT INTO operation_events (uuid,project_uuid,branch_uuid,revision_uuid,sequence,actor_uuid,"
        "occurred_at_ms,kind,target_event_uuid,before_present,before_snapshot,after_snapshot,after_present) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"), error);
    if (ok)
    {
        query.addBindValue(this->backend_->bindUuid(createUuidV7()));
        query.addBindValue(this->backend_->bindUuid(revision.project_uuid));
        query.addBindValue(this->backend_->bindUuid(revision.branch_uuid));
        query.addBindValue(this->backend_->bindUuid(revision.uuid));
        query.addBindValue(revision.generation + 1);
        query.addBindValue(this->backend_->bindUuid(actor_uuid));
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        query.addBindValue(kind);
        query.addBindValue(target_uuid.isNull() ? QVariant() : this->backend_->bindUuid(target_uuid));
        query.addBindValue(previous.has_value() ? 1 : 0);
        query.addBindValue(previous ? QVariant(*previous) : QVariant());
        query.addBindValue(new_snapshot ? QVariant(*new_snapshot) : QVariant());
        query.addBindValue(new_snapshot.has_value() ? 1 : 0);
        ok = run(query, error);
    }
    return ok;
}

std::optional<QByteArray> Database::operationJournalBytes(const QUuid &revision_uuid, QString *error) const
{
    const std::optional<QVector<HistoryRow>> rows = loadRows(this->database_, *this->backend_, revision_uuid, error);
    if (!rows)
        return std::nullopt;
    QByteArray bytes("AOWIS-OPERATIONS-V1\0", 20);
    appendInt64(bytes, static_cast<quint64>(rows->size()));
    for (const HistoryRow &row : *rows)
    {
        bytes.append(uuidBytes(row.event.uuid));
        bytes.append(uuidBytes(row.event.actor_uuid));
        bytes.append(uuidBytes(row.event.revision_uuid));
        appendInt64(bytes, static_cast<quint64>(row.event.sequence));
        appendInt64(bytes, static_cast<quint64>(row.event.occurred_at_ms));
        appendField(bytes, row.event.kind.toUtf8());
        bytes.append(uuidBytes(row.event.target_event_uuid));
        bytes.append(row.event.had_previous_snapshot ? '\x01' : '\x00');
        if (row.event.had_previous_snapshot)
            appendField(bytes, row.before);
        bytes.append(row.after_present ? '\x01' : '\x00');
        if (row.after_present)
            appendField(bytes, row.after);
    }
    return bytes;
}

std::optional<QByteArray> Database::historyDigest(const QUuid &revision_uuid,
                                                   QString *error) const
{
    const std::optional<QByteArray> bytes = operationJournalBytes(revision_uuid, error);
    if (!bytes)
        return std::nullopt;
    return QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256);
}

std::optional<QByteArray> Database::exportRevisionOperationJournal(
    const OperationContext &actor, const QUuid &revision_uuid, QString *error) const
{
    const std::optional<QByteArray> encoded = exportRevisionManifest(
        actor, revision_uuid, error);
    if (!encoded)
        return std::nullopt;
    const std::optional<RevisionManifest> manifest =
        decodeRevisionManifest(*encoded, error);
    if (!manifest || !requirePermission(actor, QStringLiteral("project.history.read"),
                                       AccessScope::branch(manifest->branch_uuid), error))
        return std::nullopt;
    const std::optional<QByteArray> bytes = operationJournalBytes(revision_uuid, error);
    if (!bytes || !verifyRevisionOperationJournal(*bytes, revision_uuid,
                                                   manifest->operation_history_sha256, error))
        return std::nullopt;
    return bytes;
}

QVector<HistoryEvent> Database::history(const OperationContext &actor,
                                        const QUuid &branch_uuid, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("project.history.read"), AccessScope::branch(branch_uuid), error))
        return {};
    const std::optional<BranchInfo> branch_info = branchUnchecked(branch_uuid, error);
    if (!branch_info)
        return {};
    const std::optional<RevisionInfo> revision_info = revisionUnchecked(branch_info->head_revision_uuid, error);
    if (!revision_info)
        return {};
    const std::optional<QVector<HistoryRow>> rows = loadRows(this->database_, *this->backend_, revision_info->uuid, error);
    if (!rows)
        return {};
    QVector<HistoryEvent> result;
    for (const HistoryRow &row : *rows)
        result.append(row.event);
    return result;
}

QVector<HistoryEvent> Database::historyForRevision(const OperationContext &actor,
                                                   const QUuid &revision_uuid, QString *error) const
{
    const std::optional<RevisionInfo> revision_info = revisionUnchecked(revision_uuid, error);
    if (!revision_info)
        return {};
    if (!requirePermission(actor, QStringLiteral("project.history.read"),
                           AccessScope::branch(revision_info->branch_uuid), error))
        return {};
    const std::optional<QVector<HistoryRow>> rows = loadRows(this->database_, *this->backend_, revision_uuid, error);
    if (!rows)
        return {};
    QVector<HistoryEvent> result;
    for (const HistoryRow &row : *rows)
        result.append(row.event);
    return result;
}

namespace
{
// Call only inside a transaction that already verified the current generation.
std::optional<QUuid> performReversal(const std::optional<Stacks> &stacks,
                                     bool is_undo, QString *error)
{
    if (!stacks)
        return std::nullopt;
    const QVector<QUuid> &candidates = is_undo ? stacks->applied : stacks->redo;
    if (candidates.isEmpty())
    {
        setError(error, is_undo ? QStringLiteral("Nothing to undo") : QStringLiteral("Nothing to redo"));
        return std::nullopt;
    }
    return candidates.constLast();
}
} // namespace

std::optional<QUuid> Database::undoLastChange(const OperationContext &actor,
                                               const QUuid &branch_uuid,
                                               qint64 expected_generation,
                                               QString *error)
{
    if (!beginWrite(error))
        return std::nullopt;
    if (!requirePermission(actor, QStringLiteral("project.model.write"), AccessScope::branch(branch_uuid), error))
    {
        rollback();
        return std::nullopt;
    }
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> revision_info = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!revision_info || revision_info->finalized || revision_info->generation != expected_generation)
    {
        setError(error, QStringLiteral("Working revision generation conflict"));
        rollback();
        return std::nullopt;
    }
    const std::optional<int> mode = objectMode(revision_info->uuid, error);
    if (!mode || *mode != 0)
    {
        setError(error, QStringLiteral("Object-mode revisions have a separate history; snapshot undo is unavailable"));
        rollback();
        return std::nullopt;
    }
    const std::optional<QVector<HistoryRow>> rows = loadRows(this->database_, *this->backend_, revision_info->uuid, error);
    const std::optional<Stacks> stacks = rows ? replay(*rows, error) : std::nullopt;
    const std::optional<QUuid> target = performReversal(stacks, true, error);
    if (!target)
    {
        rollback();
        return std::nullopt;
    }
    const HistoryRow original = stacks->replacements.value(*target);
    // The latest applied edit must be the exact currently stored state.
    QSqlQuery current(this->database_);
    if (!prepare(current, QStringLiteral("SELECT content FROM snapshots WHERE revision_uuid=?"), error))
    {
        rollback();
        return std::nullopt;
    }
    current.addBindValue(this->backend_->bindUuid(revision_info->uuid));
    if (!run(current, error) || !current.next() || current.value(0).toByteArray() != original.after)
    {
        setError(error, QStringLiteral("Undo conflict: snapshot differs from recorded operation"));
        rollback();
        return std::nullopt;
    }
    current.finish();
    const std::optional<QByteArray> replacement = original.event.had_previous_snapshot
        ? std::optional<QByteArray>(original.before) : std::nullopt;
    if (!applySnapshotOperation(*revision_info, actor.actor_uuid, replacement,
                                QStringLiteral("undo"), *target, error) || !commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return *target;
}

std::optional<QUuid> Database::redoLastChange(const OperationContext &actor,
                                               const QUuid &branch_uuid,
                                               qint64 expected_generation,
                                               QString *error)
{
    if (!beginWrite(error))
        return std::nullopt;
    if (!requirePermission(actor, QStringLiteral("project.model.write"), AccessScope::branch(branch_uuid), error))
    {
        rollback();
        return std::nullopt;
    }
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> revision_info = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!revision_info || revision_info->finalized || revision_info->generation != expected_generation)
    {
        setError(error, QStringLiteral("Working revision generation conflict"));
        rollback();
        return std::nullopt;
    }
    const std::optional<int> mode = objectMode(revision_info->uuid, error);
    if (!mode || *mode != 0)
    {
        setError(error, QStringLiteral("Object-mode revisions have a separate history; snapshot undo is unavailable"));
        rollback();
        return std::nullopt;
    }
    const std::optional<QVector<HistoryRow>> rows = loadRows(this->database_, *this->backend_, revision_info->uuid, error);
    const std::optional<Stacks> stacks = rows ? replay(*rows, error) : std::nullopt;
    const std::optional<QUuid> target = performReversal(stacks, false, error);
    if (!target)
    {
        rollback();
        return std::nullopt;
    }
    const HistoryRow original = stacks->replacements.value(*target);
    QSqlQuery current(this->database_);
    if (!prepare(current, QStringLiteral("SELECT content FROM snapshots WHERE revision_uuid=?"), error))
    {
        rollback();
        return std::nullopt;
    }
    current.addBindValue(this->backend_->bindUuid(revision_info->uuid));
    if (!run(current, error))
    {
        rollback();
        return std::nullopt;
    }
    const bool has_current = current.next();
    if (has_current != original.event.had_previous_snapshot ||
        (has_current && current.value(0).toByteArray() != original.before))
    {
        setError(error, QStringLiteral("Redo conflict: snapshot differs from recorded operation"));
        rollback();
        return std::nullopt;
    }
    current.finish();
    if (!applySnapshotOperation(*revision_info, actor.actor_uuid, original.after,
                                QStringLiteral("redo"), *target, error) || !commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return *target;
}

} // namespace aowis::db
