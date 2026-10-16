#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <algorithm>

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::AccessScope;
using aowis::db::Database;
using aowis::db::OperationContext;
using aowis::db::PrincipalKind;
using aowis::db::RevisionComparison;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("revision-ancestry.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};

    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        root, QStringLiteral("DAG ancestry"), &error);
    REQUIRE(project.has_value());
    const std::optional<RevisionComparison> initial_working = db.compareRevisions(
        root, project->working_revision_uuid, project->working_revision_uuid, &error);
    REQUIRE(initial_working.has_value());
    REQUIRE(initial_working->left_is_ancestor_of_right && initial_working->right_is_ancestor_of_left);
    REQUIRE(initial_working->merge_base_revision_uuids.isEmpty());
    REQUIRE(db.replaceWorkingSnapshot(root, project->main_branch_uuid, 0, QByteArray("root"), &error));
    const std::optional<aowis::db::Finalization> first = db.finalize(
        root, project->main_branch_uuid, 1, QStringLiteral("Root"), {}, &error);
    REQUIRE(first.has_value());

    // Equality and linear ancestry. The working head is a valid comparison
    // endpoint even though it has not been finalized yet.
    const std::optional<RevisionComparison> same = db.compareRevisions(
        root, first->finalized_revision_uuid, first->finalized_revision_uuid, &error);
    REQUIRE(same.has_value());
    REQUIRE(same->left_is_ancestor_of_right && same->right_is_ancestor_of_left);
    REQUIRE(same->merge_base_revision_uuids == QVector<QUuid>{first->finalized_revision_uuid});
    const std::optional<RevisionComparison> working = db.compareRevisions(
        root, first->finalized_revision_uuid, first->new_working_revision_uuid, &error);
    REQUIRE(working.has_value());
    REQUIRE(working->left_is_ancestor_of_right && !working->right_is_ancestor_of_left);
    REQUIRE(working->merge_base_revision_uuids == QVector<QUuid>{first->finalized_revision_uuid});
    const std::optional<RevisionComparison> same_working = db.compareRevisions(
        root, first->new_working_revision_uuid, first->new_working_revision_uuid, &error);
    REQUIRE(same_working.has_value());
    REQUIRE(same_working->left_is_ancestor_of_right && same_working->right_is_ancestor_of_left);
    REQUIRE(same_working->merge_base_revision_uuids == QVector<QUuid>{first->finalized_revision_uuid});

    const std::optional<aowis::db::BranchInfo> left = db.createBranch(
        root, project->project_uuid, QStringLiteral("left"), first->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(
        root, project->project_uuid, QStringLiteral("right"), first->finalized_revision_uuid, &error);
    REQUIRE(left && right);
    const std::optional<aowis::db::Finalization> left_one = db.finalize(
        root, left->uuid, 0, QStringLiteral("Left one"), {}, &error);
    const std::optional<aowis::db::Finalization> right_one = db.finalize(
        root, right->uuid, 0, QStringLiteral("Right one"), {}, &error);
    REQUIRE(left_one && right_one);

    const std::optional<RevisionComparison> fork = db.compareRevisions(
        root, left_one->finalized_revision_uuid, right_one->finalized_revision_uuid, &error);
    REQUIRE(fork.has_value());
    REQUIRE(fork->project_uuid == project->project_uuid);
    REQUIRE(!fork->left_is_ancestor_of_right && !fork->right_is_ancestor_of_left);
    REQUIRE(fork->merge_base_revision_uuids == QVector<QUuid>{first->finalized_revision_uuid});

    // A user with branch-scoped read rights cannot compare against a private
    // sibling revision, even when the base is known by UUID.
    const std::optional<aowis::db::UserInfo> viewer = db.createUser(root, QStringLiteral("viewer"), &error);
    const std::optional<aowis::db::RoleInfo> read_role = db.createRole(root, QStringLiteral("reviewer"), &error);
    REQUIRE(viewer && read_role);
    REQUIRE(db.grantRolePermission(root, read_role->uuid,
        db.permission(root, QStringLiteral("project.read"), &error)->uuid, &error));
    REQUIRE(db.assignRole(root, PrincipalKind::User, viewer->uuid, read_role->uuid,
        AccessScope::branch(left->uuid), &error));
    const OperationContext restricted{viewer->uuid};
    REQUIRE(db.revision(restricted, left_one->finalized_revision_uuid, &error).has_value());
    REQUIRE(!db.revision(restricted, right_one->finalized_revision_uuid, &error).has_value());
    REQUIRE(!db.compareRevisions(restricted, left_one->finalized_revision_uuid,
        right_one->finalized_revision_uuid, &error));
    REQUIRE(!db.compareRevisions(restricted, right_one->finalized_revision_uuid,
        left_one->finalized_revision_uuid, &error));
    REQUIRE(!db.compareRevisions(root, createUuidV7(), right_one->finalized_revision_uuid, &error));

    const std::optional<aowis::db::ProjectCreation> other = db.createProject(
        root, QStringLiteral("Unrelated"), &error);
    REQUIRE(other.has_value());
    REQUIRE(!db.compareRevisions(root, other->working_revision_uuid,
        left_one->finalized_revision_uuid, &error));

    // Build a criss-cross DAG with the already-supported ordered parent
    // storage. Production does not yet expose a merge operation. In this
    // fixture, the two new finalized revisions each have both sibling tips
    // as their parents. A correct merge-base algorithm must return BOTH tips.
    {
        QSqlDatabase writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("dag-fixture"));
        writer.setDatabaseName(path);
        REQUIRE(writer.open());
        REQUIRE(writer.transaction());
        {
            QSqlQuery insert(writer);
            REQUIRE(insert.prepare(QStringLiteral(
                "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) "
                "VALUES (?,1,?)")));
            insert.addBindValue(left_one->new_working_revision_uuid.toRfc4122());
            insert.addBindValue(right_one->finalized_revision_uuid.toRfc4122());
            REQUIRE(insert.exec());
            insert.finish();
            REQUIRE(insert.prepare(QStringLiteral(
                "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) "
                "VALUES (?,1,?)")));
            insert.addBindValue(right_one->new_working_revision_uuid.toRfc4122());
            insert.addBindValue(left_one->finalized_revision_uuid.toRfc4122());
            REQUIRE(insert.exec());
        }
        REQUIRE(writer.commit());
        writer.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("dag-fixture"));

    const std::optional<aowis::db::Finalization> left_two = db.finalize(
        root, left->uuid, 0, QStringLiteral("Left merge fixture"), {}, &error);
    const std::optional<aowis::db::Finalization> right_two = db.finalize(
        root, right->uuid, 0, QStringLiteral("Right merge fixture"), {}, &error);
    REQUIRE(left_two && right_two);
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);

    QVector<QUuid> expected_bases{left_one->finalized_revision_uuid, right_one->finalized_revision_uuid};
    std::sort(expected_bases.begin(), expected_bases.end(),
              [](const QUuid &a, const QUuid &b) { return a.toRfc4122() < b.toRfc4122(); });
    const std::optional<RevisionComparison> criss_cross = db.compareRevisions(
        root, left_two->finalized_revision_uuid, right_two->finalized_revision_uuid, &error);
    REQUIRE(criss_cross.has_value());
    REQUIRE(!criss_cross->left_is_ancestor_of_right && !criss_cross->right_is_ancestor_of_left);
    REQUIRE(criss_cross->merge_base_revision_uuids == expected_bases);
    const std::optional<RevisionComparison> reverse = db.compareRevisions(
        root, right_two->finalized_revision_uuid, left_two->finalized_revision_uuid, &error);
    REQUIRE(reverse && reverse->merge_base_revision_uuids == expected_bases);

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<RevisionComparison> reopened = db.compareRevisions(
        root, left_two->finalized_revision_uuid, right_two->finalized_revision_uuid, &error);
    REQUIRE(reopened && reopened->merge_base_revision_uuids == expected_bases);
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    return 0;
}
