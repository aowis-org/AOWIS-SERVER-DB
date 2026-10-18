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
    const QString path = temp.filePath(QStringLiteral("credentials.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const auto admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const auto user = db.createUser(root, QStringLiteral("engineer"), &error);
    REQUIRE(user.has_value());
    const OperationContext guest{user->uuid};
    const QByteArray original = QByteArrayLiteral("secure waterworks passphrase 2026");
    const QByteArray replacement = QByteArrayLiteral("second sufficiently long passphrase");
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), original, 3600, &error));
    REQUIRE(!db.setUserPassword(guest, guest.actor_uuid, original, &error));
    REQUIRE(!db.setUserPassword(root, user->uuid, QByteArrayLiteral("short"), &error));
    REQUIRE(!db.setUserPassword(root, user->uuid, QByteArray(1200, 'x'), &error));
    REQUIRE(db.setUserPassword(root, user->uuid, original, &error));

    // The SQLite file contains only a salt/verifier, not the actual password.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("credential-inspect"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        query.finish();
        REQUIRE(query.exec(QStringLiteral("SELECT salt,verifier,iterations FROM user_password_credentials")));
        REQUIRE(query.next());
        REQUIRE(query.value(0).toByteArray().size() == 16);
        REQUIRE(query.value(1).toByteArray().size() == 32);
        REQUIRE(query.value(1).toByteArray() != original);
        REQUIRE(query.value(2).toInt() >= 600000);
        query.finish();
        REQUIRE(query.exec(QStringLiteral(
            "SELECT user_uuid,changed_by_uuid FROM user_password_credentials")));
        REQUIRE(query.next());
        REQUIRE(query.value(0).toByteArray() == user->uuid.toRfc4122());
        REQUIRE(query.value(1).toByteArray() == admin->uuid.toRfc4122());
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("credential-inspect"));

    REQUIRE(!db.loginWithPassword(QStringLiteral("unknown"), original, 3600, &error));
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), QByteArrayLiteral("wrong"), 3600, &error));
    const auto first = db.loginWithPassword(QStringLiteral("ENGINEER"), original, 3600, &error);
    REQUIRE(first.has_value());
    REQUIRE(first->user_uuid == user->uuid);
    const std::optional<aowis::db::ActiveSession> active =
        db.activeSession(first->bearer_token, &error);
    REQUIRE(active.has_value());
    REQUIRE(active->uuid == first->uuid && active->user_uuid == user->uuid);
    REQUIRE(db.authenticateSession(first->bearer_token, &error).has_value());
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), original, 0, &error));
    REQUIRE(db.setUserPassword(root, user->uuid, replacement, &error));
    REQUIRE(!db.authenticateSession(first->bearer_token, &error));
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), original, 3600, &error));
    const auto second = db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error);
    REQUIRE(second.has_value() && second->bearer_token != first->bearer_token);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.authenticateSession(second->bearer_token, &error).has_value());

    // Five failed logins impose a temporary lockout even for a correct password.
    for (int index = 0; index < 5; ++index)
        REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), QByteArrayLiteral("wrong"), 3600, &error));
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error));
    REQUIRE(db.setUserPassword(root, user->uuid, replacement, &error));
    REQUIRE(db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error));
    REQUIRE(db.disableUser(root, user->uuid, &error));
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error));
    REQUIRE(db.enableUser(root, user->uuid, &error));
    REQUIRE(db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error));

    const auto events = db.securityAudit(root, 0, 200, &error);
    int credential_events = 0;
    int login_events = 0;
    for (const auto &event : events)
    {
        if (event.action == QStringLiteral("credential.set"))
            ++credential_events;
        if (event.action == QStringLiteral("session.password_login"))
            ++login_events;
    }
    REQUIRE(credential_events == 3);
    REQUIRE(login_events >= 3);
    REQUIRE(db.verifySecurityAudit(root).valid);

    // A failed audit insertion must not alter a password or revoke sessions.
    const auto live = db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error);
    REQUIRE(live.has_value());
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("credential-rollback"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_credential_audit BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='credential.set' BEGIN SELECT RAISE(ABORT,'reject credential'); END")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("credential-rollback"));
    REQUIRE(!db.setUserPassword(root, user->uuid, original, &error));
    REQUIRE(db.authenticateSession(live->bearer_token, &error).has_value());
    REQUIRE(db.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error).has_value());
    REQUIRE(!db.loginWithPassword(QStringLiteral("engineer"), original, 3600, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);

    return 0;
}
