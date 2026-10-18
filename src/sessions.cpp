#include <aowis/db/database.h>
#include <aowis/model/uuid.h>
#include "session_token_internal.h"

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

QByteArray makeSessionToken()
{
    return makeRandomToken().toHex();
}

QByteArray sessionTokenDigest(const QByteArray &random_bytes)
{
    return sessionDigest(random_bytes);
}

bool hasSessionCapacity(const QSqlDatabase &connection, const QUuid &user_uuid,
                        qint64 at_ms, QString *error)
{
    QSqlQuery query(connection);
    if (!query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM user_sessions WHERE user_uuid=? "
            "AND revoked_at_ms IS NULL AND expires_at_ms>?")))
    {
        setError(error, query.lastError().text());
        return false;
    }
    query.addBindValue(uuidBytes(user_uuid));
    query.addBindValue(at_ms);
    if (!query.exec() || !query.next())
    {
        setError(error, QStringLiteral("Could not check active session limit: %1")
            .arg(query.lastError().text()));
        return false;
    }
    if (query.value(0).toLongLong() >= max_active_sessions_per_user)
    {
        setError(error, QStringLiteral("Active session limit reached (32 per user)"));
        return false;
    }
    return true;
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
    if (!setSchemaVersion(8, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

// A spent session's digest is overwritten with an unmistakably non-secret
// marker, unique to that session UUID. The 16-byte prefix is ASCII
// "AOWIS-REDACT-V1!" followed by the session's 16-byte RFC 4122 UUID.
// SQLite enforces that a digest may be changed only to precisely this marker.
// No token hash or bearer token is kept in the security audit record.
bool Database::migrateSessionRedactionV13(QString *error)
{
    if (!beginWrite(error))
        return false;
    if (!exec(QStringLiteral("DROP TRIGGER IF EXISTS user_sessions_immutable_identity"), error) ||
        !exec(QStringLiteral("DROP TRIGGER IF EXISTS user_sessions_immutable_identity_v13"), error) ||
        !exec(QStringLiteral(R"SQL(
            CREATE TRIGGER user_sessions_immutable_identity_v13 BEFORE UPDATE ON user_sessions
            WHEN NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
                OR NEW.created_by_uuid!=OLD.created_by_uuid
                OR NEW.created_at_ms!=OLD.created_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
                OR (OLD.revoked_at_ms IS NOT NULL AND NEW.revoked_at_ms IS NOT OLD.revoked_at_ms)
                OR (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
                    AND NEW.revoked_at_ms<OLD.created_at_ms)
                OR (NEW.token_sha256!=OLD.token_sha256 AND NOT (
                    NEW.token_sha256=CAST(x'414f5749532d5245444143542d563121' || OLD.uuid AS BLOB)
                    AND (OLD.revoked_at_ms IS NOT NULL OR OLD.expires_at_ms<=CAST((julianday('now')-2440587.5)*86400000 AS INTEGER))
                ))
            BEGIN SELECT RAISE(ABORT,'session identity is immutable; spent credentials may only be redacted'); END
        )SQL"), error) ||
        !setSchemaVersion(13, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<int> Database::redactSpentSessionTokens(const OperationContext &admin,
                                                       qint64 cutoff_ms, int max_count,
                                                       QString *error)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cutoff_ms <= 0 || cutoff_ms > now || max_count < 1 || max_count > 500)
    {
        setError(error, QStringLiteral("Invalid session-redaction cutoff or batch limit (1..500)"));
        return std::nullopt;
    }
    if (!beginWrite(error))
        return std::nullopt;
    if (!requirePermission(admin, QStringLiteral("security.manage"), AccessScope::system(), error))
    {
        rollback();
        return std::nullopt;
    }
    // Preserve full session metadata: only spent credential digests are
    // redacted. Sessions must be revoked or expired, and their last use date
    // must be at or before the selected retention cutoff.
    QSqlQuery select(this->database_);
    if (!select.prepare(QStringLiteral(R"SQL(
        SELECT uuid FROM user_sessions
        WHERE (expires_at_ms<=? OR (revoked_at_ms IS NOT NULL AND revoked_at_ms<=?))
          AND (revoked_at_ms IS NOT NULL OR expires_at_ms<=?)
          AND token_sha256!=CAST(x'414f5749532d5245444143542d563121' || uuid AS BLOB)
        ORDER BY expires_at_ms,uuid LIMIT ?
    )SQL")))
    {
        setError(error, select.lastError().text());
        rollback();
        return std::nullopt;
    }
    select.addBindValue(cutoff_ms);
    select.addBindValue(cutoff_ms);
    select.addBindValue(now);
    select.addBindValue(max_count);
    if (!select.exec())
    {
        setError(error, select.lastError().text());
        rollback();
        return std::nullopt;
    }
    QVector<QUuid> session_ids;
    while (select.next())
        session_ids.append(QUuid::fromRfc4122(select.value(0).toByteArray()));
    select.finish();

    QSqlQuery update(this->database_);
    if (!update.prepare(QStringLiteral(
            "UPDATE user_sessions SET token_sha256="
            "CAST(x'414f5749532d5245444143542d563121' || uuid AS BLOB) "
            "WHERE uuid=? AND token_sha256!="
            "CAST(x'414f5749532d5245444143542d563121' || uuid AS BLOB)")))
    {
        setError(error, update.lastError().text());
        rollback();
        return std::nullopt;
    }
    for (const QUuid &id : session_ids)
    {
        update.bindValue(0, uuidBytes(id));
        if (!update.exec() || update.numRowsAffected() != 1)
        {
            setError(error, QStringLiteral("Unable to redact spent session credential: %1")
                              .arg(update.lastError().text()));
            rollback();
            return std::nullopt;
        }
        update.finish();
        if (!writeSecurityAuditEvent(admin.actor_uuid, QStringLiteral("session.token.redact"),
                                     id, error))
        {
            rollback();
            return std::nullopt;
        }
    }
    update.finish();
    if (!commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return session_ids.size();
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
        if (!hasSessionCapacity(this->database_, user_uuid, now, error))
            return false;
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

std::optional<ActiveSession> Database::activeSession(const QByteArray &bearer_token,
    QString *error) const
{
    const std::optional<QByteArray> raw = parseToken(bearer_token);
    if (!raw)
    {
        setError(error, QStringLiteral("Invalid or expired session"));
        return std::nullopt;
    }
    QSqlQuery query(this->database_);
    if (!query.prepare(QStringLiteral(
        "SELECT s.uuid,s.user_uuid,s.expires_at_ms FROM user_sessions s "
        "JOIN users u ON u.uuid=s.user_uuid WHERE s.token_sha256=? "
        "AND s.revoked_at_ms IS NULL AND s.expires_at_ms>? "
        "AND u.disabled_at_ms IS NULL LIMIT 1")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(sessionDigest(*raw));
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
    return ActiveSession{QUuid::fromRfc4122(query.value(0).toByteArray()),
                         QUuid::fromRfc4122(query.value(1).toByteArray()),
                         query.value(2).toLongLong()};
}

bool Database::logoutSession(const QByteArray &bearer_token, QString *error)
{
    const std::optional<ActiveSession> session = activeSession(bearer_token, error);
    const std::optional<QByteArray> raw = parseToken(bearer_token);
    if (!session || !raw)
        return false;
    if (!beginWrite(error))
        return false;

    // The token and its owner are revalidated under BEGIN IMMEDIATE. The
    // revocation and audit record must commit or roll back together.
    QSqlQuery query(this->database_);
    if (!query.prepare(QStringLiteral(
        "UPDATE user_sessions SET revoked_at_ms=? WHERE uuid=? AND user_uuid=? "
        "AND token_sha256=? AND revoked_at_ms IS NULL AND expires_at_ms>? "
        "AND EXISTS (SELECT 1 FROM users WHERE uuid=? AND disabled_at_ms IS NULL)")))
    {
        setError(error, query.lastError().text());
        rollback();
        return false;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.addBindValue(now);
    query.addBindValue(uuidBytes(session->uuid));
    query.addBindValue(uuidBytes(session->user_uuid));
    query.addBindValue(sessionDigest(*raw));
    query.addBindValue(now);
    query.addBindValue(uuidBytes(session->user_uuid));
    if (!query.exec() || query.numRowsAffected() != 1)
    {
        setError(error, QStringLiteral("Invalid or expired session"));
        rollback();
        return false;
    }
    query.finish();
    if (!writeSecurityAuditEvent(session->user_uuid, QStringLiteral("session.logout"),
                                 session->uuid, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}


QVector<OwnSessionInfo> Database::ownSessions(const QByteArray &bearer_token,
    const QUuid &after_uuid, int limit, QString *error) const
{
    QVector<OwnSessionInfo> result;
    if (limit < 1 || limit > 100)
    {
        setError(error, QStringLiteral("Session listing limit must be 1..100"));
        return result;
    }
    const std::optional<ActiveSession> current = activeSession(bearer_token, error);
    if (!current)
        return result;

    QSqlQuery query(this->database_);
    if (!query.prepare(QStringLiteral(
        "SELECT uuid,created_at_ms,expires_at_ms FROM user_sessions "
        "WHERE user_uuid=? AND revoked_at_ms IS NULL AND expires_at_ms>? "
        "AND (?=1 OR uuid>?) ORDER BY uuid LIMIT ?")))
    {
        setError(error, query.lastError().text());
        return result;
    }
    query.addBindValue(uuidBytes(current->user_uuid));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(after_uuid.isNull() ? 1 : 0);
    query.addBindValue(uuidBytes(after_uuid));
    query.addBindValue(limit);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return result;
    }
    while (query.next())
    {
        const QUuid id = QUuid::fromRfc4122(query.value(0).toByteArray());
        result.append(OwnSessionInfo{id, query.value(1).toLongLong(),
                                     query.value(2).toLongLong(), id == current->uuid});
    }
    return result;
}

bool Database::revokeOwnSession(const QByteArray &bearer_token, const QUuid &session_uuid,
    QString *error)
{
    if (session_uuid.isNull())
    {
        setError(error, QStringLiteral("Invalid session UUID"));
        return false;
    }
    // Resolve outside the lock, then independently revalidate under it.
    // A user UUID sent by an HTTP client is never accepted as authority.
    if (!activeSession(bearer_token, error))
        return false;
    if (!beginWrite(error))
        return false;
    const std::optional<ActiveSession> current = activeSession(bearer_token, error);
    if (!current)
    {
        rollback();
        return false;
    }
    QSqlQuery query(this->database_);
    if (!query.prepare(QStringLiteral(
        "UPDATE user_sessions SET revoked_at_ms=? WHERE uuid=? AND user_uuid=? "
        "AND revoked_at_ms IS NULL AND expires_at_ms>? "
        "AND EXISTS(SELECT 1 FROM users WHERE uuid=? AND disabled_at_ms IS NULL)")))
    {
        setError(error, query.lastError().text());
        rollback();
        return false;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.addBindValue(now);
    query.addBindValue(uuidBytes(session_uuid));
    query.addBindValue(uuidBytes(current->user_uuid));
    query.addBindValue(now);
    query.addBindValue(uuidBytes(current->user_uuid));
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        rollback();
        return false;
    }
    if (query.numRowsAffected() != 1)
    {
        setError(error, QStringLiteral("Session not found or unavailable"));
        rollback();
        return false;
    }
    query.finish();
    const QString action = session_uuid == current->uuid
        ? QStringLiteral("session.logout") : QStringLiteral("session.revoke.own");
    if (!writeSecurityAuditEvent(current->user_uuid, action, session_uuid, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<qint64> Database::revokeOtherSessions(const QByteArray &bearer_token,
    QString *error)
{
    const std::optional<QByteArray> raw = parseToken(bearer_token);
    if (!raw)
    {
        setError(error, QStringLiteral("Invalid or expired session"));
        return std::nullopt;
    }
    // Reject invalid credentials without taking the SQLite write lock.
    // Recheck after BEGIN IMMEDIATE to close the race with other writers.
    if (!activeSession(bearer_token, error))
        return std::nullopt;
    if (!beginWrite(error))
        return std::nullopt;
    const std::optional<ActiveSession> current = activeSession(bearer_token, error);
    if (!current)
    {
        rollback();
        return std::nullopt;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(this->database_);
    if (!query.prepare(QStringLiteral(
            "UPDATE user_sessions SET revoked_at_ms=? "
            "WHERE user_uuid=? AND uuid<>? AND revoked_at_ms IS NULL AND expires_at_ms>?")))
    {
        setError(error, query.lastError().text());
        rollback();
        return std::nullopt;
    }
    query.addBindValue(now);
    query.addBindValue(uuidBytes(current->user_uuid));
    query.addBindValue(uuidBytes(current->uuid));
    query.addBindValue(now);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        rollback();
        return std::nullopt;
    }
    const qint64 revoked_count = query.numRowsAffected();
    query.finish();
    if (!writeSecurityAuditEvent(current->user_uuid, QStringLiteral("session.revoke_others"),
                                 current->uuid, error) || !commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return revoked_count;
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
        query.finish();
        if (!query.prepare(QStringLiteral(
                "UPDATE password_recovery_tokens SET invalidated_at_ms=? "
                "WHERE user_uuid=? AND used_at_ms IS NULL AND invalidated_at_ms IS NULL")))
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
