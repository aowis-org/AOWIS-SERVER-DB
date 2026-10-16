#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCryptographicHash>
#include <QDateTime>
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

QUuid readUuid(const QVariant &value)
{
    return QUuid::fromRfc4122(value.toByteArray());
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

void appendU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(QByteArray &out, const QByteArray &value)
{
    appendU64(out, static_cast<quint64>(value.size()));
    out.append(value);
}

bool validKind(const QString &kind)
{
    if (kind.isEmpty() || kind.size() > 128)
        return false;
    for (const QChar character : kind)
    {
        const ushort unit = character.unicode();
        if ((unit < 'a' || unit > 'z') && (unit < '0' || unit > '9') && unit != '.' && unit != '_')
            return false;
    }
    return true;
}

ModelObject readObjectRow(const QSqlQuery &query)
{
    return ModelObject{readUuid(query.value(0)), query.value(1).toString(),
                       query.value(2).toByteArray(), query.value(3).toLongLong(),
                       query.value(4).toBool()};
}

} // namespace

bool Database::migrateObjectsV4(QString *error)
{
    if (!beginWrite(error))
        return false;
    const char *const statements[] = {
        R"SQL(ALTER TABLE revisions ADD COLUMN object_mode INTEGER NOT NULL DEFAULT 0
             CHECK(object_mode IN (0,1)))SQL",
        R"SQL(CREATE TABLE revision_objects (
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            object_uuid BLOB NOT NULL CHECK(length(object_uuid)=16),
            kind TEXT NOT NULL CHECK(length(kind)>0 AND length(kind)<=128),
            content BLOB,
            version INTEGER NOT NULL CHECK(version>=1),
            deleted INTEGER NOT NULL CHECK(deleted IN (0,1)),
            PRIMARY KEY(revision_uuid,object_uuid),
            CHECK((deleted=1 AND content IS NULL) OR (deleted=0 AND content IS NOT NULL))
        ))SQL",
        R"SQL(CREATE INDEX revision_objects_by_kind
              ON revision_objects(revision_uuid,kind,object_uuid))SQL",
        R"SQL(CREATE TABLE object_events (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            object_uuid BLOB NOT NULL CHECK(length(object_uuid)=16),
            actor_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            sequence INTEGER NOT NULL CHECK(sequence>=1),
            occurred_at_ms INTEGER NOT NULL,
            kind TEXT NOT NULL,
            action TEXT NOT NULL CHECK(action IN ('put','delete')),
            before_version INTEGER NOT NULL CHECK(before_version>=-1),
            after_version INTEGER NOT NULL CHECK(after_version>=1),
            before_content BLOB,
            after_content BLOB,
            before_deleted INTEGER NOT NULL CHECK(before_deleted IN (0,1)),
            UNIQUE(revision_uuid,sequence),
            CHECK((before_version=-1 AND after_version=1) OR
                  (before_version>=1 AND after_version=before_version+1)),
            CHECK((action='put' AND after_content IS NOT NULL) OR
                  (action='delete' AND after_content IS NULL)),
            CHECK((before_version=-1 AND before_content IS NULL AND before_deleted=1) OR
                  before_version>=1)
        ))SQL",
        R"SQL(CREATE TRIGGER immutable_object_event_update BEFORE UPDATE ON object_events
             BEGIN SELECT RAISE(ABORT,'object events are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER immutable_object_event_delete BEFORE DELETE ON object_events
             BEGIN SELECT RAISE(ABORT,'object events cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER object_event_insert_guard BEFORE INSERT ON object_events
             WHEN NOT EXISTS (SELECT 1 FROM revisions r WHERE r.uuid=NEW.revision_uuid
               AND r.state='working' AND r.object_mode=1 AND r.generation=NEW.sequence)
             BEGIN SELECT RAISE(ABORT,'object event must match current revision'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_object_insert BEFORE INSERT ON revision_objects
             WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
             BEGIN SELECT RAISE(ABORT,'finalized objects are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_object_update BEFORE UPDATE ON revision_objects
             WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
             BEGIN SELECT RAISE(ABORT,'finalized objects are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER object_identity_readonly BEFORE UPDATE ON revision_objects
             WHEN OLD.revision_uuid!=NEW.revision_uuid OR OLD.object_uuid!=NEW.object_uuid
                 OR OLD.kind!=NEW.kind
             BEGIN SELECT RAISE(ABORT,'object identity and type are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_object_delete BEFORE DELETE ON revision_objects
             WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
             BEGIN SELECT RAISE(ABORT,'finalized objects are immutable'); END)SQL"
    };
    for (const char *statement : statements)
    {
        if (!exec(QString::fromUtf8(statement), error))
        {
            rollback();
            return false;
        }
    }
    if (!exec(QStringLiteral("PRAGMA user_version = 4"), error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<int> Database::objectMode(const QUuid &revision_uuid, QString *error) const
{
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("SELECT object_mode FROM revisions WHERE uuid=?"), error))
        return std::nullopt;
    query.addBindValue(uuidBytes(revision_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Revision not found"));
        return std::nullopt;
    }
    return query.value(0).toInt();
}

bool Database::copyObjects(const QUuid &from_revision, const QUuid &to_revision, QString *error)
{
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
            "SELECT ?,object_uuid,kind,content,version,deleted FROM revision_objects WHERE revision_uuid=?"), error))
        return false;
    query.addBindValue(uuidBytes(to_revision));
    query.addBindValue(uuidBytes(from_revision));
    if (!run(query, error))
        return false;
    QSqlQuery members(this->database_);
    if (!prepare(members, QStringLiteral(
            "INSERT INTO hydraulic_network_members(revision_uuid,network_uuid,entity_uuid) "
            "SELECT ?,network_uuid,entity_uuid FROM hydraulic_network_members WHERE revision_uuid=?"), error))
        return false;
    members.addBindValue(uuidBytes(to_revision));
    members.addBindValue(uuidBytes(from_revision));
    if (!run(members, error))
        return false;
    QSqlQuery references(this->database_);
    if (!prepare(references, QStringLiteral(
            "INSERT INTO hydraulic_network_references"
            "(revision_uuid,network_uuid,source_uuid,target_uuid,field) "
            "SELECT ?,network_uuid,source_uuid,target_uuid,field "
            "FROM hydraulic_network_references WHERE revision_uuid=?"), error))
        return false;
    references.addBindValue(uuidBytes(to_revision));
    references.addBindValue(uuidBytes(from_revision));
    return run(references, error);
}

// Contains the full ordered journal (including before/after bytes) and live
// object state. No dependence on SQLite page layout or local serialization.
std::optional<QByteArray> Database::objectManifest(const QUuid &revision_uuid, QString *error) const
{
    QSqlQuery events(this->database_);
    if (!prepare(events, QStringLiteral(
            "SELECT uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,kind,action,"
            "before_version,after_version,before_content,after_content,before_deleted "
            "FROM object_events WHERE revision_uuid=? ORDER BY sequence"), error))
        return std::nullopt;
    events.addBindValue(uuidBytes(revision_uuid));
    if (!run(events, error))
        return std::nullopt;
    QByteArray log("AOWIS-OBJECT-JOURNAL-V1\0", 24);
    quint64 log_count = 0;
    QByteArray entries;
    while (events.next())
    {
        entries.append(events.value(0).toByteArray());
        entries.append(events.value(1).toByteArray());
        entries.append(events.value(2).toByteArray());
        appendU64(entries, events.value(3).toULongLong());
        appendU64(entries, events.value(4).toULongLong());
        appendField(entries, events.value(5).toString().toUtf8());
        appendField(entries, events.value(6).toString().toUtf8());
        appendU64(entries, static_cast<quint64>(events.value(7).toLongLong()+1));
        appendU64(entries, events.value(8).toULongLong());
        entries.append(events.value(11).toBool() ? '\x01' : '\x00');
        entries.append(events.value(9).isNull() ? '\x00' : '\x01');
        if (!events.value(9).isNull())
            appendField(entries, events.value(9).toByteArray());
        entries.append(events.value(10).isNull() ? '\x00' : '\x01');
        if (!events.value(10).isNull())
            appendField(entries, events.value(10).toByteArray());
        ++log_count;
    }
    appendU64(log, log_count);
    log.append(entries);
    const QByteArray journal_sha256 = QCryptographicHash::hash(log, QCryptographicHash::Sha256);

    QSqlQuery objects(this->database_);
    if (!prepare(objects, QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? ORDER BY object_uuid"), error))
        return std::nullopt;
    objects.addBindValue(uuidBytes(revision_uuid));
    if (!run(objects, error))
        return std::nullopt;
    QByteArray manifest("AOWIS-OBJECT-MANIFEST-V1\0", 25);
    manifest.append(journal_sha256);
    QByteArray objects_bytes;
    quint64 object_count = 0;
    while (objects.next())
    {
        objects_bytes.append(objects.value(0).toByteArray());
        appendField(objects_bytes, objects.value(1).toString().toUtf8());
        appendU64(objects_bytes, objects.value(3).toULongLong());
        const bool deleted = objects.value(4).toBool();
        objects_bytes.append(deleted ? '\x01' : '\x00');
        if (!deleted)
            appendField(objects_bytes, objects.value(2).toByteArray());
        ++object_count;
    }
    appendU64(manifest, object_count);
    manifest.append(objects_bytes);
    QSqlQuery format(this->database_);
    if (!prepare(format, QStringLiteral(
            "SELECT object_journal_version FROM revisions WHERE uuid=?"), error))
        return std::nullopt;
    format.addBindValue(uuidBytes(revision_uuid));
    if (!run(format, error) || !format.next())
        return std::nullopt;
    if (format.value(0).toInt() == 1)
        return manifest; // Byte-identical V4 manifest for old finalized hashes.
    // V5 appends an ordered, domain-separated representation of immutable
    // groups and their exact member-event identities.
    QByteArray grouped("AOWIS-OBJECT-GROUPS-V1\0", 23);
    QSqlQuery groups(this->database_);
    if (!prepare(groups, QStringLiteral(
            "SELECT uuid,actor_uuid,action,target_group_uuid,first_sequence,last_sequence,occurred_at_ms "
            "FROM object_groups WHERE revision_uuid=? ORDER BY first_sequence"), error))
        return std::nullopt;
    groups.addBindValue(uuidBytes(revision_uuid));
    if (!run(groups, error))
        return std::nullopt;
    quint64 count = 0;
    QByteArray group_entries;
    while (groups.next())
    {
        const QByteArray group_id = groups.value(0).toByteArray();
        group_entries.append(group_id);
        group_entries.append(groups.value(1).toByteArray());
        appendField(group_entries, groups.value(2).toString().toUtf8());
        group_entries.append(groups.value(3).isNull() ? QByteArray(16, '\0') : groups.value(3).toByteArray());
        appendU64(group_entries, groups.value(4).toULongLong());
        appendU64(group_entries, groups.value(5).toULongLong());
        appendU64(group_entries, groups.value(6).toULongLong());
        QSqlQuery links(this->database_);
        if (!prepare(links, QStringLiteral(
                "SELECT position,object_event_uuid FROM object_group_events WHERE group_uuid=? ORDER BY position"), error))
            return std::nullopt;
        links.addBindValue(group_id);
        if (!run(links, error))
            return std::nullopt;
        QByteArray ids;
        quint64 link_count = 0;
        while (links.next())
        {
            appendU64(ids, links.value(0).toULongLong());
            ids.append(links.value(1).toByteArray());
            ++link_count;
        }
        appendU64(group_entries, link_count);
        group_entries.append(ids);
        ++count;
    }
    appendU64(grouped, count);
    grouped.append(group_entries);
    appendField(manifest, grouped);
    return manifest;
}

std::optional<ModelObject> Database::object(const OperationContext &actor, const QUuid &revision_uuid,
                                              const QUuid &object_uuid, QString *error) const
{
    const std::optional<RevisionInfo> revision = revisionUnchecked(revision_uuid, error);
    if (!revision || !requirePermission(actor, QStringLiteral("project.read"),
                                        AccessScope::branch(revision->branch_uuid), error))
        return std::nullopt;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? AND object_uuid=?"), error))
        return std::nullopt;
    query.addBindValue(uuidBytes(revision_uuid));
    query.addBindValue(uuidBytes(object_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Object not found"));
        return std::nullopt;
    }
    return readObjectRow(query);
}

QVector<ModelObject> Database::objects(const OperationContext &actor, const QUuid &revision_uuid,
                                         QString *error) const
{
    QVector<ModelObject> result;
    const std::optional<RevisionInfo> revision = revisionUnchecked(revision_uuid, error);
    if (!revision || !requirePermission(actor, QStringLiteral("project.read"),
                                        AccessScope::branch(revision->branch_uuid), error))
        return result;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? AND deleted=0 ORDER BY object_uuid"), error))
        return result;
    query.addBindValue(uuidBytes(revision_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
        result.append(readObjectRow(query));
    return result;
}

QVector<ObjectChange> Database::objectHistoryForRevision(const OperationContext &actor,
                                      const QUuid &revision_uuid, QString *error) const
{
    QVector<ObjectChange> result;
    const std::optional<RevisionInfo> revision = revisionUnchecked(revision_uuid, error);
    if (!revision || !requirePermission(actor, QStringLiteral("project.history.read"),
                                        AccessScope::branch(revision->branch_uuid), error))
        return result;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,kind,action,before_version,after_version "
            "FROM object_events WHERE revision_uuid=? ORDER BY sequence"), error))
        return result;
    query.addBindValue(uuidBytes(revision_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
    {
        result.append(ObjectChange{readUuid(query.value(0)), revision_uuid,
            readUuid(query.value(1)), readUuid(query.value(2)),
            query.value(5).toString(), query.value(6).toString(),
            query.value(3).toLongLong(), query.value(4).toLongLong(),
            query.value(7).toLongLong(), query.value(8).toLongLong()});
    }
    return result;
}

bool Database::putObject(const OperationContext &actor, const QUuid &branch_uuid,
                          const QUuid &object_uuid, const QString &kind,
                          const QByteArray &canonical_bytes, qint64 expected_version,
                          QString *error)
{
    if (!validKind(kind))
    {
        setError(error, QStringLiteral("Invalid object kind"));
        return false;
    }
    return modifyObject(actor, branch_uuid, object_uuid, kind, canonical_bytes, expected_version, error);
}

bool Database::deleteObject(const OperationContext &actor, const QUuid &branch_uuid,
                             const QUuid &object_uuid, qint64 expected_version,
                             QString *error)
{
    return modifyObject(actor, branch_uuid, object_uuid, {}, std::nullopt, expected_version, error);
}

bool Database::modifyObject(const OperationContext &actor, const QUuid &branch_uuid,
                             const QUuid &object_uuid, const QString &kind,
                             const std::optional<QByteArray> &bytes, qint64 expected_version,
                             QString *error)
{
    if (!beginWrite(error))
        return false;
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> r = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    bool ok = r && !r->finalized;
    if (!ok)
        setError(error, QStringLiteral("Working revision not found"));
    const qint64 before = r ? r->generation : -1;
    if (ok)
        ok = modifyObjectInTransaction(actor, branch_uuid, object_uuid, kind, bytes, expected_version, error);
    if (ok)
        ok = validateTouchedHydraulicNetworks(r->uuid, QSet<QUuid>{object_uuid}, error);
    if (ok)
        ok = writeObjectGroupInTransaction(*r, actor, QStringLiteral("edit"), QUuid(), before, error).has_value();
    if (ok)
        ok = commit(error);
    if (!ok)
        rollback();
    return ok;
}

bool Database::modifyObjectInTransaction(const OperationContext &actor, const QUuid &branch_uuid,
                             const QUuid &object_uuid, const QString &kind,
                             const std::optional<QByteArray> &bytes, qint64 expected_version,
                             QString *error)
{
    if (object_uuid.isNull() || expected_version < -1)
    {
        setError(error, QStringLiteral("Invalid object UUID or version"));
        return false;
    }
    if (!requirePermission(actor, QStringLiteral("project.model.write"),
                           AccessScope::branch(branch_uuid), error))
    {
        return false;
    }
    const std::optional<BranchInfo> branch_info = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> revision = branch_info
        ? revisionUnchecked(branch_info->head_revision_uuid, error) : std::nullopt;
    if (!revision || revision->finalized)
    {
        setError(error, QStringLiteral("Working revision not found"));
        return false;
    }
    const std::optional<int> mode = objectMode(revision->uuid, error);
    if (!mode)
    {
        return false;
    }
    if (*mode == 0)
    {
        QSqlQuery legacy(this->database_);
        if (!prepare(legacy, QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM snapshots WHERE revision_uuid=?) OR "
                "EXISTS(SELECT 1 FROM operation_events WHERE revision_uuid=?)"), error))
        {
                return false;
        }
        legacy.addBindValue(uuidBytes(revision->uuid));
        legacy.addBindValue(uuidBytes(revision->uuid));
        if (!run(legacy, error) || !legacy.next() || legacy.value(0).toBool())
        {
            setError(error, QStringLiteral("Existing legacy snapshots cannot be mixed with object-mode writes"));
                return false;
        }
        QSqlQuery enable(this->database_);
        if (!prepare(enable, QStringLiteral(
                "UPDATE revisions SET object_mode=1 WHERE uuid=? AND object_mode=0"), error))
        {
                return false;
        }
        enable.addBindValue(uuidBytes(revision->uuid));
        if (!run(enable, error) || enable.numRowsAffected() != 1)
        {
                return false;
        }
    }
    QSqlQuery current(this->database_);
    if (!prepare(current, QStringLiteral(
            "SELECT kind,content,version,deleted FROM revision_objects WHERE revision_uuid=? AND object_uuid=?"), error))
    {
        return false;
    }
    current.addBindValue(uuidBytes(revision->uuid));
    current.addBindValue(uuidBytes(object_uuid));
    if (!run(current, error))
    {
        return false;
    }
    const bool existed = current.next();
    const QString existing_kind = existed ? current.value(0).toString() : kind;
    const QVariant before_content = existed ? current.value(1) : QVariant();
    const qint64 before_version = existed ? current.value(2).toLongLong() : -1;
    const bool before_deleted = !existed || current.value(3).toBool();
    current.finish();
    if ((!bytes && (!existed || before_deleted)) || before_version != expected_version ||
        (existed && bytes && kind != existing_kind) ||
        (bytes && existed && !before_deleted && before_content.toByteArray() == *bytes))
    {
        setError(error, QStringLiteral("Object version conflict, invalid kind, or no state change"));
        return false;
    }
    QSqlQuery query(this->database_);
    const qint64 stored_version = existed ? before_version + 1 : 1;
    const QString chosen_kind = existed ? existing_kind : kind;
    bool ok;
    if (!existed)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
            "VALUES (?,?,?,?,1,0)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(revision->uuid));
            query.addBindValue(uuidBytes(object_uuid));
            query.addBindValue(chosen_kind);
            query.addBindValue(*bytes);
            ok = run(query, error);
        }
    }
    else
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE revision_objects SET content=?,version=?,deleted=? WHERE revision_uuid=? AND object_uuid=?"), error);
        if (ok)
        {
            query.addBindValue(bytes ? QVariant(*bytes) : QVariant());
            query.addBindValue(stored_version);
            query.addBindValue(bytes ? 0 : 1);
            query.addBindValue(uuidBytes(revision->uuid));
            query.addBindValue(uuidBytes(object_uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE revisions SET generation=generation+1 WHERE uuid=? AND state='working'"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(revision->uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO object_events(uuid,revision_uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,kind,action,"
            "before_version,after_version,before_content,after_content,before_deleted) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(createUuidV7()));
            query.addBindValue(uuidBytes(revision->uuid));
            query.addBindValue(uuidBytes(object_uuid));
            query.addBindValue(uuidBytes(actor.actor_uuid));
            query.addBindValue(revision->generation + 1);
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            query.addBindValue(chosen_kind);
            query.addBindValue(bytes ? QStringLiteral("put") : QStringLiteral("delete"));
            query.addBindValue(before_version);
            query.addBindValue(stored_version);
            query.addBindValue(before_content);
            query.addBindValue(bytes ? QVariant(*bytes) : QVariant());
            query.addBindValue(before_deleted ? 1 : 0);
            ok = run(query, error);
        }
    }
    return ok;
}

} // namespace aowis::db
