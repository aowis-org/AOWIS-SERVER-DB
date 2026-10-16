#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDebug>
#include <QTemporaryDir>

using aowis::db::AccessScope;
using aowis::db::Database;
using aowis::db::OperationContext;
using aowis::db::PrincipalKind;
using aowis::db::SecurityListOptions;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    Database db;
    const QString file = temporary.filePath(QStringLiteral("security-catalog.sqlite"));
    REQUIRE(db.openSqlite(file, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin);
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> alice = db.createUser(root, QStringLiteral("alice"), &error);
    const std::optional<aowis::db::UserInfo> bob = db.createUser(root, QStringLiteral("bob"), &error);
    const std::optional<aowis::db::GroupInfo> team = db.createGroup(root, QStringLiteral("engineers"), &error);
    const std::optional<aowis::db::RoleInfo> editor = db.createRole(root, QStringLiteral("test.editor"), &error);
    const std::optional<aowis::db::PermissionInfo> custom = db.createPermission(root, QStringLiteral("test.custom"), &error);
    const std::optional<aowis::db::PermissionInfo> builtin = db.permission(root, QStringLiteral("project.model.write"), &error);
    REQUIRE(alice && bob && team && editor && custom && builtin);
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(root, QStringLiteral("Site"), &error);
    REQUIRE(project);

    const std::optional<QUuid> member = db.addGroupMember(root, team->uuid, alice->uuid, &error);
    const std::optional<QUuid> grant = db.grantRolePermission(root, editor->uuid, builtin->uuid, &error);
    const std::optional<QUuid> assigned = db.assignRole(root, PrincipalKind::Group,
        team->uuid, editor->uuid, AccessScope::project(project->project_uuid), &error);
    REQUIRE(member && grant && assigned);
    const OperationContext guest{bob->uuid};

    // Administrator-only reads must not leak identities or relationship data.
    error.clear();
    REQUIRE(db.securityUsers(guest, {}, &error).isEmpty());
    REQUIRE(error.contains(QStringLiteral("Permission denied")));
    error.clear();
    REQUIRE(db.securityGroupMembers(guest, team->uuid, {}, &error).isEmpty());
    REQUIRE(error.contains(QStringLiteral("Permission denied")));
    error.clear();
    REQUIRE(db.securityRoleAssignments(guest, PrincipalKind::Group, team->uuid, {}, &error).isEmpty());
    REQUIRE(error.contains(QStringLiteral("Permission denied")));

    error.clear();
    const qint64 audit_before = db.verifySecurityAudit(root).checked_events;
    const QVector<aowis::db::UserInfo> users = db.securityUsers(root, {}, &error);
    REQUIRE(error.isEmpty() && users.size() == 3);
    REQUIRE(db.securityGroups(root, {}, &error).size() == 1);
    REQUIRE(db.securityRoles(root, {}, &error).size() >= 3);
    REQUIRE(db.securityPermissions(root, {}, &error).size() >= 10);
    REQUIRE(db.securityGroupMembers(root, team->uuid, {}, &error).size() == 1);
    REQUIRE(db.securityRolePermissions(root, editor->uuid, {}, &error).size() == 1);
    const QVector<aowis::db::RoleAssignmentInfo> assignments = db.securityRoleAssignments(
        root, PrincipalKind::Group, team->uuid, {}, &error);
    REQUIRE(assignments.size() == 1 && assignments[0].uuid == *assigned);
    REQUIRE(assignments[0].scope.kind == aowis::db::AccessScopeKind::Project);
    REQUIRE(assignments[0].scope.resource_uuid == project->project_uuid);
    REQUIRE(db.securityRoleAssignments(root, PrincipalKind::User, alice->uuid, {}, &error).isEmpty());
    REQUIRE(db.verifySecurityAudit(root).checked_events == audit_before);

    // UUID cursor pagination must never repeat an entry or miss an existing one.
    QVector<QUuid> page_ids;
    SecurityListOptions page;
    page.limit = 1;
    for (int index = 0; index < 3; ++index)
    {
        const QVector<aowis::db::UserInfo> records = db.securityUsers(root, page, &error);
        REQUIRE(records.size() == 1);
        page_ids.append(records[0].uuid);
        page.after_uuid = records[0].uuid;
    }
    REQUIRE(db.securityUsers(root, page, &error).isEmpty());
    REQUIRE(page_ids.size() == 3 && page_ids[0] != page_ids[1] && page_ids[1] != page_ids[2]);
    SecurityListOptions bad_page;
    bad_page.limit = 201;
    error.clear();
    REQUIRE(db.securityUsers(root, bad_page, &error).isEmpty());
    REQUIRE(error.contains(QStringLiteral("1..200")));

    REQUIRE(db.removeGroupMember(root, *member, &error));
    REQUIRE(db.revokeRolePermission(root, *grant, &error));
    REQUIRE(db.revokeRoleAssignment(root, *assigned, &error));
    REQUIRE(db.disableUser(root, bob->uuid, &error));
    error.clear();
    REQUIRE(db.securityUsers(root, {}, &error).size() == 2);
    REQUIRE(db.securityGroupMembers(root, team->uuid, {}, &error).isEmpty());
    REQUIRE(db.securityRolePermissions(root, editor->uuid, {}, &error).isEmpty());
    REQUIRE(db.securityRoleAssignments(root, PrincipalKind::Group, team->uuid, {}, &error).isEmpty());

    SecurityListOptions history;
    history.include_inactive = true;
    const QVector<aowis::db::UserInfo> all_users = db.securityUsers(root, history, &error);
    REQUIRE(all_users.size() == 3);
    bool saw_disabled_bob = false;
    for (const aowis::db::UserInfo &user : all_users)
    {
        if (user.uuid == bob->uuid)
            saw_disabled_bob = user.disabled;
    }
    REQUIRE(saw_disabled_bob);
    const QVector<aowis::db::GroupMembershipInfo> old_members = db.securityGroupMembers(root, team->uuid, history, &error);
    const QVector<aowis::db::RolePermissionGrantInfo> old_grants = db.securityRolePermissions(root, editor->uuid, history, &error);
    const QVector<aowis::db::RoleAssignmentInfo> old_assignments = db.securityRoleAssignments(root, PrincipalKind::Group, team->uuid, history, &error);
    REQUIRE(old_members.size() == 1 && old_members[0].uuid == *member && old_members[0].revoked);
    REQUIRE(old_grants.size() == 1 && old_grants[0].uuid == *grant && old_grants[0].revoked);
    REQUIRE(old_assignments.size() == 1 && old_assignments[0].uuid == *assigned && old_assignments[0].revoked);

    const std::optional<QUuid> new_member = db.addGroupMember(root, team->uuid, alice->uuid, &error);
    const std::optional<QUuid> new_grant = db.grantRolePermission(root, editor->uuid, builtin->uuid, &error);
    const std::optional<QUuid> new_assignment = db.assignRole(root, PrincipalKind::Group,
        team->uuid, editor->uuid, AccessScope::project(project->project_uuid), &error);
    REQUIRE(new_member && new_grant && new_assignment);
    REQUIRE(*new_member != *member && *new_grant != *grant && *new_assignment != *assigned);
    REQUIRE(db.securityGroupMembers(root, team->uuid, history, &error).size() == 2);
    REQUIRE(db.securityGroupMembers(root, team->uuid, {}, &error).size() == 1);
    REQUIRE(db.securityRolePermissions(root, editor->uuid, history, &error).size() == 2);
    REQUIRE(db.securityRoleAssignments(root, PrincipalKind::Group, team->uuid, history, &error).size() == 2);
    REQUIRE(db.verifySecurityAudit(root).valid);

    db.close();
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.securityGroupMembers(root, team->uuid, history, &error).size() == 2);
    REQUIRE(db.securityRolePermissions(root, editor->uuid, history, &error).size() == 2);
    REQUIRE(db.securityRoleAssignments(root, PrincipalKind::Group, team->uuid, history, &error).size() == 2);
    REQUIRE(db.securityUsers(root, {}, &error).size() == 2);
    REQUIRE(db.verifySecurityAudit(root).valid);
    return 0;
}
