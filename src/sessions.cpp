#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <limits>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &value)
{
    if (error)
        *error = value;
}

QByteArray uuidBytes(const QUuid &id)
{
    return id.toRfc4122();
}

QByteArray sessionDigest(const QByteArray &random_bytes)
{
    QByteArray data("AOWIS-SESSION-V1\0", 17);
    data.append(random_bytes);
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray makeRandomToken()
{
    QByteArray random_bytes(32, '\0');
    QRandomGenerator *generator = QRandomGenerator::system();
    for (int i = 0; i < 32; i += 4)
    {
        const quint32 number = generator->generate();
        for (int byte = 0; byte < 4; ++byte)
            random_bytes[i + byte] = static_cast<char>((number >> (8 * byte)) & 0xff);
    }
    return random_bytes;
}

std::optional<QByteArray> parseToken(const QByteArray &value)
{
    if (value.size() != 64)
        return std::nullopt;
    for (const char c : value)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return std::nullopt;
    }
    return QByteArray::fromHex(value);
}
}

bool Database::migrateSessionsV8(QString *error)
{
    if (!beginWrite(error))
        return false;
    const char *const statements[] = {
        R"SQL(CREATE TABLE user_sessions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            token_sha256 BLOB NOT NULL UNIQUE CHECK(length(token_sha256)=32),
            created_by_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            created_at_ms INTEGER NOT NULL,
            expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>created_at_ms),
            revoked_at_ms INTEGER,
            CHECK(revoked_at_ms IS NULL OR revoked_at_ms>=created_at_ms)
        ))SQL",
        R"SQL(CREATE INDEX user_sessions_by_user ON user_sessions(user_uuid))SQL",
        R"SQL(CREATE TRIGGER user_sessions_immutable_delete BEFORE DELETE ON user_sessions
            BEGIN SELECT RAISE(ABORT,'sessions are append-only'); END)SQL",
        R"SQL(CREATE TRIGGER user_sessions_immutable_identity BEFORE UPDATE ON user_sessions
            WHEN NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
                OR NEW.token_sha256!=OLD.token_sha256 OR NEW.created_by_uuid!=OLD.created_by_uuid
                OR NEW.created_at_ms!=OLD.created_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
                OR OLD.revoked_at_ms IS NOT NULL OR NEW.revoked_at_ms IS NULL
            BEGIN SELECT RAISE(ABORT,'session identity is immutable; revoke only once'); END)SQL"
    };
    for (const char *statement : statements)
    {
        QSqlQuery query(this->database_);
        if (!query.exec(QString::fromUtf8(statement)))
        {
            setError(error, query.lastError().text());
            rollback();
            return false;
        }
    }
    if (!exec(QStringLiteral("PRAGMA user_version = 8"), error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<IssuedSession> Database::issueSession(const OperationContext &admin,
    const QUuid &user_uuid, qint64 lifetime_seconds, QString *error)
{
    if (!requirePermission(admin, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    // No unbounded, zero-duration or negative sessions. 30 days maximum.
    if (user_uuid.isNull() || lifetime_seconds < 1 || lifetime_seconds > 30 * 24 * 60 * 60)
    {
        setError(error, QStringLiteral("Invalid target user or session lifetime (1..2592000 seconds)"));
        return std::nullopt;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    IssuedSession result;
    result.uuid = createUuidV7();
    result.user_uuid = user_uuid;
    result.bearer_token = makeRandomToken().toHex();
    result.expires_at_ms = now + lifetime_seconds * 1000;

    // Session issuance is committed atomically with its audit event. The audit
    // stores the session UUID, never the bearer token or its hash.
    const bool saved = auditedSecurityMutation(admin.actor_uuid, QStringLiteral("session.issue"),
        result.uuid, [&]() -> bool {
        QSqlQuery query(this->database_);
        if (!query.prepare(QStringLiteral(
            "INSERT INTO user_sessions(uuid,user_uuid,token_sha256,created_by_uuid,created_at_ms,expires_at_ms) "
            "SELECT ?,u.uuid,?,?,?,? FROM users u WHERE u.uuid=? AND u.disabled_at_ms IS NULL")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(uuidBytes(result.uuid));
        query.addBindValue(sessionDigest(QByteArray::fromHex(result.bearer_token)));
        query.addBindValue(uuidBytes(admin.actor_uuid));
        query.addBindValue(now);
        query.addBindValue(result.expires_at_ms);
        query.addBindValue(uuidBytes(user_uuid));
        if (!query.exec() || query.numRowsAffected() != 1)
        {
            setError(error, QStringLiteral("Target user is unknown or disabled: %1").arg(query.lastError().text()));
            return false;
        }
        return true;
    }, error);
    if (!saved)
        return std::nullopt;
    return result;
}

std::optional<OperationContext> Database::authenticateSession(const QByteArray &bearer_token,
    QString *error) const
{
    const std::optional<QByteArray> bytes = parseToken(bearer_token);
    if (!bytes)
    {
        setError(error, QStringLiteral("Invalid or expired session"));
        return std::nullopt;
    }
    QSqlQuery query(this->database_);
    if (!query.prepare(QStringLiteral(
        "SELECT s.user_uuid FROM user_sessions s JOIN users u ON u.uuid=s.user_uuid "
        "WHERE s.token_sha256=? AND s.revoked_at_ms IS NULL "
        "AND s.expires_at_ms>? AND u.disabled_at_ms IS NULL LIMIT 1")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(sessionDigest(*bytes));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    if (!query.next())
    {
        setError(error, QStringLiteral("Invalid or expired session"));
        return std::nullopt;
    }
    return OperationContext{QUuid::fromRfc4122(query.value(0).toByteArray())};
}

bool Database::revokeSession(const OperationContext &admin, const QUuid &session_uuid,
    QString *error)
{
    if (!requirePermission(admin, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    return auditedSecurityMutation(admin.actor_uuid, QStringLiteral("session.revoke"),
        session_uuid, [&]() -> bool {
        QSqlQuery query(this->database_);
        if (!query.prepare(QStringLiteral(
            "UPDATE user_sessions SET revoked_at_ms=? WHERE uuid=? AND revoked_at_ms IS NULL")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        query.addBindValue(uuidBytes(session_uuid));
        if (!query.exec() || query.numRowsAffected() != 1)
        {
            setError(error, QStringLiteral("Session not found or already revoked"));
            return false;
        }
        return true;
    }, error);
}

bool Database::disableUser(const OperationContext &admin, const QUuid &user_uuid,
    QString *error)
{
    if (!requirePermission(admin, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    return auditedSecurityMutation(admin.actor_uuid, QStringLiteral("user.disable"),
        user_uuid, [&]() -> bool {
        QSqlQuery query(this->database_);
        if (!query.prepare(QStringLiteral(
            "UPDATE users SET disabled_at_ms=? WHERE uuid=? AND disabled_at_ms IS NULL")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        query.addBindValue(uuidBytes(user_uuid));
        if (!query.exec() || query.numRowsAffected() != 1)
        {
            setError(error, QStringLiteral("User not found or already disabled"));
            return false;
        }
        // Never revive pre-disable bearer tokens when this account is later
        // reactivated. Session invalidation is atomic with the disable event.
        if (!query.prepare(QStringLiteral(
                "UPDATE user_sessions SET revoked_at_ms=? "
                "WHERE user_uuid=? AND revoked_at_ms IS NULL")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        query.addBindValue(uuidBytes(user_uuid));
        if (!query.exec())
        {
            setError(error, query.lastError().text());
            return false;
        }
        return true;
    }, error);
}

} // namespace aowis::db
