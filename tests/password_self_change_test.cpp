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
    QCoreApplication application(argc, argv);
    QString error;
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QString filename = temporary.filePath(QStringLiteral("password-change.sqlite"));
    Database database;
    REQUIRE(database.openSqlite(filename, &error));
    const std::optional<aowis::db::UserInfo> administrator =
        database.bootstrapAdministrator(QStringLiteral("administrator"), &error);
    REQUIRE(administrator.has_value());
    const OperationContext admin{administrator->uuid};
    const std::optional<aowis::db::UserInfo> user =
        database.createUser(admin, QStringLiteral("engineer"), &error);
    REQUIRE(user.has_value());
    const std::optional<aowis::db::UserInfo> another_user =
        database.createUser(admin, QStringLiteral("second-engineer"), &error);
    REQUIRE(another_user.has_value());
    const QByteArray initial = QByteArrayLiteral("initial-password-1234");
    const QByteArray replacement = QByteArrayLiteral("a-new-and-longer-password-5678");
    REQUIRE(database.setUserPassword(admin, user->uuid, initial, &error));
    REQUIRE(database.setUserPassword(admin, another_user->uuid, initial, &error));
    const std::optional<aowis::db::IssuedSession> first =
        database.loginWithPassword(QStringLiteral("engineer"), initial, 3600, &error);
    const std::optional<aowis::db::IssuedSession> second =
        database.loginWithPassword(QStringLiteral("engineer"), initial, 3600, &error);
    const std::optional<aowis::db::IssuedSession> other =
        database.loginWithPassword(QStringLiteral("second-engineer"), initial, 3600, &error);
    REQUIRE(first.has_value() && second.has_value() && other.has_value());

    REQUIRE(!database.changeOwnPassword(QByteArrayLiteral("not-a-bearer"), initial, replacement, &error));
    REQUIRE(!database.changeOwnPassword(first->bearer_token, initial, QByteArrayLiteral("short"), &error));
    REQUIRE(!database.changeOwnPassword(first->bearer_token, initial, initial, &error));
    // An authenticated client cannot submit unbounded KDF input. Rejection
    // must leave existing credentials and sessions untouched.
    REQUIRE(!database.changeOwnPassword(first->bearer_token, QByteArray(1025, 'x'), replacement, &error));
    REQUIRE(error == QStringLiteral("Current password incorrect or temporarily unavailable"));
    REQUIRE(!database.changeOwnPassword(first->bearer_token, QByteArray(), replacement, &error));
    REQUIRE(error == QStringLiteral("Current password incorrect or temporarily unavailable"));
    REQUIRE(!database.changeOwnPassword(first->bearer_token, QByteArrayLiteral("wrong-password"), replacement, &error));
    REQUIRE(database.authenticateSession(first->bearer_token, &error).has_value());
    REQUIRE(database.authenticateSession(second->bearer_token, &error).has_value());
    REQUIRE(database.authenticateSession(other->bearer_token, &error).has_value());

    // If the security audit cannot be written, not even session revocations
    // or credential changes may commit.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("change-audit-block"));
        inspector.setDatabaseName(filename);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_own_change BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='credential.change' BEGIN SELECT RAISE(ABORT,'audit blocked'); END")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("change-audit-block"));
    REQUIRE(!database.changeOwnPassword(first->bearer_token, initial, replacement, &error));
    REQUIRE(database.authenticateSession(first->bearer_token, &error).has_value());
    REQUIRE(database.authenticateSession(second->bearer_token, &error).has_value());
    REQUIRE(!database.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error));
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("change-audit-unblock"));
        inspector.setDatabaseName(filename);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_own_change")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("change-audit-unblock"));

    REQUIRE(database.changeOwnPassword(first->bearer_token, initial, replacement, &error));
    REQUIRE(!database.authenticateSession(first->bearer_token, &error));
    REQUIRE(!database.authenticateSession(second->bearer_token, &error));
    REQUIRE(database.authenticateSession(other->bearer_token, &error).has_value());
    REQUIRE(!database.changeOwnPassword(first->bearer_token, replacement, initial, &error));
    REQUIRE(!database.loginWithPassword(QStringLiteral("engineer"), initial, 3600, &error));
    const std::optional<aowis::db::IssuedSession> renewed =
        database.loginWithPassword(QStringLiteral("engineer"), replacement, 3600, &error);
    REQUIRE(renewed.has_value());
    database.close();
    REQUIRE(database.openSqlite(filename, &error));
    REQUIRE(database.authenticateSession(renewed->bearer_token, &error).has_value());
    const QVector<aowis::db::SecurityAuditEvent> events = database.securityAudit(admin, 0, 200, &error);
    int changes = 0;
    for (const aowis::db::SecurityAuditEvent &event : events)
    {
        if (event.action == QStringLiteral("credential.change"))
        {
            ++changes;
            REQUIRE(event.actor_uuid == user->uuid && event.subject_uuid == user->uuid);
        }
    }
    REQUIRE(changes == 1);
    REQUIRE(database.verifySecurityAudit(admin).valid);
    return 0;
}
