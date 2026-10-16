#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ObjectMergeKind;
using aowis::db::ObjectMergeResolution;
using aowis::db::ObjectMergeSide;
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
    const QString path = directory.filePath(QStringLiteral("merge-local.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(actor, QStringLiteral("local work"), &error);
    REQUIRE(project);
    const QString kind = QStringLiteral("hydraulic.label");
    const QUuid shared = createUuidV7();
    const QUuid remove_local = createUuidV7();
    const QUuid update_right = createUuidV7();
    const QUuid right_new = createUuidV7();
    const QUuid local_new = createUuidV7();
    const QUuid left_only = createUuidV7();
    REQUIRE(db.putObject(actor, project->main_branch_uuid, shared, kind, QByteArray("base"), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, remove_local, kind, QByteArray("original"), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, update_right, kind, QByteArray("old"), -1, &error));
    const std::optional<aowis::db::Finalization> base = db.finalize(
        actor, project->main_branch_uuid, 3, QStringLiteral("base"), {}, &error);
    REQUIRE(base);
    const std::optional<aowis::db::BranchInfo> left = db.createBranch(
        actor, project->project_uuid, QStringLiteral("left"), base->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(
        actor, project->project_uuid, QStringLiteral("right"), base->finalized_revision_uuid, &error);
    REQUIRE(left && right);
    REQUIRE(db.putObject(actor, left->uuid, shared, kind, QByteArray("L"), 1, &error));
    REQUIRE(db.putObject(actor, left->uuid, left_only, kind, QByteArray("left"), -1, &error));
    REQUIRE(db.putObject(actor, right->uuid, shared, kind, QByteArray("R"), 1, &error));
    REQUIRE(db.putObject(actor, right->uuid, remove_local, kind, QByteArray("R-replacement"), 1, &error));
    REQUIRE(db.putObject(actor, right->uuid, update_right, kind, QByteArray("R-updated"), 1, &error));
    REQUIRE(db.putObject(actor, right->uuid, right_new, kind, QByteArray("R-created"), -1, &error));
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        actor, left->uuid, 2, QStringLiteral("left finalized"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        actor, right->uuid, 4, QStringLiteral("right finalized"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid head = left_final->new_working_revision_uuid;

    // Three independent pre-merge local actions must survive finalization.
    REQUIRE(db.putObject(actor, left->uuid, shared, kind, QByteArray("LOCAL"), 2, &error));
    REQUIRE(db.deleteObject(actor, left->uuid, remove_local, 1, &error));
    REQUIRE(db.putObject(actor, left->uuid, local_new, kind, QByteArray("LOCAL-created"), -1, &error));
    REQUIRE(db.revision(actor, head, &error)->generation == 3);
    REQUIRE(db.objectGroupsForRevision(actor, head, &error).size() == 3);
    REQUIRE(db.objectHistoryForRevision(actor, head, &error).size() == 3);
    const std::optional<aowis::db::ObjectMergePreview> preview = db.previewObjectMerge(
        actor, head, right_final->finalized_revision_uuid, &error);
    REQUIRE(preview && preview->left_generation == 3 && preview->conflict_count == 2);
    REQUIRE(preview->left_revision_uuid == head);
    REQUIRE(preview->base_revision_uuid == base->finalized_revision_uuid);
    REQUIRE(preview->left_physical_sha256.size() == 32);
    bool saw_local_only = false;
    bool saw_right_only = false;
    for (const aowis::db::ObjectMergeChange &change : preview->changes)
    {
        if (change.object_uuid == local_new)
            saw_local_only = change.kind == ObjectMergeKind::LeftOnly;
        if (change.object_uuid == right_new)
            saw_right_only = change.kind == ObjectMergeKind::RightOnly;
    }
    REQUIRE(saw_local_only && saw_right_only);
    const QVector<ObjectMergeResolution> choices{
        {shared, ObjectMergeSide::Right}, {remove_local, ObjectMergeSide::Left}};

    // A stale generation, incomplete choices or an unsupported resolution may
    // not mutate the existing local journal, even before starting a write.
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, head, 2, right_final->finalized_revision_uuid,
        choices, QStringLiteral("stale"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, head, 3, right_final->finalized_revision_uuid,
        {{shared, ObjectMergeSide::Right}}, QStringLiteral("unresolved"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, head, 3, right_final->finalized_revision_uuid,
        {{shared, ObjectMergeSide::Right}, {remove_local, ObjectMergeSide::Left},
        {right_new, ObjectMergeSide::Right}}, QStringLiteral("invalid"), {}, &error));
    REQUIRE(db.revision(actor, head, &error)->generation == 3);
    REQUIRE(db.objectGroupsForRevision(actor, head, &error).size() == 3);

    // Simulate failure after all object mutations. Preexisting local work and
    // its journal must remain, while the merge's new group/parent rolls back.
    {
        QSqlDatabase sql = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("local-merge-trigger"));
        sql.setDatabaseName(path);
        REQUIRE(sql.open());
        QSqlQuery query(sql);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_local_merge BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'injected parent failure'); END")));
        sql.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("local-merge-trigger"));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, head, 3, right_final->finalized_revision_uuid,
        choices, QStringLiteral("rollback"), {}, &error));
    REQUIRE(db.revision(actor, head, &error)->generation == 3);
    REQUIRE(db.revision(actor, head, &error)->parent_revision_uuids.size() == 1);
    REQUIRE(db.object(actor, head, shared, &error)->content == QByteArray("LOCAL"));
    REQUIRE(db.object(actor, head, remove_local, &error)->deleted);
    REQUIRE(!db.object(actor, head, right_new, &error));
    REQUIRE(db.objectGroupsForRevision(actor, head, &error).size() == 3);
    REQUIRE(db.objectHistoryForRevision(actor, head, &error).size() == 3);
    {
        QSqlDatabase sql = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("local-merge-trigger"));
        sql.setDatabaseName(path);
        REQUIRE(sql.open());
        QSqlQuery query(sql);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER fail_local_merge")));
        sql.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("local-merge-trigger"));

    const std::optional<aowis::db::Finalization> merged = db.mergeObjectRevisions(
        actor, left->uuid, head, 3, right_final->finalized_revision_uuid,
        choices, QStringLiteral("keep local delete"), QStringLiteral("local + remote"), &error);
    REQUIRE(merged && merged->finalized_revision_uuid == head);
    const std::optional<aowis::db::RevisionInfo> finalized = db.revision(actor, head, &error);
    REQUIRE(finalized && finalized->finalized && finalized->generation == 6);
    REQUIRE(finalized->parent_revision_uuids.size() == 2);
    REQUIRE(finalized->parent_revision_uuids.at(0) == left_final->finalized_revision_uuid);
    REQUIRE(finalized->parent_revision_uuids.at(1) == right_final->finalized_revision_uuid);
    REQUIRE(db.object(actor, head, shared, &error)->content == QByteArray("R"));
    REQUIRE(db.object(actor, head, remove_local, &error)->deleted);
    REQUIRE(db.object(actor, head, local_new, &error)->content == QByteArray("LOCAL-created"));
    REQUIRE(db.object(actor, head, left_only, &error)->content == QByteArray("left"));
    REQUIRE(db.object(actor, head, update_right, &error)->content == QByteArray("R-updated"));
    REQUIRE(db.object(actor, head, right_new, &error)->content == QByteArray("R-created"));
    REQUIRE(db.objectGroupsForRevision(actor, head, &error).size() == 4);
    REQUIRE(db.objectHistoryForRevision(actor, head, &error).size() == 6);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // Opposite conflict choices: keep locally modified shared content but
    // explicitly adopt the remote version of a locally deleted entity.
    const std::optional<aowis::db::BranchInfo> alternate = db.createBranch(actor,
        project->project_uuid, QStringLiteral("alternate"), left_final->finalized_revision_uuid, &error);
    REQUIRE(alternate);
    REQUIRE(db.putObject(actor, alternate->uuid, shared, kind, QByteArray("LOCAL-2"), 2, &error));
    REQUIRE(db.deleteObject(actor, alternate->uuid, remove_local, 1, &error));
    const std::optional<aowis::db::Finalization> alternate_merge = db.mergeObjectRevisions(
        actor, alternate->uuid, alternate->head_revision_uuid, 2,
        right_final->finalized_revision_uuid,
        {{shared, ObjectMergeSide::Left}, {remove_local, ObjectMergeSide::Right}},
        QStringLiteral("alternate merge"), {}, &error);
    REQUIRE(alternate_merge);
    REQUIRE(db.object(actor, alternate_merge->finalized_revision_uuid, shared, &error)->content == QByteArray("LOCAL-2"));
    REQUIRE(db.object(actor, alternate_merge->finalized_revision_uuid, remove_local, &error)->content == QByteArray("R-replacement"));
    REQUIRE(db.objectGroupsForRevision(actor, alternate_merge->finalized_revision_uuid, &error).size() == 3);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // A branch whose local changes already match the entire remote result
    // still makes a two-parent merge commit, with no artificial edit group.
    const std::optional<aowis::db::BranchInfo> converged = db.createBranch(actor,
        project->project_uuid, QStringLiteral("converged"), left_final->finalized_revision_uuid, &error);
    REQUIRE(converged);
    REQUIRE(db.putObject(actor, converged->uuid, shared, kind, QByteArray("R"), 2, &error));
    REQUIRE(db.putObject(actor, converged->uuid, remove_local, kind, QByteArray("R-replacement"), 1, &error));
    REQUIRE(db.putObject(actor, converged->uuid, update_right, kind, QByteArray("R-updated"), 1, &error));
    REQUIRE(db.putObject(actor, converged->uuid, right_new, kind, QByteArray("R-created"), -1, &error));
    const std::optional<aowis::db::Finalization> converged_merge = db.mergeObjectRevisions(
        actor, converged->uuid, converged->head_revision_uuid, 4,
        right_final->finalized_revision_uuid, {}, QStringLiteral("zero edits"), {}, &error);
    REQUIRE(converged_merge);
    REQUIRE(db.objectHistoryForRevision(actor, converged_merge->finalized_revision_uuid, &error).size() == 4);
    REQUIRE(db.objectGroupsForRevision(actor, converged_merge->finalized_revision_uuid, &error).size() == 4);
    REQUIRE(db.revision(actor, converged_merge->finalized_revision_uuid, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // Undo and redo groups from before the merge must retain their identity
    // and sequence; the merge appends its own group after them.
    const std::optional<aowis::db::BranchInfo> history = db.createBranch(actor,
        project->project_uuid, QStringLiteral("undo-redo"), left_final->finalized_revision_uuid, &error);
    REQUIRE(history);
    const QUuid history_object = createUuidV7();
    REQUIRE(db.putObject(actor, history->uuid, history_object, kind, QByteArray("kept"), -1, &error));
    REQUIRE(db.undoObjectGroup(actor, history->uuid, 1, &error));
    REQUIRE(db.redoObjectGroup(actor, history->uuid, 2, &error));
    const std::optional<aowis::db::Finalization> history_merge = db.mergeObjectRevisions(
        actor, history->uuid, history->head_revision_uuid, 3,
        right_final->finalized_revision_uuid, {{shared, ObjectMergeSide::Left}},
        QStringLiteral("preserve undo redo"), {}, &error);
    REQUIRE(history_merge);
    const QVector<aowis::db::ObjectGroup> groups = db.objectGroupsForRevision(
        actor, history_merge->finalized_revision_uuid, &error);
    REQUIRE(groups.size() == 4);
    REQUIRE(groups.at(0).action == QStringLiteral("edit"));
    REQUIRE(groups.at(1).action == QStringLiteral("undo"));
    REQUIRE(groups.at(2).action == QStringLiteral("redo"));
    REQUIRE(groups.at(3).action == QStringLiteral("edit"));
    REQUIRE(db.object(actor, history_merge->finalized_revision_uuid, history_object, &error)->content == QByteArray("kept"));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.revision(actor, head, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.objectHistoryForRevision(actor, head, &error).size() == 6);
    return 0;
}
