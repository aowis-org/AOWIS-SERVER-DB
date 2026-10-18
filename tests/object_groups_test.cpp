#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ObjectEdit;
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
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("group.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> user = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(user.has_value());
    const OperationContext actor{user->uuid};
    const std::optional<aowis::db::UserInfo> visitor = db.createUser(actor, QStringLiteral("visitor"), &error);
    REQUIRE(visitor.has_value());
    const OperationContext unauthorized{visitor->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(actor, QStringLiteral("Atomic history"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid rev1 = project->working_revision_uuid;
    const QUuid pipe = createUuidV7();
    const QUuid junction = createUuidV7();
    const QString pipe_kind = QStringLiteral("hydraulic.pipe");
    const QString junction_kind = QStringLiteral("hydraulic.junction");

    const QVector<ObjectEdit> initial{
        {pipe, pipe_kind, QByteArray("pipe A"), -1},
        {junction, junction_kind, QByteArray("junction A"), -1}
    };
    REQUIRE(!db.commitObjectGroup(unauthorized, branch, 0, initial, &error));
    REQUIRE(!db.commitObjectGroup(actor, branch, 0, {initial.at(0), initial.at(0)}, &error));
    const std::optional<QUuid> first = db.commitObjectGroup(actor, branch, 0, initial, &error);
    REQUIRE(first.has_value());
    REQUIRE(db.revision(actor, rev1, &error)->generation == 2);
    const QVector<aowis::db::ObjectGroup> first_groups = db.objectGroupsForRevision(actor, rev1, &error);
    REQUIRE(first_groups.size() == 1);
    REQUIRE(first_groups.first().uuid == *first);
    REQUIRE(first_groups.first().actor_uuid == actor.actor_uuid);
    REQUIRE(first_groups.first().target_group_uuid.isNull());
    REQUIRE(first_groups.first().object_event_uuids.size() == 2);
    REQUIRE(db.objectHistoryForRevision(actor, rev1, &error).size() == 2);
    REQUIRE(db.object(actor, rev1, pipe, &error)->content == QByteArray("pipe A"));
    REQUIRE(!db.commitObjectGroup(actor, branch, 2, {
        {pipe, pipe_kind, QByteArray("pipe B"), 1},
        {junction, junction_kind, QByteArray("junction B"), 0} // invalid: entire batch rolls back
    }, &error));
    REQUIRE(db.revision(actor, rev1, &error)->generation == 2);
    REQUIRE(db.objectHistoryForRevision(actor, rev1, &error).size() == 2);
    REQUIRE(db.object(actor, rev1, pipe, &error)->content == QByteArray("pipe A"));
    REQUIRE(!db.commitObjectGroup(actor, branch, 1, {{pipe, pipe_kind, QByteArray("bad"), 1}}, &error));

    const std::optional<QUuid> second = db.commitObjectGroup(actor, branch, 2, {
        {pipe, pipe_kind, QByteArray("pipe B"), 1},
        {junction, junction_kind, std::nullopt, 1}
    }, &error);
    REQUIRE(second.has_value());
    REQUIRE(db.object(actor, rev1, junction, &error)->deleted);
    REQUIRE(!db.undoObjectGroup(actor, branch, 3, &error));
    REQUIRE(db.undoObjectGroup(actor, branch, 4, &error) == second);
    const QVector<aowis::db::ObjectGroup> after_group_undo = db.objectGroupsForRevision(actor, rev1, &error);
    REQUIRE(after_group_undo.size() == 3);
    REQUIRE(after_group_undo.last().action == QStringLiteral("undo"));
    REQUIRE(after_group_undo.last().actor_uuid == actor.actor_uuid);
    REQUIRE(after_group_undo.last().target_group_uuid == *second);
    REQUIRE(db.object(actor, rev1, pipe, &error)->content == QByteArray("pipe A"));
    REQUIRE(!db.object(actor, rev1, junction, &error)->deleted);
    REQUIRE(db.redoObjectGroup(actor, branch, 6, &error) == second);
    REQUIRE(db.object(actor, rev1, pipe, &error)->content == QByteArray("pipe B"));
    REQUIRE(db.object(actor, rev1, junction, &error)->deleted);
    REQUIRE(db.undoObjectGroup(actor, branch, 8, &error) == second);
    REQUIRE(db.undoObjectGroup(actor, branch, 10, &error) == first);
    REQUIRE(db.object(actor, rev1, pipe, &error)->deleted);
    REQUIRE(db.object(actor, rev1, junction, &error)->deleted);
    REQUIRE(db.redoObjectGroup(actor, branch, 12, &error) == first);
    REQUIRE(db.redoObjectGroup(actor, branch, 14, &error) == second);
    REQUIRE(db.object(actor, rev1, junction, &error)->deleted);
    REQUIRE(db.objectGroupsForRevision(actor, rev1, &error).size() == 8);
    REQUIRE(db.objectHistoryForRevision(actor, rev1, &error).size() == 16);
    REQUIRE(db.objectGroupsForRevision(unauthorized, rev1, &error).isEmpty());

    REQUIRE(db.undoObjectGroup(actor, branch, 16, &error) == second);
    // A new edit clears redo, including when a different object is edited.
    const QUuid label = createUuidV7();
    REQUIRE(db.commitObjectGroup(actor, branch, 18,
        {{label, QStringLiteral("hydraulic.label"), QByteArray("label"), -1}}, &error));
    REQUIRE(!db.redoObjectGroup(actor, branch, 19, &error));
    REQUIRE(db.objectHistoryForRevision(actor, rev1, &error).size() == 19);

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.objectGroupsForRevision(actor, rev1, &error).size() == 10);
    REQUIRE(db.undoObjectGroup(actor, branch, 19, &error).has_value());
    REQUIRE(db.revision(actor, rev1, &error)->generation == 20);
    REQUIRE(!db.redoObjectGroup(unauthorized, branch, 20, &error));
    const std::optional<QByteArray> manifest = db.snapshot(actor, rev1, &error);
    REQUIRE(manifest && manifest->startsWith("AOWIS-OBJECT-MANIFEST-V1"));
    const std::optional<aowis::db::Finalization> final = db.finalize(actor, branch, 20,
                    QStringLiteral("Grouped history"), QStringLiteral("Undo/redo journal"), &error);
    REQUIRE(final.has_value());
    REQUIRE(final->snapshot_sha256 == QCryptographicHash::hash(
        *manifest, QCryptographicHash::Sha256));
    REQUIRE(db.snapshot(actor, rev1, &error) == manifest);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(!db.undoObjectGroup(actor, branch, 0, &error)); // successor has no events
    REQUIRE(db.objectGroupsForRevision(actor, final->new_working_revision_uuid, &error).isEmpty());

    const std::optional<aowis::db::BranchInfo> alternate = db.createBranch(actor,
        project->project_uuid, QStringLiteral("experiment"), rev1, &error);
    REQUIRE(alternate.has_value());
    REQUIRE(db.object(actor, alternate->head_revision_uuid, pipe, &error)->content == QByteArray("pipe A"));
    REQUIRE(db.putObject(actor, alternate->uuid, pipe, pipe_kind, QByteArray("alternate"),
                         db.object(actor, alternate->head_revision_uuid, pipe, &error)->version, &error));
    REQUIRE(db.finalize(actor, alternate->uuid, 1, QStringLiteral("alt"), {}, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // Immutability at SQL level and tampering detected by the finalized object manifest.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("groups-inspect"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.prepare(QStringLiteral("UPDATE object_groups SET action='edit' WHERE revision_uuid=?")));
        query.addBindValue(rev1.toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER object_group_update_guard")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE object_groups SET occurred_at_ms=occurred_at_ms+1 WHERE uuid=?")));
        query.addBindValue(first->toRfc4122());
        REQUIRE(query.exec());
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("groups-inspect"));
    REQUIRE(!db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
