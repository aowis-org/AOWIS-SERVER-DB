#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include "legacy_sqlite_v1.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

namespace
{
void appendInt64(QByteArray &buffer, quint64 number)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        buffer.append(static_cast<char>((number >> shift) & 0xffU));
}

void appendField(QByteArray &buffer, const QByteArray &field)
{
    appendInt64(buffer, static_cast<quint64>(field.size()));
    buffer.append(field);
}

QByteArray legacyChain(const QUuid &project, const QUuid &branch, const QUuid &revision,
                       qint64 number, qint64 time, const QString &title, const QString &message,
                       const QByteArray &snapshot_sha256)
{
    QByteArray bytes("AOWIS-REVISION-DAG-V1\0", 22);
    bytes.append(project.toRfc4122());
    bytes.append(branch.toRfc4122());
    bytes.append(revision.toRfc4122());
    appendInt64(bytes, static_cast<quint64>(number));
    appendInt64(bytes, static_cast<quint64>(time));
    appendField(bytes, title.toUtf8());
    appendField(bytes, message.toUtf8());
    bytes.append(snapshot_sha256);
    appendInt64(bytes, 0); // root revision: no parent hashes
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("legacy.sqlite"));
    const QUuid project_uuid = createUuidV7();
    const QUuid branch_uuid = createUuidV7();
    const QUuid finalized_uuid = createUuidV7();
    const QUuid working_uuid = createUuidV7();
    const QByteArray body("original-v1-snapshot");
    const QByteArray sha = QCryptographicHash::hash(body, QCryptographicHash::Sha256);
    const QByteArray hash = legacyChain(project_uuid, branch_uuid, finalized_uuid, 1, 1234,
                                        QStringLiteral("Historical"), {}, sha);

    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("aowis-old-db"));
        legacy.setDatabaseName(path);
        REQUIRE(legacy.open());
        QSqlQuery query(legacy);
        REQUIRE(query.exec(QStringLiteral("PRAGMA foreign_keys=ON")));
        for (const char *statement : legacy_sqlite_v1::statements)
            REQUIRE(query.exec(QString::fromUtf8(statement)));
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version=1")));
        REQUIRE(query.prepare(QStringLiteral("INSERT INTO projects(uuid,name,created_at_ms) VALUES(?,?,?)")));
        query.addBindValue(project_uuid.toRfc4122());
        query.addBindValue(QStringLiteral("Legacy"));
        query.addBindValue(1000);
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral("INSERT INTO branches(uuid,project_uuid,name) VALUES(?,?,'main')")));
        query.addBindValue(branch_uuid.toRfc4122());
        query.addBindValue(project_uuid.toRfc4122());
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral(
            "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms) "
            "VALUES(?,?,?,1,'working',?)")));
        query.addBindValue(finalized_uuid.toRfc4122());
        query.addBindValue(project_uuid.toRfc4122());
        query.addBindValue(branch_uuid.toRfc4122());
        query.addBindValue(1000);
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral("INSERT INTO snapshots(revision_uuid,content) VALUES(?,?)")));
        query.addBindValue(finalized_uuid.toRfc4122());
        query.addBindValue(body);
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral(
            "UPDATE revisions SET state='finalized',finalized_at_ms=?,title=?,message=?,"
            "snapshot_sha256=?,chain_sha256=? WHERE uuid=?")));
        query.addBindValue(1234);
        query.addBindValue(QStringLiteral("Historical"));
        query.addBindValue(QStringLiteral(""));
        query.addBindValue(sha);
        query.addBindValue(hash);
        query.addBindValue(finalized_uuid.toRfc4122());
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral(
            "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms) "
            "VALUES(?,?,?,2,'working',?)")));
        query.addBindValue(working_uuid.toRfc4122());
        query.addBindValue(project_uuid.toRfc4122());
        query.addBindValue(branch_uuid.toRfc4122());
        query.addBindValue(2000);
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) VALUES(?,0,?)")));
        query.addBindValue(working_uuid.toRfc4122());
        query.addBindValue(finalized_uuid.toRfc4122());
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral("INSERT INTO snapshots(revision_uuid,content) VALUES(?,?)")));
        query.addBindValue(working_uuid.toRfc4122());
        query.addBindValue(body);
        REQUIRE(query.exec());
        REQUIRE(query.prepare(QStringLiteral("UPDATE branches SET head_revision_uuid=? WHERE uuid=?")));
        query.addBindValue(working_uuid.toRfc4122());
        query.addBindValue(branch_uuid.toRfc4122());
        REQUIRE(query.exec());
        query.finish();
        legacy.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("aowis-old-db"));

    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> root_user = db.bootstrapAdministrator(QStringLiteral("operator"), &error);
    REQUIRE(root_user.has_value());
    const OperationContext root{root_user->uuid};
    // This fixture starts at V1. Its final schema version must match V10,
    // including the audit journal (rather than stopping at the old V8).
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                             QStringLiteral("schema-inspection"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        QSqlQuery pragma(inspection);
        REQUIRE(pragma.exec(QStringLiteral("PRAGMA user_version")));
        REQUIRE(pragma.next() && pragma.value(0).toInt() == 10);
        pragma.finish();
        QSqlQuery audit_table(inspection);
        REQUIRE(audit_table.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
            "AND name='security_audit_events'")));
        REQUIRE(audit_table.next() && audit_table.value(0).toInt() == 1);
        audit_table.finish();
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("schema-inspection"));

    const aowis::db::IntegrityReport migrated = db.verifyProject(root, project_uuid);
    REQUIRE(migrated.valid && migrated.checked_revisions == 1);
    REQUIRE(db.revision(root, finalized_uuid, &error)->hash_version == 1);
    REQUIRE(db.revision(root, finalized_uuid, &error)->created_by_uuid.isNull());
    REQUIRE(db.revision(root, finalized_uuid, &error)->finalized_by_uuid.isNull());
    REQUIRE(db.snapshot(root, finalized_uuid, &error) == body);

    // A pre-migration working revision can now finalize using the new actor
    // format while preserving the parent revision's exact V1 chain hash.
    const std::optional<aowis::db::Finalization> next = db.finalize(
        root, branch_uuid, 0, QStringLiteral("New milestone"), {}, &error);
    REQUIRE(next.has_value());
    REQUIRE(db.revision(root, working_uuid, &error)->hash_version == 2);
    REQUIRE(db.revision(root, working_uuid, &error)->finalized_by_uuid == root.actor_uuid);
    REQUIRE(db.verifyProject(root, project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(root, project_uuid).valid);
    return 0;
}
