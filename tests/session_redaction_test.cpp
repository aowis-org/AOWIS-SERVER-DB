#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

namespace
{
QByteArray digestFor(const QString &file, const QUuid &session_uuid, const QString &connection_name)
{
    QByteArray value;
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        inspect.setDatabaseName(file);
        if (inspect.open())
        {
            QSqlQuery query(inspect);
            if (query.prepare(QStringLiteral("SELECT token_sha256 FROM user_sessions WHERE uuid=?")))
            {
                query.addBindValue(session_uuid.toRfc4122());
                if (query.exec() && query.next())
                    value = query.value(0).toByteArray();
            }
            query.finish();
            inspect.close();
        }
    }
    QSqlDatabase::removeDatabase(connection_name);
    return value;
}

QByteArray markerFor(const QUuid &session_uuid)
{
    return QByteArrayLiteral("AOWIS-REDACT-V1!") + session_uuid.toRfc4122();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("session-redaction.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(file, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> alice = db.createUser(root, QStringLiteral("alice"), &error);
    REQUIRE(alice.has_value());
    const OperationContext unprivileged{alice->uuid};
    const std::optional<aowis::db::IssuedSession> active = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> revoked = db.issueSession(root, alice->uuid, 3600, &error);
    REQUIRE(active.has_value() && revoked.has_value());
    REQUIRE(db.revokeSession(root, revoked->uuid, &error));
    const QByteArray original_digest = digestFor(file, revoked->uuid, QStringLiteral("initial-digest"));
    REQUIRE(original_digest.size() == 32 && original_digest != markerFor(revoked->uuid));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Unauthorized actors cannot erase session credential hashes.
    error.clear();
    REQUIRE(!db.redactSpentSessionTokens(unprivileged, now, 20, &error).has_value());
    REQUIRE(error.contains(QStringLiteral("Permission denied")));
    REQUIRE(digestFor(file, revoked->uuid, QStringLiteral("no-privilege")) == original_digest);
    error.clear();
    REQUIRE(!db.redactSpentSessionTokens(root, now + 86400000LL, 20, &error).has_value());
    REQUIRE(!db.redactSpentSessionTokens(root, now, 0, &error).has_value());
    REQUIRE(!db.redactSpentSessionTokens(root, now, 501, &error).has_value());

    // If an audit insertion fails, neither credential digest nor history changes.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("redact-audit-reject"));
        inspect.setDatabaseName(file);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_session_redaction BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='session.token.redact' BEGIN SELECT RAISE(ABORT,'reject redaction audit'); END")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("redact-audit-reject"));
    const qint64 count_before = db.verifySecurityAudit(root).checked_events;
    error.clear();
    REQUIRE(!db.redactSpentSessionTokens(root, QDateTime::currentMSecsSinceEpoch(), 20, &error).has_value());
    REQUIRE(db.verifySecurityAudit(root).checked_events == count_before);
    REQUIRE(digestFor(file, revoked->uuid, QStringLiteral("rollback-digest")) == original_digest);
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("redact-audit-restore"));
        inspect.setDatabaseName(file);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_session_redaction")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("redact-audit-restore"));

    error.clear();
    REQUIRE(db.redactSpentSessionTokens(root, QDateTime::currentMSecsSinceEpoch(), 1, &error) == 1);
    REQUIRE(digestFor(file, revoked->uuid, QStringLiteral("redacted-digest")) == markerFor(revoked->uuid));
    REQUIRE(db.activeSession(active->bearer_token).has_value());
    REQUIRE(digestFor(file, active->uuid, QStringLiteral("active-digest")) != markerFor(active->uuid));
    REQUIRE(db.verifySecurityAudit(root).valid);
    REQUIRE(db.verifySecurityAudit(root).checked_events == count_before + 1);
    const QVector<aowis::db::SecurityAuditEvent> audit = db.securityAudit(root, count_before, 10, &error);
    REQUIRE(audit.size() == 1 && audit.first().actor_uuid == root.actor_uuid);
    REQUIRE(audit.first().action == QStringLiteral("session.token.redact"));
    REQUIRE(audit.first().subject_uuid == revoked->uuid);
    REQUIRE(db.redactSpentSessionTokens(root, QDateTime::currentMSecsSinceEpoch(), 20, &error) == 0);

    // An ordinary SQL client cannot forge a redacted/active session digest
    // or restore a digest after a one-way redaction.
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("redact-trigger-check"));
        inspect.setDatabaseName(file);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.prepare(QStringLiteral("UPDATE user_sessions SET token_sha256=? WHERE uuid=?")));
        query.addBindValue(markerFor(active->uuid));
        query.addBindValue(active->uuid.toRfc4122());
        REQUIRE(!query.exec());
        REQUIRE(query.prepare(QStringLiteral("UPDATE user_sessions SET token_sha256=? WHERE uuid=?")));
        query.addBindValue(original_digest);
        query.addBindValue(revoked->uuid.toRfc4122());
        REQUIRE(!query.exec());
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("redact-trigger-check"));

    // A naturally expired session can be scrubbed without a revocation event.
    const std::optional<aowis::db::IssuedSession> short_lived = db.issueSession(root, alice->uuid, 1, &error);
    REQUIRE(short_lived.has_value());
    QThread::msleep(1150);
    REQUIRE(!db.activeSession(short_lived->bearer_token).has_value());
    REQUIRE(db.redactSpentSessionTokens(root, QDateTime::currentMSecsSinceEpoch(), 20, &error) == 1);
    REQUIRE(digestFor(file, short_lived->uuid, QStringLiteral("expired-digest")) == markerFor(short_lived->uuid));
    REQUIRE(db.activeSession(active->bearer_token).has_value());
    REQUIRE(db.verifySecurityAudit(root).valid);

    // V12 -> V13 migration retains session metadata and seals the same audit
    // chain. Simulate the exact predecessor trigger and declared schema.
    const QByteArray existing_active_digest = digestFor(file, active->uuid, QStringLiteral("before-migration"));
    db.close();
    {
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("redact-v12-fixture"));
        inspect.setDatabaseName(file);
        REQUIRE(inspect.open());
        QSqlQuery query(inspect);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER user_sessions_immutable_identity_v13")));
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER user_sessions_immutable_identity BEFORE UPDATE ON user_sessions "
            "WHEN NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid "
            "OR NEW.token_sha256!=OLD.token_sha256 OR NEW.created_by_uuid!=OLD.created_by_uuid "
            "OR NEW.created_at_ms!=OLD.created_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms "
            "OR OLD.revoked_at_ms IS NOT NULL OR NEW.revoked_at_ms IS NULL "
            "BEGIN SELECT RAISE(ABORT,'session identity is immutable; revoke only once'); END")));
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version=12")));
        query.finish();
        inspect.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("redact-v12-fixture"));
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(digestFor(file, active->uuid, QStringLiteral("after-migration")) == existing_active_digest);
    REQUIRE(db.activeSession(active->bearer_token).has_value());
    REQUIRE(db.verifySecurityAudit(root).valid);
    return 0;
}
