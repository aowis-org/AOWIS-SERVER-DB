#include <aowis/db/database.h>
#include "storage_backend.h"
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

// Stable 32-byte marker already accepted by both backends' session guards.
// Keep its bytes unchanged: previously redacted records must remain recognizable.
QByteArray redactedSessionDigest(const QUuid &session_uuid)
{
    return QByteArrayLiteral("AOWIS-REDACT-V1!") + session_uuid.toRfc4122();
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

bool hasSessionCapacity(const QSqlDatabase &connection, const StorageBackend &backend,
                        const QUuid &user_uuid, qint64 at_ms, QString *error)
{
    QSqlQuery query(connection);
    if (!query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM user_sessions WHERE user_uuid=? "
            "AND revoked_at_ms IS NULL AND expires_at_ms>?")))
    {
        setError(error, query.lastError().text());
        return false;
    }
    query.addBindValue(backend.bindUuid(user_uuid));
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

// A spent session's digest is overwritten with an unmistakably non-secret
// marker, unique to that session UUID. The 16-byte prefix is ASCII
// "AOWIS-REDACT-V1!" followed by the session's 16-byte RFC 4122 UUID.
// Both SQLite and PostgreSQL allow changing a spent digest only to this marker.
// No token hash or bearer token is kept in the security audit record.
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
    // Portable across SQLite BLOBs and PostgreSQL BYTEA/UUID. A spent
    // credential is selected only if it is not already the stable redaction
    // marker for its own UUID. Don't impose SQL LIMIT before filtering:
    // already-redacted earlier sessions must not consume the batch limit.
    QSqlQuery select(this->database_);
    if (!select.prepare(QStringLiteral(R"SQL(
        SELECT uuid, token_sha256 FROM user_sessions
        WHERE (expires_at_ms<=? OR (revoked_at_ms IS NOT NULL AND revoked_at_ms<=?))
          AND (revoked_at_ms IS NOT NULL OR expires_at_ms<=?)
        ORDER BY expires_at_ms,uuid
    )SQL")))
    {
        setError(error, select.lastError().text());
        rollback();
        return std::nullopt;
    }
    select.addBindValue(cutoff_ms);
    select.addBindValue(cutoff_ms);
    select.addBindValue(now);
    if (!select.exec())
    {
        setError(error, select.lastError().text());
        rollback();
        return std::nullopt;
    }
    QVector<QPair<QUuid, QByteArray>> candidates;
    while (candidates.size() < max_count && select.next())
    {
        const QUuid id = this->backend_->readUuid(select.value(0));
        const QByteArray previous_digest = select.value(1).toByteArray();
        if (previous_digest != redactedSessionDigest(id))
            candidates.append(qMakePair(id, previous_digest));
    }
    select.finish();

    QSqlQuery update(this->database_);
    if (!update.prepare(QStringLiteral(
            "UPDATE user_sessions SET token_sha256=? "
            "WHERE uuid=? AND token_sha256=?")))
    {
        setError(error, update.lastError().text());
        rollback();
        return std::nullopt;
    }
    for (const QPair<QUuid, QByteArray> &candidate : candidates)
    {
        update.bindValue(0, redactedSessionDigest(candidate.first));
        update.bindValue(1, this->backend_->bindUuid(candidate.first));
        update.bindValue(2, candidate.second);
        if (!update.exec() || update.numRowsAffected() != 1)
        {
            setError(error, QStringLiteral("Unable to redact spent session credential: %1")
                              .arg(update.lastError().text()));
            rollback();
            return std::nullopt;
        }
        update.finish();
        if (!writeSecurityAuditEvent(admin.actor_uuid, QStringLiteral("session.token.redact"),
                                     candidate.first, error))
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
    return candidates.size();
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
        if (!hasSessionCapacity(this->database_, *this->backend_, user_uuid, now, error))
            return false;
        QSqlQuery query(this->database_);
        if (!query.prepare(QStringLiteral(
            "INSERT INTO user_sessions(uuid,user_uuid,token_sha256,created_by_uuid,created_at_ms,expires_at_ms) "
            "SELECT ?,u.uuid,?,?,?,? FROM users u WHERE u.uuid=? AND u.disabled_at_ms IS NULL")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(this->backend_->bindUuid(result.uuid));
        query.addBindValue(sessionDigest(QByteArray::fromHex(result.bearer_token)));
        query.addBindValue(this->backend_->bindUuid(admin.actor_uuid));
        query.addBindValue(now);
        query.addBindValue(result.expires_at_ms);
        query.addBindValue(this->backend_->bindUuid(user_uuid));
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
    return OperationContext{this->backend_->readUuid(query.value(0))};
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
    return ActiveSession{this->backend_->readUuid(query.value(0)),
                         this->backend_->readUuid(query.value(1)),
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
    query.addBindValue(this->backend_->bindUuid(session->uuid));
    query.addBindValue(this->backend_->bindUuid(session->user_uuid));
    query.addBindValue(sessionDigest(*raw));
    query.addBindValue(now);
    query.addBindValue(this->backend_->bindUuid(session->user_uuid));
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
    query.addBindValue(this->backend_->bindUuid(current->user_uuid));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(after_uuid.isNull() ? 1 : 0);
    query.addBindValue(this->backend_->bindUuid(after_uuid));
    query.addBindValue(limit);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return result;
    }
    while (query.next())
    {
        const QUuid id = this->backend_->readUuid(query.value(0));
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
    query.addBindValue(this->backend_->bindUuid(session_uuid));
    query.addBindValue(this->backend_->bindUuid(current->user_uuid));
    query.addBindValue(now);
    query.addBindValue(this->backend_->bindUuid(current->user_uuid));
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
    query.addBindValue(this->backend_->bindUuid(current->user_uuid));
    query.addBindValue(this->backend_->bindUuid(current->uuid));
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
        query.addBindValue(this->backend_->bindUuid(session_uuid));
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
        query.addBindValue(this->backend_->bindUuid(user_uuid));
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
        query.addBindValue(this->backend_->bindUuid(user_uuid));
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
        query.addBindValue(this->backend_->bindUuid(user_uuid));
        if (!query.exec())
        {
            setError(error, query.lastError().text());
            return false;
        }
        return true;
    }, error);
}

} // namespace aowis::db
