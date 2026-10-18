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
    // Authorization must resolve persisted UUIDs for group, principal, role,
    // project and branch scopes consistently, not only during SQL insertion.
    const std::optional<aowis::db::RoleInfo> stored_role =
        db.role(root, QStringLiteral("local.editor"), &error);
    const std::optional<aowis::db::PermissionInfo> stored_permission =
        db.permission(root, QStringLiteral("project.model.write"), &error);
    REQUIRE(stored_role && stored_role->uuid == role->uuid);
    REQUIRE(stored_permission && stored_permission->uuid == perm->uuid);
    REQUIRE(!db.hasPermission(worker, QStringLiteral("project.model.write"), AccessScope::system()));
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

    return 0;
}
