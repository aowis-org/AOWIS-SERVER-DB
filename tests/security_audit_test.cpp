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
    REQUIRE(entries[1].action == QStringLiteral("user.create"));
    REQUIRE(entries[1].actor_uuid == administrator->uuid);
    REQUIRE(entries[1].subject_uuid == guest->uuid);
    REQUIRE(entries[2].action == QStringLiteral("group.create"));
    REQUIRE(entries[2].subject_uuid == group->uuid);
    REQUIRE(entries[8].action == QStringLiteral("session.issue"));
    REQUIRE(entries[8].subject_uuid == session->uuid);
    const QVector<aowis::db::SecurityAuditEvent> page = db.securityAudit(root, 4, 3, &error);
    REQUIRE(page.size() == 3);
    REQUIRE(page.first().sequence == 5 && page.last().sequence == 7);
    REQUIRE(page.first().uuid == entries[4].uuid);
    REQUIRE(page.last().actor_uuid == root.actor_uuid);
    REQUIRE(db.securityAudit(root, 0, 1001, &error).isEmpty());
    REQUIRE(db.verifySecurityAudit(root).valid);
    REQUIRE(db.verifySecurityAudit(root).checked_events == entries.size());

    db.close();
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);
    const QVector<aowis::db::SecurityAuditEvent> reopened = db.securityAudit(root, 0, 100, &error);
    REQUIRE(reopened.size() == entries.size());
    REQUIRE(reopened[1].uuid == entries[1].uuid);
    REQUIRE(reopened[1].actor_uuid == root.actor_uuid);
    REQUIRE(reopened[1].subject_uuid == guest->uuid);
    REQUIRE(reopened[1].chain_sha256 == entries[1].chain_sha256);

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
        // The backend's SQL UUID representation stays as 16-byte SQLite BLOBs.
        // Hashing, however, is based on RFC-4122 bytes independently of SQL.
        REQUIRE(query.exec(QStringLiteral(
            "SELECT uuid,actor_uuid,subject_uuid,typeof(uuid),typeof(actor_uuid),"
            "typeof(subject_uuid) FROM security_audit_events WHERE sequence=2")));
        REQUIRE(query.next());
        REQUIRE(query.value(0).toByteArray() == entries[1].uuid.toRfc4122());
        REQUIRE(query.value(1).toByteArray() == administrator->uuid.toRfc4122());
        REQUIRE(query.value(2).toByteArray() == guest->uuid.toRfc4122());
        REQUIRE(query.value(3).toString() == QStringLiteral("blob"));
        REQUIRE(query.value(4).toString() == QStringLiteral("blob"));
        REQUIRE(query.value(5).toString() == QStringLiteral("blob"));
        query.finish();
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

    return 0;
}
