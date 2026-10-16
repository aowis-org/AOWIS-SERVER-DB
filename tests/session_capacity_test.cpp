#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDebug>
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
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database db;
    const QString path = directory.filePath(QStringLiteral("session-quota.sqlite"));
    REQUIRE(db.openSqlite(path, &error));

    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> alice = db.createUser(root, QStringLiteral("alice"), &error);
    const std::optional<aowis::db::UserInfo> bob = db.createUser(root, QStringLiteral("bob"), &error);
    REQUIRE(alice && bob);
    const QByteArray password = QByteArrayLiteral("long-password-for-testing");
    REQUIRE(db.setUserPassword(root, alice->uuid, password, &error));

    QVector<aowis::db::IssuedSession> sessions;
    for (int i = 0; i < 32; ++i)
    {
        const std::optional<aowis::db::IssuedSession> session = db.issueSession(root, alice->uuid, 3600, &error);
        REQUIRE(session.has_value());
        sessions.append(*session);
    }

    const qint64 before = db.verifySecurityAudit(root).checked_events;
    error.clear();
    REQUIRE(!db.issueSession(root, alice->uuid, 3600, &error));
    REQUIRE(error.contains(QStringLiteral("Active session limit")));
    REQUIRE(db.verifySecurityAudit(root).checked_events == before);
    REQUIRE(db.activeSession(sessions.at(0).bearer_token).has_value());

    // An independent account's quota is unaffected.
    REQUIRE(db.issueSession(root, bob->uuid, 3600, &error).has_value());

    // Password login uses the same quota. Valid credentials must not silently
    // create an extra session, reset the lockout counter, or add an audit event.
    const qint64 before_login = db.verifySecurityAudit(root).checked_events;
    error.clear();
    REQUIRE(!db.loginWithPassword(QStringLiteral("alice"), password, 3600, &error));
    REQUIRE(error == QStringLiteral("Invalid credentials or temporarily unavailable"));
    REQUIRE(db.verifySecurityAudit(root).checked_events == before_login);

    // The restriction survives database reopen and leaves all existing
    // sessions intact until they are revoked or expire.
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(!db.issueSession(root, alice->uuid, 3600, &error));
    REQUIRE(db.revokeSession(root, sessions.at(0).uuid, &error));
    REQUIRE(!db.activeSession(sessions.at(0).bearer_token));
    const std::optional<aowis::db::IssuedSession> replacement =
        db.loginWithPassword(QStringLiteral("alice"), password, 3600, &error);
    REQUIRE(replacement.has_value());
    REQUIRE(db.activeSession(replacement->bearer_token).has_value());
    REQUIRE(!db.issueSession(root, alice->uuid, 3600, &error));
    REQUIRE(db.verifySecurityAudit(root).valid);
    return 0;
}
