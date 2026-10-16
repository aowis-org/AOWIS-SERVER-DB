#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

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
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sessions.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("operator"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> guest = db.createUser(root, QStringLiteral("engineer"), &error);
    REQUIRE(guest.has_value());
    REQUIRE(!db.issueSession({guest->uuid}, guest->uuid, 3600, &error));
    REQUIRE(!db.issueSession(root, guest->uuid, 0, &error));
    REQUIRE(!db.issueSession(root, guest->uuid, 31LL * 86400, &error));
    REQUIRE(!db.issueSession(root, createUuidV7(), 3600, &error));

    const std::optional<aowis::db::IssuedSession> issued = db.issueSession(root, guest->uuid, 3600, &error);
    REQUIRE(issued.has_value());
    REQUIRE(issued->bearer_token.size() == 64);
    REQUIRE(issued->uuid != guest->uuid);
    REQUIRE(db.authenticateSession(issued->bearer_token, &error)->actor_uuid == guest->uuid);
    REQUIRE(!db.authenticateSession(QByteArrayLiteral("not-a-valid-session"), &error));
    REQUIRE(!db.authenticateSession(issued->bearer_token.toUpper(), &error));
    QByteArray altered = issued->bearer_token;
    altered[0] = altered[0] == '0' ? '1' : '0';
    REQUIRE(!db.authenticateSession(altered, &error));
    REQUIRE(!db.revokeSession({guest->uuid}, issued->uuid, &error));

    // Inspect database directly: token SHA-256 only; never store plaintext.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("session-inspect"));
        inspect.setDatabaseName(path);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version")));
        REQUIRE(query.next() && query.value(0).toInt() == 10);
        REQUIRE(query.exec(QStringLiteral("SELECT token_sha256,uuid FROM user_sessions")));
        REQUIRE(query.next() && query.value(0).toByteArray().size() == 32);
        REQUIRE(query.value(0).toByteArray() != issued->bearer_token);
        REQUIRE(query.value(1).toByteArray() == issued->uuid.toRfc4122());
        query.finish();
        // Force expiry using a privileged direct-SQL user. This is strictly a
        // test; the application API does not change immutable expiration.
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER user_sessions_immutable_identity")));
        REQUIRE(query.prepare(QStringLiteral("UPDATE user_sessions SET expires_at_ms=created_at_ms+1 WHERE uuid=?")));
        query.addBindValue(issued->uuid.toRfc4122());
        REQUIRE(query.exec());
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("session-inspect"));
    REQUIRE(!db.authenticateSession(issued->bearer_token, &error));

    const std::optional<aowis::db::IssuedSession> second = db.issueSession(root, guest->uuid, 3600, &error);
    REQUIRE(second.has_value() && second->bearer_token != issued->bearer_token);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.authenticateSession(second->bearer_token, &error)->actor_uuid == guest->uuid);
    REQUIRE(db.revokeSession(root, second->uuid, &error));
    REQUIRE(!db.revokeSession(root, second->uuid, &error));
    REQUIRE(!db.authenticateSession(second->bearer_token, &error));

    const std::optional<aowis::db::IssuedSession> third = db.issueSession(root, guest->uuid, 3600, &error);
    REQUIRE(third.has_value());
    REQUIRE(db.disableUser(root, guest->uuid, &error));
    REQUIRE(!db.authenticateSession(third->bearer_token, &error));
    REQUIRE(!db.issueSession(root, guest->uuid, 3600, &error));
    REQUIRE(!db.disableUser(root, guest->uuid, &error));
    const auto project = db.createProject(root, QStringLiteral("Persistent data"), &error);
    REQUIRE(project.has_value());
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);

    // V7 -> V9: simulate a valid V7 installation with existing users and
    // projects, without session or security-audit tables. Both migrations
    // must preserve those users and grant audit.read to system admins.
    const QString legacy_path = directory.filePath(QStringLiteral("upgrade-v7.sqlite"));
    Database legacy;
    REQUIRE(legacy.openSqlite(legacy_path, &error));
    const auto old_admin = legacy.bootstrapAdministrator(QStringLiteral("legacy-admin"), &error);
    REQUIRE(old_admin.has_value());
    const auto old_project = legacy.createProject({old_admin->uuid}, QStringLiteral("Historical project"), &error);
    REQUIRE(old_project.has_value());
    legacy.close();
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                            QStringLiteral("downgrade-v7"));
        inspector.setDatabaseName(legacy_path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TABLE security_audit_events")));
        REQUIRE(query.exec(QStringLiteral("DROP TABLE user_sessions")));
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version=7")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("downgrade-v7"));
    REQUIRE(legacy.openSqlite(legacy_path, &error));
    REQUIRE(legacy.hasPermission({old_admin->uuid}, QStringLiteral("security.audit.read"),
        aowis::db::AccessScope::system(), &error));
    REQUIRE(legacy.project({old_admin->uuid}, old_project->project_uuid, &error).has_value());
    REQUIRE(legacy.issueSession({old_admin->uuid}, old_admin->uuid, 60, &error).has_value());
    return 0;
}
