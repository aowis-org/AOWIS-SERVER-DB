#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::AccessScope;
using aowis::db::Database;
using aowis::db::OperationContext;
using aowis::db::PrincipalKind;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("aowis.sqlite"));
    const QUuid uuid = createUuidV7();
    REQUIRE(!uuid.isNull());
    REQUIRE((static_cast<unsigned char>(uuid.toRfc4122().at(6)) >> 4) == 7);

    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const OperationContext nobody{createUuidV7()};
    REQUIRE(!db.createProject(nobody, QStringLiteral("Unauthenticated"), &error));
    REQUIRE(db.projects(nobody, &error).isEmpty());
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    REQUIRE(!db.bootstrapAdministrator(QStringLiteral("root2"), &error));
    const OperationContext root{admin->uuid};
    REQUIRE(db.hasPermission(root, QStringLiteral("security.manage"), AccessScope::system()));
    REQUIRE(!db.hasPermission(nobody, QStringLiteral("security.manage"), AccessScope::system()));
    REQUIRE(db.role(root, QStringLiteral("system.administrator"), &error).has_value());
    REQUIRE(db.role(root, QStringLiteral("project.owner"), &error).has_value());
    REQUIRE(db.permission(root, QStringLiteral("project.read"), &error).has_value());
    REQUIRE(db.permission(root, QStringLiteral("project.read"), &error)->uuid ==
            QUuid(QStringLiteral("{01a14202-2800-712a-81ad-be2a0fa8aa02}")));

    const std::optional<aowis::db::UserInfo> engineer = db.createUser(root, QStringLiteral("engineer"), &error);
    const std::optional<aowis::db::UserInfo> viewer = db.createUser(root, QStringLiteral("viewer"), &error);
    const std::optional<aowis::db::GroupInfo> team = db.createGroup(root, QStringLiteral("engineers"), &error);
    const std::optional<aowis::db::RoleInfo> editor = db.createRole(root, QStringLiteral("project.editor"), &error);
    REQUIRE(engineer && viewer && team && editor);
    REQUIRE(!db.createUser(root, QStringLiteral("ENGINEER"), &error));
    REQUIRE(!db.createRole(root, QStringLiteral("project.editor"), &error));
    REQUIRE(!db.createPermission(root, QStringLiteral("project.read"), &error));
    REQUIRE(db.createPermission(root, QStringLiteral("custom.export"), &error).has_value());
    REQUIRE(db.addGroupMember(root, team->uuid, engineer->uuid, &error).has_value());
    REQUIRE(!db.addGroupMember(root, team->uuid, engineer->uuid, &error));
    REQUIRE(!db.addGroupMember(root, team->uuid, createUuidV7(), &error));
    REQUIRE(db.grantRolePermission(root, editor->uuid,
            db.permission(root, QStringLiteral("project.read"), &error)->uuid, &error).has_value());
    REQUIRE(db.grantRolePermission(root, editor->uuid,
            db.permission(root, QStringLiteral("project.model.write"), &error)->uuid, &error).has_value());
    REQUIRE(!db.grantRolePermission(root, editor->uuid,
            db.permission(root, QStringLiteral("project.read"), &error)->uuid, &error));

    const std::optional<aowis::db::ProjectCreation> p1 = db.createProject(root, QStringLiteral("Water"), &error);
    const std::optional<aowis::db::ProjectCreation> p2 = db.createProject(root, QStringLiteral("Irrigation"), &error);
    REQUIRE(p1 && p2);
    REQUIRE(p1->project_uuid != p2->project_uuid);
    REQUIRE(p1->main_branch_uuid != p2->main_branch_uuid);
    REQUIRE(p1->working_revision_uuid != p2->working_revision_uuid);
    REQUIRE(db.assignRole(root, PrincipalKind::Group, team->uuid, editor->uuid,
                          AccessScope::project(p1->project_uuid), &error).has_value());
    REQUIRE(!db.assignRole(root, PrincipalKind::User, createUuidV7(), editor->uuid,
                           AccessScope::project(p1->project_uuid), &error));
    REQUIRE(!db.assignRole(root, PrincipalKind::User, viewer->uuid, editor->uuid,
                           AccessScope::project(createUuidV7()), &error));

    const OperationContext worker{engineer->uuid};
    const OperationContext guest{viewer->uuid};
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.read"), AccessScope::project(p1->project_uuid)));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.read"), AccessScope::project(p2->project_uuid)));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.create"), AccessScope::system()));
    REQUIRE(!db.createUser(worker, QStringLiteral("malicious"), &error));
    REQUIRE(!db.createProject(worker, QStringLiteral("malicious"), &error));
    REQUIRE(!db.project(guest, p1->project_uuid, &error));
    REQUIRE(!db.project(worker, p2->project_uuid, &error));
    REQUIRE(db.projects(worker, &error).size() == 1);
    REQUIRE(db.projects(guest, &error).isEmpty());
    REQUIRE(db.projects(root, &error).size() == 2);
    REQUIRE(db.project(root, p1->project_uuid, &error)->name == QStringLiteral("Water"));
    REQUIRE(db.project(root, p1->project_uuid, &error)->created_by_uuid == root.actor_uuid);
    REQUIRE(db.branches(root, p1->project_uuid, &error).size() == 1);
    REQUIRE(db.revisions(root, p1->main_branch_uuid, &error).size() == 1);
    REQUIRE(db.branch(root, p1->main_branch_uuid, &error)->created_by_uuid == root.actor_uuid);

    const std::optional<aowis::db::RevisionInfo> first = db.revision(root, p1->working_revision_uuid, &error);
    REQUIRE(first && !first->finalized && first->number == 1 && first->generation == 0);
    REQUIRE(first->created_by_uuid == root.actor_uuid);
    REQUIRE(!db.createBranch(worker, p1->project_uuid, QStringLiteral("bad"), p1->working_revision_uuid, &error));
    REQUIRE(!db.createBranch(root, p1->project_uuid, QStringLiteral("bad"), p1->working_revision_uuid, &error));

    const QByteArray original("\x00\x01\x02\xff", 4);
    REQUIRE(!db.replaceWorkingSnapshot(guest, p1->main_branch_uuid, 0, original, &error));
    REQUIRE(!db.replaceWorkingSnapshot(worker, p2->main_branch_uuid, 0, original, &error));
    REQUIRE(db.replaceWorkingSnapshot(worker, p1->main_branch_uuid, 0, original, &error));
    REQUIRE(!db.replaceWorkingSnapshot(worker, p1->main_branch_uuid, 0, QByteArray("stale"), &error));
    REQUIRE(db.snapshot(worker, p1->working_revision_uuid, &error) == original);
    REQUIRE(!db.snapshot(guest, p1->working_revision_uuid, &error));
    REQUIRE(db.revision(root, p1->working_revision_uuid, &error)->generation == 1);
    REQUIRE(!db.finalize(worker, p1->main_branch_uuid, 1, QStringLiteral("denied"), {}, &error));
    REQUIRE(!db.finalize(root, p1->main_branch_uuid, 0, QStringLiteral("old"), {}, &error));

    const std::optional<aowis::db::Finalization> finalized = db.finalize(
        root, p1->main_branch_uuid, 1, QStringLiteral("Initial design"), QStringLiteral("Baseline"), &error);
    REQUIRE(finalized.has_value());
    REQUIRE(finalized->finalized_revision_uuid == p1->working_revision_uuid);
    REQUIRE(finalized->chain_sha256.size() == 32);
    REQUIRE(finalized->snapshot_sha256.size() == 32);
    REQUIRE(db.revision(root, p1->working_revision_uuid, &error)->finalized);
    REQUIRE(db.revision(root, p1->working_revision_uuid, &error)->finalized_by_uuid == root.actor_uuid);
    REQUIRE(db.snapshot(root, finalized->new_working_revision_uuid, &error) == original);
    REQUIRE(db.branch(root, p1->main_branch_uuid, &error)->head_revision_uuid == finalized->new_working_revision_uuid);
    REQUIRE(db.revision(root, finalized->new_working_revision_uuid, &error)->number == 2);
    REQUIRE(db.revisions(root, p1->main_branch_uuid, &error).size() == 2);
    REQUIRE(db.revision(root, finalized->new_working_revision_uuid, &error)->parent_revision_uuids == QVector<QUuid>{p1->working_revision_uuid});
    REQUIRE(!db.createBranch(root, p2->project_uuid, QStringLiteral("cross-project"), p1->working_revision_uuid, &error));

    const std::optional<aowis::db::BranchInfo> alternative = db.createBranch(
        root, p1->project_uuid, QStringLiteral("alternative-design"), p1->working_revision_uuid, &error);
    REQUIRE(alternative.has_value());
    REQUIRE(db.branches(root, p1->project_uuid, &error).size() == 2);
    REQUIRE(db.snapshot(root, alternative->head_revision_uuid, &error) == original);
    REQUIRE(db.revision(root, alternative->head_revision_uuid, &error)->number == 1);
    REQUIRE(db.revision(root, alternative->head_revision_uuid, &error)->parent_revision_uuids == QVector<QUuid>{p1->working_revision_uuid});
    REQUIRE(!db.createBranch(root, p1->project_uuid, QStringLiteral("alternative-design"), p1->working_revision_uuid, &error));
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), AccessScope::branch(alternative->uuid)));

    // Role granted to a single user on only one branch; no sibling access.
    REQUIRE(db.assignRole(root, PrincipalKind::User, viewer->uuid, editor->uuid,
                          AccessScope::branch(alternative->uuid), &error).has_value());
    REQUIRE(db.hasPermission(guest, QStringLiteral("project.read"), AccessScope::branch(alternative->uuid)));
    REQUIRE(!db.hasPermission(guest, QStringLiteral("project.read"), AccessScope::branch(p1->main_branch_uuid)));
    REQUIRE(!db.project(guest, p1->project_uuid, &error));
    REQUIRE(db.branch(guest, alternative->uuid, &error).has_value());
    REQUIRE(!db.snapshot(guest, p1->working_revision_uuid, &error));

    const QByteArray different("alternative model bytes");
    REQUIRE(db.replaceWorkingSnapshot(guest, alternative->uuid, 0, different, &error));
    REQUIRE(db.finalize(root, alternative->uuid, 1, QStringLiteral("Alternative"), {}, &error));
    REQUIRE(db.replaceWorkingSnapshot(worker, p1->main_branch_uuid, 0, QByteArray("main design"), &error));
    REQUIRE(db.finalize(root, p1->main_branch_uuid, 1, QStringLiteral("Second main"), {}, &error));
    const aowis::db::IntegrityReport good = db.verifyProject(root, p1->project_uuid);
    REQUIRE(good.valid && good.checked_revisions == 3);
    REQUIRE(db.verifyProject(root, p2->project_uuid).valid);
    REQUIRE(!db.verifyProject(guest, p2->project_uuid).valid);
    REQUIRE(!db.verifyProject(root, createUuidV7()).valid);

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(root, p1->project_uuid).valid);
    REQUIRE(!db.bootstrapAdministrator(QStringLiteral("another"), &error));
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), AccessScope::project(p1->project_uuid)));

    // Project creators automatically receive scoped ownership without acquiring
    // admin permissions on other projects.
    const std::optional<aowis::db::RoleInfo> creator = db.createRole(root, QStringLiteral("project.creator"), &error);
    REQUIRE(creator.has_value());
    REQUIRE(db.grantRolePermission(root, creator->uuid,
            db.permission(root, QStringLiteral("project.create"), &error)->uuid, &error).has_value());
    REQUIRE(db.assignRole(root, PrincipalKind::User, worker.actor_uuid, creator->uuid,
                          AccessScope::system(), &error).has_value());
    const std::optional<aowis::db::ProjectCreation> p3 = db.createProject(worker, QStringLiteral("Engineer design"), &error);
    REQUIRE(p3.has_value());
    REQUIRE(db.project(worker, p3->project_uuid, &error)->created_by_uuid == worker.actor_uuid);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.read"), AccessScope::project(p3->project_uuid)));
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.revision.finalize"), AccessScope::branch(p3->main_branch_uuid)));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.read"), AccessScope::project(p2->project_uuid)));
    REQUIRE(db.replaceWorkingSnapshot(worker, p3->main_branch_uuid, 0, QByteArray("new project"), &error));
    REQUIRE(db.finalize(worker, p3->main_branch_uuid, 1, QStringLiteral("First draft"), {}, &error));
    REQUIRE(db.verifyProject(worker, p3->project_uuid).valid);

    // SQL-level immutable snapshots, and hash detects administrative tampering.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("aowis-test-inspector"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("PRAGMA foreign_keys = ON")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE snapshots SET content=? WHERE revision_uuid=?")));
        query.addBindValue(QByteArray("tampered"));
        query.addBindValue(p1->working_revision_uuid.toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.prepare(QStringLiteral("DELETE FROM revisions WHERE uuid=?")));
        query.addBindValue(p1->working_revision_uuid.toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER finalized_snapshot_readonly_update")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE snapshots SET content=? WHERE revision_uuid=?")));
        query.addBindValue(QByteArray("tampered"));
        query.addBindValue(p1->working_revision_uuid.toRfc4122());
        REQUIRE(query.exec());
        // A privileged administrator also changes the finalizing actor on an
        // independent finalized revision. Its hash must detect the altered provenance.
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER finalized_revision_readonly_update")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE revisions SET finalized_by_uuid=? WHERE uuid=?")));
        query.addBindValue(viewer->uuid.toRfc4122());
        query.addBindValue(p3->working_revision_uuid.toRfc4122());
        REQUIRE(query.exec());
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("aowis-test-inspector"));
    const aowis::db::IntegrityReport bad = db.verifyProject(root, p1->project_uuid);
    REQUIRE(!bad.valid && bad.failed_revision_uuid == p1->working_revision_uuid);
    REQUIRE(db.verifyProject(root, p2->project_uuid).valid);
    REQUIRE(!db.verifyProject(root, p3->project_uuid).valid);
    return 0;
}
