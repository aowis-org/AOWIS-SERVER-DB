#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVector>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

namespace
{
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

// Independent reference encoder for the one current pre-release chain format.
QByteArray referenceDigest(const QUuid &project, const QUuid &branch,
                           const QUuid &revision, const QUuid &creator,
                           const QUuid &finalizer, qint64 created_at,
                           qint64 number, qint64 finalized_at,
                           const QString &title, const QString &message,
                           const QByteArray &snapshot_digest,
                           const QByteArray &history_digest,
                           const QVector<QPair<QUuid, QByteArray>> &parents,
                           const QByteArray &object_root)
{
    QByteArray bytes("AOWIS-REVISION-DAG-V1\0", 22);
    bytes.append(project.toRfc4122());
    bytes.append(branch.toRfc4122());
    bytes.append(revision.toRfc4122());
    bytes.append(creator.toRfc4122());
    bytes.append(finalizer.toRfc4122());
    appendInt64(bytes, static_cast<quint64>(created_at));
    appendInt64(bytes, static_cast<quint64>(number));
    appendInt64(bytes, static_cast<quint64>(finalized_at));
    appendField(bytes, title.toUtf8());
    appendField(bytes, message.toUtf8());
    bytes.append(snapshot_digest);
    bytes.append(history_digest);
    bytes.append(object_root);
    appendInt64(bytes, static_cast<quint64>(parents.size()));
    for (const QPair<QUuid, QByteArray> &parent : parents)
    {
        bytes.append(parent.first.toRfc4122());
        bytes.append(parent.second);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

struct StoredRevision
{
    qint64 created_at = 0;
    qint64 finalized_at = 0;
    qint64 number = 0;
    QString title;
    QString message;
    QByteArray snapshot_hash;
    QByteArray history_hash;
    QByteArray chain_hash;
    QByteArray object_root;
    QUuid creator;
    QUuid finalizer;
    QVector<QPair<QUuid, QByteArray>> parents;
};

bool inspect(QSqlDatabase &connection, const QUuid &revision_uuid, StoredRevision *out)
{
    QSqlQuery query(connection);
    if (!query.prepare(QStringLiteral(
            "SELECT created_at_ms,finalized_at_ms,number,title,message,snapshot_sha256,"
            "operation_history_sha256,created_by_uuid,finalized_by_uuid,chain_sha256,object_root_sha256 "
            "FROM revisions WHERE uuid=?")))
        return false;
    query.addBindValue(revision_uuid.toRfc4122());
    if (!query.exec() || !query.next())
        return false;
    out->created_at = query.value(0).toLongLong();
    out->finalized_at = query.value(1).toLongLong();
    out->number = query.value(2).toLongLong();
    out->title = query.value(3).toString();
    out->message = query.value(4).toString();
    out->snapshot_hash = query.value(5).toByteArray();
    out->history_hash = query.value(6).toByteArray();
    out->creator = QUuid::fromRfc4122(query.value(7).toByteArray());
    out->finalizer = QUuid::fromRfc4122(query.value(8).toByteArray());
    out->chain_hash = query.value(9).toByteArray();
    out->object_root = query.value(10).isNull() ? QByteArray(32, '\0') : query.value(10).toByteArray();
    query.finish();

    QSqlQuery parents(connection);
    if (!parents.prepare(QStringLiteral(
            "SELECT p.parent_revision_uuid, r.chain_sha256 "
            "FROM revision_parents p JOIN revisions r ON r.uuid=p.parent_revision_uuid "
            "WHERE p.revision_uuid=? ORDER BY p.parent_position")))
        return false;
    parents.addBindValue(revision_uuid.toRfc4122());
    if (!parents.exec())
        return false;
    while (parents.next())
        out->parents.append(qMakePair(
            QUuid::fromRfc4122(parents.value(0).toByteArray()),
            parents.value(1).toByteArray()));
    return true;
}

// Simulate a privileged tamperer, restoring the original schema afterward.
// Ordinary applications cannot change finalized revision metadata.
bool tamperHistorySeal(QSqlDatabase &connection, const QUuid &revision_uuid)
{
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral(
            "SELECT sql FROM sqlite_master WHERE type='trigger' "
            "AND name='finalized_revision_readonly_update'")) || !query.next())
        return false;
    const QString original_trigger = query.value(0).toString();
    query.finish();
    if (original_trigger.isEmpty() ||
        !query.exec(QStringLiteral("DROP TRIGGER finalized_revision_readonly_update")))
        return false;
    query.finish();
    bool changed = query.prepare(QStringLiteral(
        "UPDATE revisions SET operation_history_sha256=? WHERE uuid=?"));
    if (changed)
    {
        query.addBindValue(QByteArray(32, '\x7f'));
        query.addBindValue(revision_uuid.toRfc4122());
        changed = query.exec() && query.numRowsAffected() == 1;
    }
    query.finish();
    const bool restored = query.exec(original_trigger);
    query.finish();
    return changed && restored;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QString error;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("revision-hashes.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};

    const std::optional<aowis::db::ProjectCreation> a = db.createProject(root, QStringLiteral("A"), &error);
    const std::optional<aowis::db::ProjectCreation> b = db.createProject(root, QStringLiteral("B"), &error);
    REQUIRE(a.has_value() && b.has_value());
    REQUIRE(db.replaceWorkingSnapshot(root, a->main_branch_uuid, 0, QByteArray("state 1"), &error));
    REQUIRE(db.replaceWorkingSnapshot(root, b->main_branch_uuid, 0, QByteArray("other project"), &error));
    const std::optional<aowis::db::Finalization> initial = db.finalize(
        root, a->main_branch_uuid, 1, QStringLiteral("Initial"), QStringLiteral("First history"), &error);
    const std::optional<aowis::db::Finalization> other = db.finalize(
        root, b->main_branch_uuid, 1, QStringLiteral("Other"), {}, &error);
    REQUIRE(initial.has_value() && other.has_value());
    REQUIRE(db.replaceWorkingSnapshot(root, a->main_branch_uuid, 0, QByteArray("state 2"), &error));
    const std::optional<aowis::db::Finalization> successor = db.finalize(
        root, a->main_branch_uuid, 1, QStringLiteral("Successor"), QStringLiteral("Has a parent"), &error);
    REQUIRE(successor.has_value());
    REQUIRE(db.verifyProject(root, a->project_uuid).valid);
    REQUIRE(db.verifyProject(root, b->project_uuid).valid);

    const QString connection_name = QStringLiteral("revision-hash-inspector");
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        for (const QUuid &revision_uuid : {initial->finalized_revision_uuid,
                                          successor->finalized_revision_uuid,
                                          other->finalized_revision_uuid})
        {
            StoredRevision stored;
            REQUIRE(inspect(inspector, revision_uuid, &stored));
            REQUIRE(stored.snapshot_hash.size() == 32);
            REQUIRE(stored.history_hash.size() == 32);
            const QUuid project_uuid = revision_uuid == other->finalized_revision_uuid
                ? b->project_uuid : a->project_uuid;
            const QUuid branch_uuid = revision_uuid == other->finalized_revision_uuid
                ? b->main_branch_uuid : a->main_branch_uuid;
            const QByteArray expected = referenceDigest(project_uuid, branch_uuid, revision_uuid,
                stored.creator, stored.finalizer, stored.created_at, stored.number,
                stored.finalized_at, stored.title, stored.message, stored.snapshot_hash,
                stored.history_hash, stored.parents, stored.object_root);
            REQUIRE(stored.chain_hash == expected);
            REQUIRE(expected != referenceDigest(project_uuid, branch_uuid, revision_uuid,
                stored.creator, stored.finalizer, stored.created_at, stored.number,
                stored.finalized_at, stored.title, stored.message, stored.snapshot_hash,
                QByteArray(32, '\0'), stored.parents, stored.object_root));
            if (revision_uuid == successor->finalized_revision_uuid)
            {
                REQUIRE(stored.parents.size() == 1);
                REQUIRE(stored.parents.at(0).first == initial->finalized_revision_uuid);
                REQUIRE(stored.parents.at(0).second == initial->chain_sha256);
                REQUIRE(expected != referenceDigest(project_uuid, branch_uuid, revision_uuid,
                    stored.creator, stored.finalizer, stored.created_at, stored.number,
                    stored.finalized_at, stored.title, stored.message, stored.snapshot_hash,
                    stored.history_hash, {}, stored.object_root));
            }
        }
        REQUIRE(tamperHistorySeal(inspector, initial->finalized_revision_uuid));
        inspector.close();
    }
    QSqlDatabase::removeDatabase(connection_name);

    REQUIRE(!db.verifyProject(root, a->project_uuid).valid);
    REQUIRE(db.verifyProject(root, b->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error)); // Verify the exact trigger definitions were restored.
    REQUIRE(!db.verifyProject(root, a->project_uuid).valid);
    REQUIRE(db.verifyProject(root, b->project_uuid).valid);
    return 0;
}
