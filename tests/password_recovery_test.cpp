#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("recovery.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> user = db.createUser(root, QStringLiteral("engineer"), &error);
    REQUIRE(user.has_value());
    const OperationContext guest{user->uuid};
    const QByteArray old_password("valid initial password for recovery");
    const QByteArray new_password("valid replacement password for recovery");
    REQUIRE(db.setUserPassword(root, user->uuid, old_password, &error));
    const std::optional<aowis::db::IssuedSession> first_login =
        db.loginWithPassword(QStringLiteral("engineer"), old_password, 3600, &error);
    REQUIRE(first_login.has_value());
    const std::optional<aowis::db::ActiveSession> first_active =
        db.activeSession(first_login->bearer_token, &error);
    REQUIRE(first_active.has_value());
    REQUIRE(first_active->uuid == first_login->uuid && first_active->user_uuid == user->uuid);

    REQUIRE(!db.issuePasswordRecovery(guest, user->uuid, 900, &error));
    REQUIRE(!db.issuePasswordRecovery(root, user->uuid, 3601, &error));
    const std::optional<aowis::db::IssuedRecoveryToken> first =
        db.issuePasswordRecovery(root, user->uuid, 900, &error);
    REQUIRE(first.has_value());
    REQUIRE(first->token.size() == 64);
    REQUIRE(first->uuid != user->uuid);

    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("recovery-inspect"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        query.finish();
        REQUIRE(query.exec(QStringLiteral("SELECT token_sha256 FROM password_recovery_tokens")));
        REQUIRE(query.next());
        REQUIRE(query.value(0).toByteArray().size() == 32);
        REQUIRE(query.value(0).toByteArray() != first->token);
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("recovery-inspect"));

    REQUIRE(!db.redeemPasswordRecovery(QByteArrayLiteral("nonsense"), new_password, &error));
    REQUIRE(!db.redeemPasswordRecovery(first->token, QByteArrayLiteral("short"), &error));
    REQUIRE(db.authenticateSession(first_login->bearer_token, &error).has_value());

    // Issuing another token revokes the older outstanding capability.
    const std::optional<aowis::db::IssuedRecoveryToken> second =
        db.issuePasswordRecovery(root, user->uuid, 900, &error);
    REQUIRE(second.has_value());
    REQUIRE(!db.redeemPasswordRecovery(first->token, new_password, &error));

    // If audit journaling fails, the token stays unused and the password
    // and existing sessions remain exactly as before.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("recovery-rollback"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_recovery_audit BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='credential.recovery.redeem' "
            "BEGIN SELECT RAISE(ABORT,'reject recovery'); END")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("recovery-rollback"));
    REQUIRE(!db.redeemPasswordRecovery(second->token, new_password, &error));
    REQUIRE(db.authenticateSession(first_login->bearer_token, &error).has_value());
    REQUIRE(db.loginWithPassword(QStringLiteral("engineer"), old_password, 3600, &error).has_value());
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("recovery-unblock"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_recovery_audit")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("recovery-unblock"));
    REQUIRE(db.redeemPasswordRecovery(second->token, new_password, &error));

    // The administrator authorized issuance, but did not redeem the token.
    // The redemption belongs to the affected account, not to its issuer.
    // Possession of the one-use recovery capability does not independently
    // establish which human presented it.
    const QVector<aowis::db::SecurityAuditEvent> audit = db.securityAudit(root, 0, 100, &error);
    bool saw_issuance = false;
    bool saw_redemption = false;
    for (const aowis::db::SecurityAuditEvent &event : audit)
    {
        if (event.subject_uuid == second->uuid &&
            event.action == QStringLiteral("credential.recovery.issue"))
        {
            REQUIRE(event.actor_uuid == admin->uuid);
            saw_issuance = true;
        }
        if (event.subject_uuid == second->uuid &&
            event.action == QStringLiteral("credential.recovery.redeem"))
        {
            REQUIRE(event.actor_uuid == user->uuid);
            saw_redemption = true;
        }
    }
    REQUIRE(saw_issuance && saw_redemption);
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
            QStringLiteral("recovery-attribution"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT issued_by_uuid FROM password_recovery_tokens WHERE uuid=?")));
        query.addBindValue(second->uuid.toRfc4122());
        REQUIRE(query.exec() && query.next());
        REQUIRE(query.value(0).toByteArray() == admin->uuid.toRfc4122());
        query.finish();
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT changed_by_uuid FROM user_password_credentials WHERE user_uuid=?")));
        query.addBindValue(user->uuid.toRfc4122());
        REQUIRE(query.exec() && query.next());
        REQUIRE(query.value(0).toByteArray() == user->uuid.toRfc4122());
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("recovery-attribution"));

    REQUIRE(!db.redeemPasswordRecovery(second->token, old_password, &error));
    REQUIRE(!db.authenticateSession(first_login->bearer_token, &error));
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), old_password, 3600, &error));
    REQUIRE(db.loginWithPassword(QStringLiteral("engineer"), new_password, 3600, &error).has_value());
    REQUIRE(db.verifySecurityAudit(root).valid);

    const std::optional<aowis::db::IssuedRecoveryToken> third =
        db.issuePasswordRecovery(root, user->uuid, 900, &error);
    REQUIRE(third.has_value());
    REQUIRE(db.setUserPassword(root, user->uuid, old_password, &error));
    REQUIRE(!db.redeemPasswordRecovery(third->token, new_password, &error));

    const std::optional<aowis::db::IssuedRecoveryToken> fourth =
        db.issuePasswordRecovery(root, user->uuid, 900, &error);
    REQUIRE(fourth.has_value());
    REQUIRE(db.disableUser(root, user->uuid, &error));
    REQUIRE(!db.redeemPasswordRecovery(fourth->token, new_password, &error));
    REQUIRE(db.enableUser(root, user->uuid, &error));
    REQUIRE(!db.redeemPasswordRecovery(fourth->token, new_password, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);
    REQUIRE(!db.redeemPasswordRecovery(second->token, old_password, &error));

    return 0;
}
