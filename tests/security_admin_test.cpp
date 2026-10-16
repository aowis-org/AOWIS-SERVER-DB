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
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("security-admin.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    // Self-disabling the sole administrator must not make recovery impossible.
    REQUIRE(!db.disableUser(root, admin->uuid, &error));
    REQUIRE(db.verifySecurityAudit(root).checked_events == 1);
    const std::optional<aowis::db::UserInfo> engineer = db.createUser(root, QStringLiteral("engineer"), &error);
    const std::optional<aowis::db::GroupInfo> group = db.createGroup(root, QStringLiteral("crew"), &error);
    const std::optional<aowis::db::RoleInfo> role = db.createRole(root, QStringLiteral("local.editor"), &error);
    const std::optional<aowis::db::PermissionInfo> perm = db.permission(root, QStringLiteral("project.model.write"), &error);
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(root, QStringLiteral("Water"), &error);
    REQUIRE(engineer && group && role && perm && project);
    const OperationContext worker{engineer->uuid};
    const AccessScope scope = AccessScope::project(project->project_uuid);
    const AccessScope branch = AccessScope::branch(project->main_branch_uuid);

    const std::optional<QUuid> membership = db.addGroupMember(root, group->uuid, engineer->uuid, &error);
    const std::optional<QUuid> grant = db.grantRolePermission(root, role->uuid, perm->uuid, &error);
    const std::optional<QUuid> assignment = db.assignRole(root, PrincipalKind::Group,
        group->uuid, role->uuid, scope, &error);
    REQUIRE(membership && grant && assignment);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    const qint64 baseline = db.verifySecurityAudit(root).checked_events;
    REQUIRE(!db.removeGroupMember(worker, *membership, &error));
    REQUIRE(!db.revokeRolePermission(worker, *grant, &error));
    REQUIRE(!db.revokeRoleAssignment(worker, *assignment, &error));
    REQUIRE(!db.enableUser(worker, engineer->uuid, &error));
    REQUIRE(db.verifySecurityAudit(root).checked_events == baseline);

    REQUIRE(db.removeGroupMember(root, *membership, &error));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    REQUIRE(!db.removeGroupMember(root, *membership, &error));
    const std::optional<QUuid> membership2 = db.addGroupMember(root, group->uuid, engineer->uuid, &error);
    REQUIRE(membership2 && *membership2 != *membership);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));

    REQUIRE(db.revokeRolePermission(root, *grant, &error));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    REQUIRE(!db.revokeRolePermission(root, *grant, &error));
    const std::optional<QUuid> grant2 = db.grantRolePermission(root, role->uuid, perm->uuid, &error);
    REQUIRE(grant2 && *grant2 != *grant);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));

    REQUIRE(db.revokeRoleAssignment(root, *assignment, &error));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    REQUIRE(!db.revokeRoleAssignment(root, *assignment, &error));
    const std::optional<QUuid> assignment2 = db.assignRole(root, PrincipalKind::Group,
        group->uuid, role->uuid, scope, &error);
    REQUIRE(assignment2 && *assignment2 != *assignment);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));

    // An account reactivation must not make old bearer tokens valid again.
    const std::optional<aowis::db::IssuedSession> session = db.issueSession(root, engineer->uuid, 3600, &error);
    REQUIRE(session.has_value());
    REQUIRE(db.authenticateSession(session->bearer_token, &error).has_value());
    REQUIRE(db.disableUser(root, engineer->uuid, &error));
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    REQUIRE(!db.authenticateSession(session->bearer_token, &error));
    REQUIRE(db.enableUser(root, engineer->uuid, &error));
    REQUIRE(!db.enableUser(root, engineer->uuid, &error));
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    REQUIRE(!db.authenticateSession(session->bearer_token, &error));
    const std::optional<aowis::db::IssuedSession> new_session = db.issueSession(root, engineer->uuid, 3600, &error);
    REQUIRE(new_session && db.authenticateSession(new_session->bearer_token, &error));

    // Revocation requires one audit event and cannot partially commit when
    // audit storage fails. Direct SQL mutation is likewise blocked.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("admin-inspector"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("PRAGMA foreign_keys=ON")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE group_memberships SET user_uuid=? WHERE uuid=?")));
        query.addBindValue(admin->uuid.toRfc4122());
        query.addBindValue(membership2->toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.prepare(QStringLiteral("DELETE FROM role_assignments WHERE uuid=?")));
        query.addBindValue(assignment2->toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.prepare(QStringLiteral("UPDATE role_permissions SET revoked_at_ms=NULL WHERE uuid=?")));
        query.addBindValue(grant->toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_audit_revoke BEFORE INSERT ON security_audit_events "
            "BEGIN SELECT RAISE(ABORT,'audit failure'); END")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("admin-inspector"));
    const qint64 count = db.verifySecurityAudit(root).checked_events;
    REQUIRE(!db.removeGroupMember(root, *membership2, &error));
    REQUIRE(db.verifySecurityAudit(root).checked_events == count);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("admin-cleanup"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_audit_revoke")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("admin-cleanup"));
    REQUIRE(db.verifySecurityAudit(root).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);
    REQUIRE(db.hasPermission(worker, QStringLiteral("project.model.write"), branch));
    REQUIRE(!db.authenticateSession(session->bearer_token, &error));
    REQUIRE(db.authenticateSession(new_session->bearer_token, &error).has_value());

    // Simulate an actual V9 security catalog: old uniqueness constraints and
    // no revocation columns. The original grants must survive migration.
    const QString legacy_path = directory.filePath(QStringLiteral("legacy-v9.sqlite"));
    Database older;
    REQUIRE(older.openSqlite(legacy_path, &error));
    const std::optional<aowis::db::UserInfo> legacy_admin = older.bootstrapAdministrator(QStringLiteral("legacy"), &error);
    REQUIRE(legacy_admin.has_value());
    const OperationContext legacy_root{legacy_admin->uuid};
    const std::optional<aowis::db::UserInfo> legacy_user = older.createUser(legacy_root, QStringLiteral("legacy-worker"), &error);
    const std::optional<aowis::db::GroupInfo> legacy_group = older.createGroup(legacy_root, QStringLiteral("legacy-team"), &error);
    REQUIRE(legacy_user && legacy_group);
    const std::optional<QUuid> legacy_membership = older.addGroupMember(
        legacy_root, legacy_group->uuid, legacy_user->uuid, &error);
    REQUIRE(legacy_membership.has_value());
    older.close();
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-inspector"));
        inspector.setDatabaseName(legacy_path);
        REQUIRE(inspector.open());
        QSqlQuery q(inspector);
        REQUIRE(q.exec(QStringLiteral("PRAGMA foreign_keys=ON")));
        REQUIRE(q.exec(QStringLiteral("CREATE TABLE legacy_members_v9 AS SELECT uuid,group_uuid,user_uuid,created_by_uuid,created_at_ms FROM group_memberships")));
        REQUIRE(q.exec(QStringLiteral("CREATE TABLE legacy_permissions_v9 AS SELECT uuid,role_uuid,permission_uuid FROM role_permissions")));
        REQUIRE(q.exec(QStringLiteral("CREATE TABLE legacy_assignments_v9 AS SELECT uuid,principal_kind,principal_uuid,role_uuid,scope_kind,scope_uuid,created_by_uuid,created_at_ms FROM role_assignments")));
        REQUIRE(q.exec(QStringLiteral("DROP TABLE group_memberships")));
        REQUIRE(q.exec(QStringLiteral("DROP TABLE role_permissions")));
        REQUIRE(q.exec(QStringLiteral("DROP TABLE role_assignments")));
        REQUIRE(q.exec(QStringLiteral("ALTER TABLE legacy_members_v9 RENAME TO group_memberships")));
        REQUIRE(q.exec(QStringLiteral("ALTER TABLE legacy_permissions_v9 RENAME TO role_permissions")));
        REQUIRE(q.exec(QStringLiteral("ALTER TABLE legacy_assignments_v9 RENAME TO role_assignments")));
        REQUIRE(q.exec(QStringLiteral("CREATE TRIGGER role_assignment_no_update BEFORE UPDATE ON role_assignments BEGIN SELECT RAISE(ABORT,'legacy'); END")));
        REQUIRE(q.exec(QStringLiteral("CREATE TRIGGER role_assignment_valid BEFORE INSERT ON role_assignments BEGIN SELECT 1; END")));
        REQUIRE(q.exec(QStringLiteral("DROP TABLE password_recovery_tokens")));
        REQUIRE(q.exec(QStringLiteral("PRAGMA user_version=9")));
        q.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy-inspector"));
    REQUIRE(older.openSqlite(legacy_path, &error));
    REQUIRE(older.hasPermission(legacy_root, QStringLiteral("security.manage"), AccessScope::system()));
    REQUIRE(older.removeGroupMember(legacy_root, *legacy_membership, &error));
    REQUIRE(older.addGroupMember(legacy_root, legacy_group->uuid, legacy_user->uuid, &error));
    REQUIRE(older.verifySecurityAudit(legacy_root).valid);

    // A file claiming V10 but missing an essential security trigger is invalid.
    older.close();
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("broken-v10"));
        inspector.setDatabaseName(legacy_path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER role_assignment_revoke_only")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("broken-v10"));
    error.clear();
    REQUIRE(!older.openSqlite(legacy_path, &error));
    REQUIRE(error.contains(QStringLiteral("V10 security grant schema")));
    return 0;
}
