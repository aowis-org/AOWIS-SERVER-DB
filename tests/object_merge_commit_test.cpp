#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::AccessScope;
using aowis::db::Database;
using aowis::db::ObjectMergeResolution;
using aowis::db::ObjectMergeSide;
using aowis::db::OperationContext;
using aowis::db::PrincipalKind;

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
    const QString path = directory.filePath(QStringLiteral("atomic-merge.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(root, QStringLiteral("merge"), &error);
    REQUIRE(project);

    const QUuid shared = createUuidV7();
    const QUuid left_only = createUuidV7();
    const QUuid right_only = createUuidV7();
    const QUuid deleted = createUuidV7();
    const QString kind = QStringLiteral("hydraulic.label");
    REQUIRE(db.putObject(root, project->main_branch_uuid, shared, kind, QByteArray("base"), -1, &error));
    REQUIRE(db.putObject(root, project->main_branch_uuid, deleted, kind, QByteArray("base"), -1, &error));
    const std::optional<aowis::db::Finalization> base = db.finalize(
        root, project->main_branch_uuid, 2, QStringLiteral("base"), {}, &error);
    REQUIRE(base);
    const std::optional<aowis::db::BranchInfo> left = db.createBranch(
        root, project->project_uuid, QStringLiteral("left"), base->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(
        root, project->project_uuid, QStringLiteral("right"), base->finalized_revision_uuid, &error);
    REQUIRE(left && right);
    REQUIRE(db.putObject(root, left->uuid, shared, kind, QByteArray("L"), 1, &error));
    REQUIRE(db.putObject(root, left->uuid, left_only, kind, QByteArray("L only"), -1, &error));
    REQUIRE(db.deleteObject(root, left->uuid, deleted, 1, &error));
    REQUIRE(db.putObject(root, right->uuid, shared, kind, QByteArray("R"), 1, &error));
    REQUIRE(db.putObject(root, right->uuid, right_only, kind, QByteArray("R only"), -1, &error));
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        root, left->uuid, 3, QStringLiteral("left"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        root, right->uuid, 2, QStringLiteral("right"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid target = left_final->new_working_revision_uuid;

    // Fail closed for unresolved conflicts, duplicate/unknown resolutions,
    // and a stale head. No mutation occurs after any of these failures.
    REQUIRE(!db.mergeObjectRevisions(root, left->uuid, target, 0,
        right_final->finalized_revision_uuid, {}, QStringLiteral("merge"), {}, &error));
    const QVector<ObjectMergeResolution> choice{{shared, ObjectMergeSide::Right}};
    REQUIRE(!db.mergeObjectRevisions(root, left->uuid, target, 0,
        right_final->finalized_revision_uuid, {{shared, ObjectMergeSide::Right},
        {shared, ObjectMergeSide::Left}}, QStringLiteral("merge"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(root, left->uuid, target, 0,
        right_final->finalized_revision_uuid, {{right_only, ObjectMergeSide::Right}},
        QStringLiteral("merge"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(root, left->uuid, createUuidV7(), 0,
        right_final->finalized_revision_uuid, choice, QStringLiteral("merge"), {}, &error));

    // Read access to all sources is not sufficient to write or finalize.
    const std::optional<aowis::db::UserInfo> viewer = db.createUser(root, QStringLiteral("viewer"), &error);
    const std::optional<aowis::db::RoleInfo> role = db.createRole(root, QStringLiteral("merge.readonly"), &error);
    const std::optional<aowis::db::PermissionInfo> read = db.permission(root, QStringLiteral("project.read"), &error);
    REQUIRE(viewer && role && read);
    REQUIRE(db.grantRolePermission(root, role->uuid, read->uuid, &error));
    for (const QUuid &branch_uuid : QVector<QUuid>{project->main_branch_uuid, left->uuid, right->uuid})
        REQUIRE(db.assignRole(root, PrincipalKind::User, viewer->uuid, role->uuid,
                              AccessScope::branch(branch_uuid), &error));
    REQUIRE(!db.mergeObjectRevisions(OperationContext{viewer->uuid}, left->uuid, target, 0,
        right_final->finalized_revision_uuid, choice, QStringLiteral("merge"), {}, &error));

    // Simulated crash/constraint failure at the *final* ancestry operation.
    // Earlier changed objects and journal entries must all roll back.
    {
        QSqlDatabase sql = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("merge-atomic-fixture"));
        sql.setDatabaseName(path);
        REQUIRE(sql.open());
        QSqlQuery query(sql);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_merge_parent BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'simulated failed commit'); END")));
        sql.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge-atomic-fixture"));
    REQUIRE(!db.mergeObjectRevisions(root, left->uuid, target, 0,
        right_final->finalized_revision_uuid, choice, QStringLiteral("merge"), {}, &error));
    REQUIRE(db.revision(root, target, &error)->generation == 0);
    REQUIRE(db.revision(root, target, &error)->parent_revision_uuids.size() == 1);
    REQUIRE(db.object(root, target, shared, &error)->content == QByteArray("L"));
    REQUIRE(db.objectGroupsForRevision(root, target, &error).isEmpty());
    REQUIRE(db.objectHistoryForRevision(root, target, &error).isEmpty());
    {
        QSqlDatabase sql = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("merge-atomic-fixture"));
        sql.setDatabaseName(path);
        REQUIRE(sql.open());
        QSqlQuery query(sql);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_merge_parent")));
        sql.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge-atomic-fixture"));

    const std::optional<aowis::db::Finalization> merged = db.mergeObjectRevisions(
        root, left->uuid, target, 0, right_final->finalized_revision_uuid, choice,
        QStringLiteral("merged"), QStringLiteral("two-parent commit"), &error);
    REQUIRE(merged);
    REQUIRE(merged->finalized_revision_uuid == target);
    const std::optional<aowis::db::RevisionInfo> merged_revision = db.revision(root, target, &error);
    REQUIRE(merged_revision && merged_revision->finalized);
    REQUIRE(merged_revision->parent_revision_uuids.size() == 2);
    REQUIRE(merged_revision->parent_revision_uuids.at(0) == left_final->finalized_revision_uuid);
    REQUIRE(merged_revision->parent_revision_uuids.at(1) == right_final->finalized_revision_uuid);
    REQUIRE(db.object(root, target, shared, &error)->content == QByteArray("R"));
    REQUIRE(db.object(root, target, right_only, &error)->content == QByteArray("R only"));
    REQUIRE(db.object(root, target, left_only, &error)->content == QByteArray("L only"));
    REQUIRE(db.object(root, target, deleted, &error)->deleted);
    REQUIRE(db.objectGroupsForRevision(root, target, &error).size() == 1);
    REQUIRE(db.objectHistoryForRevision(root, target, &error).size() == 2);
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    const std::optional<aowis::db::RevisionComparison> ancestry = db.compareRevisions(
        root, left_final->finalized_revision_uuid, merged->finalized_revision_uuid, &error);
    REQUIRE(ancestry && ancestry->left_is_ancestor_of_right);
    const std::optional<aowis::db::RevisionComparison> ancestry_right = db.compareRevisions(
        root, right_final->finalized_revision_uuid, merged->finalized_revision_uuid, &error);
    REQUIRE(ancestry_right && ancestry_right->left_is_ancestor_of_right);
    REQUIRE(!db.mergeObjectRevisions(root, left->uuid, target, 0,
        right_final->finalized_revision_uuid, choice, QStringLiteral("replay"), {}, &error));
    REQUIRE(db.revision(root, merged->new_working_revision_uuid, &error));

    // Choosing left on the same conflict retains that object but still
    // incorporates non-conflicting right-only changes.
    const std::optional<aowis::db::BranchInfo> alternate = db.createBranch(root,
        project->project_uuid, QStringLiteral("alternate"), left_final->finalized_revision_uuid, &error);
    REQUIRE(alternate);
    const std::optional<aowis::db::Finalization> alternate_merge = db.mergeObjectRevisions(
        root, alternate->uuid, alternate->head_revision_uuid, 0,
        right_final->finalized_revision_uuid, {{shared, ObjectMergeSide::Left}},
        QStringLiteral("keep left"), {}, &error);
    REQUIRE(alternate_merge);
    REQUIRE(db.object(root, alternate_merge->finalized_revision_uuid, shared, &error)->content == QByteArray("L"));
    REQUIRE(db.object(root, alternate_merge->finalized_revision_uuid, right_only, &error)->content == QByteArray("R only"));
    REQUIRE(db.objectHistoryForRevision(root, alternate_merge->finalized_revision_uuid, &error).size() == 1);

    // A real local edit must never be overwritten by an old zero-generation
    // preview, even if both source revisions are still valid.
    const std::optional<aowis::db::BranchInfo> edited = db.createBranch(root,
        project->project_uuid, QStringLiteral("locally-edited"), left_final->finalized_revision_uuid, &error);
    REQUIRE(edited);
    const QUuid local_edit = createUuidV7();
    REQUIRE(db.putObject(root, edited->uuid, local_edit, kind, QByteArray("unsaved work"), -1, &error));
    REQUIRE(!db.mergeObjectRevisions(root, edited->uuid, edited->head_revision_uuid, 0,
        right_final->finalized_revision_uuid, choice, QStringLiteral("stale"), {}, &error));
    REQUIRE(db.revision(root, edited->head_revision_uuid, &error)->generation == 1);
    REQUIRE(db.object(root, edited->head_revision_uuid, local_edit, &error)->content == QByteArray("unsaved work"));

    // Divergent histories can converge on identical bytes. Such a merge must
    // still finalize both parents, even though it has zero object edits.
    const std::optional<aowis::db::BranchInfo> same_left = db.createBranch(root,
        project->project_uuid, QStringLiteral("same-left"), base->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> same_right = db.createBranch(root,
        project->project_uuid, QStringLiteral("same-right"), base->finalized_revision_uuid, &error);
    REQUIRE(same_left && same_right);
    REQUIRE(db.putObject(root, same_left->uuid, shared, kind, QByteArray("same"), 1, &error));
    REQUIRE(db.putObject(root, same_right->uuid, shared, kind, QByteArray("same"), 1, &error));
    const std::optional<aowis::db::Finalization> same_left_final = db.finalize(
        root, same_left->uuid, 1, QStringLiteral("same L"), {}, &error);
    const std::optional<aowis::db::Finalization> same_right_final = db.finalize(
        root, same_right->uuid, 1, QStringLiteral("same R"), {}, &error);
    REQUIRE(same_left_final && same_right_final);
    const std::optional<aowis::db::Finalization> no_changes = db.mergeObjectRevisions(
        root, same_left->uuid, same_left_final->new_working_revision_uuid, 0,
        same_right_final->finalized_revision_uuid, {}, QStringLiteral("no edits"), {}, &error);
    REQUIRE(no_changes);
    REQUIRE(db.revision(root, no_changes->finalized_revision_uuid, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.objectHistoryForRevision(root, no_changes->finalized_revision_uuid, &error).isEmpty());
    REQUIRE(db.object(root, no_changes->finalized_revision_uuid, shared, &error)->content == QByteArray("same"));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    REQUIRE(db.revision(root, target, &error)->parent_revision_uuids.size() == 2);
    return 0;
}
