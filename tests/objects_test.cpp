#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("objects.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> outsider = db.createUser(root, QStringLiteral("outsider"), &error);
    REQUIRE(outsider.has_value());
    const OperationContext unauthorized{outsider->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(root, QStringLiteral("Object store"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid rev1 = project->working_revision_uuid;
    const QUuid pipe = createUuidV7();
    const QUuid junction = createUuidV7();
    const QByteArray first("\0\1\xff", 3);
    const QByteArray next("updated", 7);

    REQUIRE(!db.putObject(unauthorized, branch, pipe, QStringLiteral("hydraulic.pipe"), first, -1, &error));
    REQUIRE(!db.putObject(root, branch, pipe, QStringLiteral("INVALID KIND"), first, -1, &error));
    REQUIRE(db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), first, -1, &error));
    REQUIRE(db.object(root, rev1, pipe, &error)->uuid == pipe);
    REQUIRE(db.object(root, rev1, pipe, &error)->version == 1);
    REQUIRE(db.object(root, rev1, pipe, &error)->content == first);
    REQUIRE(!db.object(unauthorized, rev1, pipe, &error).has_value());
    REQUIRE(!db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), next, -1, &error));
    REQUIRE(!db.putObject(root, branch, pipe, QStringLiteral("hydraulic.junction"), next, 1, &error));
    REQUIRE(db.putObject(root, branch, junction, QStringLiteral("hydraulic.junction"), QByteArray("junction"), -1, &error));
    // Another object's creation has changed the *revision* generation, not pipe's version.
    REQUIRE(db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), next, 1, &error));
    REQUIRE(!db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), first, 1, &error));
    REQUIRE(db.deleteObject(root, branch, pipe, 2, &error));
    const std::optional<aowis::db::ModelObject> tombstone = db.object(root, rev1, pipe, &error);
    REQUIRE(tombstone.has_value() && tombstone->deleted && tombstone->version == 3);
    REQUIRE(!db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), first, -1, &error));
    REQUIRE(db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), first, 3, &error));
    REQUIRE(db.object(root, rev1, pipe, &error)->version == 4);
    REQUIRE(db.objects(root, rev1, &error).size() == 2);
    const QVector<aowis::db::ObjectChange> changes = db.objectHistoryForRevision(root, rev1, &error);
    REQUIRE(changes.size() == 5);
    REQUIRE(changes[0].object_uuid == pipe);
    REQUIRE(changes[1].object_uuid == junction);
    for (const aowis::db::ObjectChange &change : changes)
    {
        REQUIRE(!change.uuid.isNull());
        REQUIRE(change.revision_uuid == rev1);
        REQUIRE(change.actor_uuid == root.actor_uuid);
        REQUIRE(change.object_uuid == pipe || change.object_uuid == junction);
    }
    REQUIRE(db.objectHistoryForRevision(unauthorized, rev1, &error).isEmpty());
    REQUIRE(db.historyForRevision(root, rev1, &error).isEmpty());
    REQUIRE(!db.undoLastChange(root, branch, 5, &error));
    REQUIRE(!db.replaceWorkingSnapshot(root, branch, 5, QByteArray("legacy"), &error));
    const std::optional<QByteArray> working_manifest = db.snapshot(root, rev1, &error);
    REQUIRE(working_manifest.has_value() && working_manifest->startsWith("AOWIS-REVISION-HISTORY-V1"));
    // Compact history seal: two 32-byte hashes following the domain magic.
    REQUIRE(working_manifest->size() == int(sizeof("AOWIS-REVISION-HISTORY-V1")) + 64);

    const std::optional<aowis::db::Finalization> finalized = db.finalize(
        root, branch, 5, QStringLiteral("Design"), QStringLiteral("object history"), &error);
    REQUIRE(finalized.has_value());
    REQUIRE(db.revision(root, rev1, &error)->object_root_sha256.size() == 32);
    REQUIRE(db.revision(root, finalized->new_working_revision_uuid, &error)->object_root_sha256.isEmpty());
    REQUIRE(finalized->snapshot_sha256 == QCryptographicHash::hash(
        *working_manifest, QCryptographicHash::Sha256));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    REQUIRE(db.snapshot(root, rev1, &error) == working_manifest);
    REQUIRE(db.objectHistoryForRevision(root, rev1, &error).size() == 5);
    REQUIRE(db.object(root, finalized->new_working_revision_uuid, pipe, &error)->version == 4);
    REQUIRE(db.objectHistoryForRevision(root, finalized->new_working_revision_uuid, &error).isEmpty());
    REQUIRE(!db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), first, 4, &error));
    REQUIRE(db.putObject(root, branch, pipe, QStringLiteral("hydraulic.pipe"), next, 4, &error));
    REQUIRE(db.object(root, rev1, pipe, &error)->content == first);

    const std::optional<aowis::db::BranchInfo> alt = db.createBranch(
        root, project->project_uuid, QStringLiteral("alternative"), rev1, &error);
    REQUIRE(alt.has_value());
    REQUIRE(db.object(root, alt->head_revision_uuid, pipe, &error)->content == first);
    REQUIRE(db.putObject(root, alt->uuid, pipe, QStringLiteral("hydraulic.pipe"), QByteArray("alt"), 4, &error));
    REQUIRE(db.object(root, rev1, pipe, &error)->content == first);
    REQUIRE(db.finalize(root, alt->uuid, 1, QStringLiteral("alt"), {}, &error));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);

    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("object-inspector"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        // Finalized revisions have no per-revision physical object rows.
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM revision_objects WHERE revision_uuid=?")));
        query.addBindValue(rev1.toRfc4122());
        REQUIRE(query.exec() && query.next() && query.value(0).toInt() == 0);
        query.finish();
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT object_uuid FROM revision_objects WHERE revision_uuid=? AND object_uuid=?")));
        query.addBindValue(finalized->new_working_revision_uuid.toRfc4122());
        query.addBindValue(pipe.toRfc4122());
        REQUIRE(query.exec() && query.next());
        REQUIRE(query.value(0).toByteArray() == pipe.toRfc4122());
        query.finish();
        // Immutable manifests belong to finalized revisions only. Working
        // object-mode branches should not store a duplicated full manifest.
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM snapshots WHERE revision_uuid=?")));
        query.addBindValue(finalized->new_working_revision_uuid.toRfc4122());
        REQUIRE(query.exec() && query.next() && query.value(0).toInt() == 0);
        query.finish();
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT uuid,actor_uuid,object_uuid FROM object_events "
            "WHERE revision_uuid=? ORDER BY sequence LIMIT 1")));
        query.addBindValue(rev1.toRfc4122());
        REQUIRE(query.exec() && query.next());
        REQUIRE(query.value(0).toByteArray() == changes[0].uuid.toRfc4122());
        REQUIRE(query.value(1).toByteArray() == root.actor_uuid.toRfc4122());
        REQUIRE(query.value(2).toByteArray() == pipe.toRfc4122());
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("object-inspector"));
    // Finalized reads use the sealed Merkle root with no duplicate SQL copies.
    REQUIRE(db.object(root, rev1, pipe, &error)->content == first);
    const QVector<aowis::db::ModelObject> sealed_objects = db.objects(root, rev1, &error);
    REQUIRE(sealed_objects.size() == 2);
    bool found_original_pipe = false;
    for (const aowis::db::ModelObject &item : sealed_objects)
    {
        if (item.uuid == pipe)
        {
            REQUIRE(item.content == first && item.version == 4 && !item.deleted);
            found_original_pipe = true;
        }
    }
    REQUIRE(found_original_pipe);
    REQUIRE(db.snapshot(root, rev1, &error) == working_manifest);
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    return 0;
}
