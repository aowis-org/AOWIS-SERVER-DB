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
    const QString file = directory.filePath(QStringLiteral("audit.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifySecurityAudit({createUuidV7()}).valid == false);
    const std::optional<aowis::db::UserInfo> administrator =
        db.bootstrapAdministrator(QStringLiteral("administrator"), &error);
    REQUIRE(administrator.has_value());
    const OperationContext root{administrator->uuid};
    const QVector<aowis::db::SecurityAuditEvent> bootstrap = db.securityAudit(root, 0, 100, &error);
    REQUIRE(bootstrap.size() == 1);
    REQUIRE(bootstrap.first().action == QStringLiteral("administrator.bootstrap"));
    REQUIRE(bootstrap.first().actor_uuid == administrator->uuid);
    REQUIRE(bootstrap.first().subject_uuid == administrator->uuid);
    REQUIRE(db.verifySecurityAudit(root).valid);

    const std::optional<aowis::db::UserInfo> guest = db.createUser(root, QStringLiteral("engineer"), &error);
    REQUIRE(guest.has_value());
    const OperationContext nonadmin{guest->uuid};
    REQUIRE(db.securityAudit(nonadmin, 0, 100, &error).isEmpty());
    REQUIRE(!db.verifySecurityAudit(nonadmin).valid);
    REQUIRE(!db.createGroup(nonadmin, QStringLiteral("denied"), &error));
    REQUIRE(!db.createUser(root, QStringLiteral("engineer"), &error));
    REQUIRE(db.securityAudit(root, 0, 100, &error).size() == 2);

    const std::optional<aowis::db::GroupInfo> group = db.createGroup(root, QStringLiteral("team"), &error);
    const std::optional<aowis::db::RoleInfo> role = db.createRole(root, QStringLiteral("engineering.viewer"), &error);
    const std::optional<aowis::db::PermissionInfo> permission =
        db.createPermission(root, QStringLiteral("custom.view"), &error);
    REQUIRE(group && role && permission);
    REQUIRE(db.addGroupMember(root, group->uuid, guest->uuid, &error).has_value());
    REQUIRE(db.grantRolePermission(root, role->uuid, permission->uuid, &error).has_value());
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        root, QStringLiteral("Audit project"), &error);
    REQUIRE(project.has_value());
    const std::optional<QUuid> assignment = db.assignRole(root, PrincipalKind::Group,
        group->uuid, role->uuid, AccessScope::project(project->project_uuid), &error);
    REQUIRE(assignment.has_value());
    REQUIRE(db.hasPermission(nonadmin, QStringLiteral("custom.view"),
        AccessScope::project(project->project_uuid), &error));

    const std::optional<aowis::db::IssuedSession> session = db.issueSession(root, guest->uuid, 60, &error);
    REQUIRE(session.has_value());
    REQUIRE(db.authenticateSession(session->bearer_token, &error).has_value());
    REQUIRE(db.revokeSession(root, session->uuid, &error));
    REQUIRE(!db.revokeSession(root, session->uuid, &error));
    REQUIRE(db.disableUser(root, guest->uuid, &error));
    REQUIRE(!db.createRole(nonadmin, QStringLiteral("forbidden"), &error));
    REQUIRE(!db.issueSession(root, guest->uuid, 60, &error));

    const QVector<aowis::db::SecurityAuditEvent> entries = db.securityAudit(root, 0, 100, &error);
    REQUIRE(entries.size() == 11); // bootstrap, user, group, role, permission,
                                    // membership, role-permission, assignment,
                                    // session issuance, revocation, disable.
    for (int i = 0; i < entries.size(); ++i)
    {
        REQUIRE(entries[i].sequence == i + 1);
        REQUIRE(entries[i].chain_sha256.size() == 32);
        if (i > 0)
            REQUIRE(entries[i].previous_sha256 == entries[i - 1].chain_sha256);
    }
    REQUIRE(entries.last().action == QStringLiteral("user.disable"));
    const QVector<aowis::db::SecurityAuditEvent> page = db.securityAudit(root, 4, 3, &error);
    REQUIRE(page.size() == 3);
    REQUIRE(page.first().sequence == 5 && page.last().sequence == 7);
    REQUIRE(db.securityAudit(root, 0, 1001, &error).isEmpty());
    REQUIRE(db.verifySecurityAudit(root).valid);
    REQUIRE(db.verifySecurityAudit(root).checked_events == entries.size());

    db.close();
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);
    REQUIRE(db.securityAudit(root, 0, 100, &error).size() == entries.size());

    // Audit failure must roll the corresponding security mutation back.
    // A deliberately failing audit INSERT trigger simulates storage failure.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                             QStringLiteral("audit-atomicity"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER audit_fail_insert BEFORE INSERT ON security_audit_events "
            "BEGIN SELECT RAISE(ABORT,'forced audit failure'); END")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("audit-atomicity"));
    REQUIRE(!db.createGroup(root, QStringLiteral("must-rollback"), &error));
    REQUIRE(db.securityAudit(root, 0, 100, &error).size() == entries.size());
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                             QStringLiteral("audit-rollback-check"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("SELECT COUNT(*) FROM groups WHERE name='must-rollback'")));
        REQUIRE(query.next() && query.value(0).toInt() == 0);
        query.finish();
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER audit_fail_insert")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("audit-rollback-check"));

    // Ordinary SQL cannot alter the audit log, even if a client accidentally
    // obtains direct access to SQLite without permission enforcement.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                             QStringLiteral("audit-inspector"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("PRAGMA foreign_keys=ON")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE security_audit_events SET action='forged' WHERE sequence=2")));
        REQUIRE(!query.exec());
        REQUIRE(query.exec(QStringLiteral("DELETE FROM security_audit_events WHERE sequence=2")) == false);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER security_audit_readonly_update")));
        REQUIRE(query.exec(QStringLiteral("UPDATE security_audit_events SET action='forged' WHERE sequence=2")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("audit-inspector"));
    const aowis::db::SecurityAuditReport tampered = db.verifySecurityAudit(root);
    REQUIRE(!tampered.valid && tampered.failed_sequence == 2);
    db.close();
    error.clear();
    REQUIRE(!db.openSqlite(file, &error));
    REQUIRE(error.contains(QStringLiteral("immutability triggers")));
    error.clear();

    // Historical V8 databases upgrade without inventing audit history for
    // old operations. New events begin at sequence 1 after the migration.
    const QString legacy_file = directory.filePath(QStringLiteral("upgrade-v8.sqlite"));
    Database old;
    REQUIRE(old.openSqlite(legacy_file, &error));
    const std::optional<aowis::db::UserInfo> old_admin = old.bootstrapAdministrator(
        QStringLiteral("legacy"), &error);
    REQUIRE(old_admin.has_value());
    const OperationContext old_root{old_admin->uuid};
    const std::optional<aowis::db::UserInfo> old_user = old.createUser(old_root,
        QStringLiteral("legacy-user"), &error);
    REQUIRE(old_user.has_value());
    old.close();
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                           QStringLiteral("audit-legacy"));
        inspect.setDatabaseName(legacy_file);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral("DROP TABLE security_audit_events")));
        // An actual V8 installation has neither the new permission nor its
        // system-administrator grant. Emulate that, not only the table version.
        // This deliberately simulates an older schema; remove the newer V10
        // append-only trigger before deleting the V9-only permission grant.
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER role_permission_no_delete")));
        REQUIRE(query.exec(QStringLiteral(
            "DELETE FROM role_permissions WHERE permission_uuid IN "
            "(SELECT uuid FROM permissions WHERE key='security.audit.read')")));
        REQUIRE(query.exec(QStringLiteral(
            "DELETE FROM permissions WHERE key='security.audit.read'")));
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version=8")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("audit-legacy"));
    REQUIRE(old.openSqlite(legacy_file, &error));
    REQUIRE(old.hasPermission(old_root, QStringLiteral("security.audit.read"),
        aowis::db::AccessScope::system(), &error));
    REQUIRE(old.securityAudit(old_root, 0, 100, &error).isEmpty());
    REQUIRE(old.verifySecurityAudit(old_root).valid);
    REQUIRE(old.createGroup(old_root, QStringLiteral("new-group"), &error).has_value());
    const QVector<aowis::db::SecurityAuditEvent> new_history = old.securityAudit(old_root, 0, 100, &error);
    REQUIRE(new_history.size() == 1 && new_history.first().sequence == 1);
    REQUIRE(old.verifySecurityAudit(old_root).valid);
    old.close();

    // A database advertising schema V9 must not be silently accepted after
    // losing the V9 audit table. Existing audit history must never be
    // recreated or silently discarded when the migration version is already 9.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                            QStringLiteral("audit-incomplete-v9"));
        inspect.setDatabaseName(legacy_file);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version")));
        REQUIRE(query.next() && query.value(0).toInt() == 10);
        query.finish();
        REQUIRE(query.exec(QStringLiteral("DROP TABLE security_audit_events")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("audit-incomplete-v9"));
    error.clear();
    REQUIRE(!old.openSqlite(legacy_file, &error));
    REQUIRE(error.contains(QStringLiteral("security_audit_events")) ||
            error.contains(QStringLiteral("audit schema")));
    return 0;
}
