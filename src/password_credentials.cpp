#include <aowis/db/database.h>
#include <aowis/model/uuid.h>
#include "session_token_internal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

namespace aowis::db
{
namespace
{
constexpr int password_iterations = 600000;
constexpr int password_salt_bytes = 16;
constexpr int password_hash_bytes = 32;
constexpr int password_failure_limit = 5;
constexpr qint64 password_lockout_ms = 60000;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool validPassword(const QByteArray &password)
{
    return password.size() >= 12 && password.size() <= 1024 && !password.contains('\0');
}

QByteArray createSalt()
{
    QByteArray salt(password_salt_bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), salt.size()) != 1)
        return {};
    return salt;
}

QByteArray derivePassword(const QByteArray &password, const QByteArray &salt, int iterations)
{
    if (salt.size() != password_salt_bytes || iterations < 100000 || iterations > 2000000)
        return {};
    QByteArray digest(password_hash_bytes, '\0');
    if (PKCS5_PBKDF2_HMAC(password.constData(), password.size(),
            reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
            iterations, EVP_sha256(), digest.size(),
            reinterpret_cast<unsigned char *>(digest.data())) != 1)
        return {};
    return digest;
}

// Invalid username/disabled user/no credential use the same expensive KDF
// path as an incorrect password. Do not reveal which case occurred to HTTP.
void dummyPasswordCheck(const QByteArray &password)
{
    const QByteArray dummy_salt("AOWIS-DUMMY-SALT", password_salt_bytes);
    (void)derivePassword(password, dummy_salt, password_iterations);
}

QByteArray uuidBytes(const QUuid &uuid)
{
    return uuid.toRfc4122();
}

} // namespace

bool Database::migratePasswordCredentialsV11(QString *error)
{
    if (!beginWrite(error))
        return false;
    if (!exec(QStringLiteral(R"SQL(CREATE TABLE IF NOT EXISTS user_password_credentials (
        user_uuid BLOB PRIMARY KEY NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
        salt BLOB NOT NULL CHECK(length(salt)=16),
        verifier BLOB NOT NULL CHECK(length(verifier)=32),
        iterations INTEGER NOT NULL CHECK(iterations>=100000 AND iterations<=2000000),
        changed_at_ms INTEGER NOT NULL,
        changed_by_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
        failed_attempts INTEGER NOT NULL DEFAULT 0 CHECK(failed_attempts>=0 AND failed_attempts<=5),
        locked_until_ms INTEGER NOT NULL DEFAULT 0
    ))SQL"), error) ||
        !setSchemaVersion(11, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

bool Database::setUserPassword(const OperationContext &admin, const QUuid &user_uuid,
                               const QByteArray &password, QString *error)
{
    if (!requirePermission(admin, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    if (user_uuid.isNull() || !validPassword(password))
    {
        setError(error, QStringLiteral("Password must contain 12..1024 UTF-8 bytes without NUL"));
        return false;
    }
    const QByteArray salt = createSalt();
    const QByteArray verifier = derivePassword(password, salt, password_iterations);
    if (salt.size() != password_salt_bytes || verifier.size() != password_hash_bytes)
    {
        setError(error, QStringLiteral("Password derivation failed"));
        return false;
    }
    return auditedSecurityMutation(admin.actor_uuid, QStringLiteral("credential.set"), user_uuid,
        [&]() -> bool {
        QSqlQuery query(this->database_);
        if (!query.prepare(QStringLiteral(
            "INSERT INTO user_password_credentials(user_uuid,salt,verifier,iterations,changed_at_ms,changed_by_uuid) "
            "SELECT uuid,?,?,?,?,? FROM users WHERE uuid=? AND disabled_at_ms IS NULL "
            "ON CONFLICT(user_uuid) DO UPDATE SET salt=excluded.salt,verifier=excluded.verifier,"
            "iterations=excluded.iterations,changed_at_ms=excluded.changed_at_ms,"
            "changed_by_uuid=excluded.changed_by_uuid,failed_attempts=0,locked_until_ms=0")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(salt);
        query.addBindValue(verifier);
        query.addBindValue(password_iterations);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        query.addBindValue(now);
        query.addBindValue(uuidBytes(admin.actor_uuid));
        query.addBindValue(uuidBytes(user_uuid));
        if (!query.exec() || query.numRowsAffected() != 1)
        {
            setError(error, QStringLiteral("User not found or disabled"));
            return false;
        }
        query.finish();
        // Password reset always invalidates old sessions, including tokens
        // that would otherwise become usable again after a user is enabled.
        if (!query.prepare(QStringLiteral(
            "UPDATE user_sessions SET revoked_at_ms=? WHERE user_uuid=? AND revoked_at_ms IS NULL")))
        {
            setError(error, query.lastError().text());
            return false;
        }
        query.addBindValue(now);
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
        query.addBindValue(now);
        query.addBindValue(uuidBytes(user_uuid));
        if (!query.exec())
        {
            setError(error, query.lastError().text());
            return false;
        }
        return true;
    }, error);
}

bool Database::changeOwnPassword(const QByteArray &bearer_token,
                                 const QByteArray &current_password,
                                 const QByteArray &new_password, QString *error)
{
    const QString rejected = QStringLiteral("Current password incorrect or temporarily unavailable");
    // The current password comes from an authenticated but still untrusted
    // caller. Do not feed arbitrarily large buffers into OpenSSL's PBKDF2
    // implementation (which accepts its input length as an int).
    if (current_password.isEmpty() || current_password.size() > 1024)
    {
        setError(error, rejected);
        return false;
    }
    if (!validPassword(new_password) || new_password == current_password)
    {
        setError(error, QStringLiteral("New password must differ and contain 12..1024 UTF-8 bytes without NUL"));
        return false;
    }
    const std::optional<ActiveSession> session = activeSession(bearer_token, error);
    if (!session)
        return false;

    // Perform the expensive PBKDF2 computations outside SQLite's write lock.
    // The current session and exact credential are rechecked under that lock.
    QSqlQuery lookup(this->database_);
    if (!lookup.prepare(QStringLiteral(
        "SELECT c.salt,c.verifier,c.iterations,c.failed_attempts,c.locked_until_ms "
        "FROM user_password_credentials c WHERE c.user_uuid=?")))
    {
        setError(error, lookup.lastError().text());
        return false;
    }
    lookup.addBindValue(uuidBytes(session->user_uuid));
    if (!lookup.exec())
    {
        setError(error, lookup.lastError().text());
        return false;
    }
    if (!lookup.next())
    {
        lookup.finish();
        dummyPasswordCheck(current_password);
        setError(error, rejected);
        return false;
    }
    const QByteArray previous_salt = lookup.value(0).toByteArray();
    const QByteArray previous_verifier = lookup.value(1).toByteArray();
    const int previous_iterations = lookup.value(2).toInt();
    lookup.finish();

    const QByteArray computed = derivePassword(current_password, previous_salt, previous_iterations);
    const bool matches = computed.size() == password_hash_bytes &&
        previous_verifier.size() == password_hash_bytes &&
        CRYPTO_memcmp(computed.constData(), previous_verifier.constData(), password_hash_bytes) == 0;
    QByteArray new_salt;
    QByteArray new_verifier;
    if (matches)
    {
        new_salt = createSalt();
        new_verifier = derivePassword(new_password, new_salt, password_iterations);
        if (new_salt.size() != password_salt_bytes || new_verifier.size() != password_hash_bytes)
        {
            setError(error, QStringLiteral("Password derivation failed"));
            return false;
        }
    }

    if (!beginWrite(error))
        return false;
    const std::optional<ActiveSession> current_session = activeSession(bearer_token, error);
    if (!current_session || current_session->uuid != session->uuid ||
        current_session->user_uuid != session->user_uuid)
    {
        setError(error, QStringLiteral("Invalid or expired session"));
        rollback();
        return false;
    }
    QSqlQuery check(this->database_);
    if (!check.prepare(QStringLiteral(
        "SELECT salt,verifier,iterations,failed_attempts,locked_until_ms "
        "FROM user_password_credentials WHERE user_uuid=?")))
    {
        setError(error, check.lastError().text());
        rollback();
        return false;
    }
    check.addBindValue(uuidBytes(session->user_uuid));
    if (!check.exec())
    {
        setError(error, check.lastError().text());
        rollback();
        return false;
    }
    const bool present = check.next();
    const bool unchanged = present && check.value(0).toByteArray() == previous_salt &&
        check.value(1).toByteArray() == previous_verifier &&
        check.value(2).toInt() == previous_iterations;
    const int failures = present ? check.value(3).toInt() : 0;
    const qint64 locked_until = present ? check.value(4).toLongLong() : 0;
    check.finish();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!unchanged || locked_until > now || !matches)
    {
        // Share the password-login failure counter/lockout. Rejected attempts
        // must never reset it or bypass a lockout through this endpoint.
        if (unchanged && locked_until <= now && !matches)
        {
            QSqlQuery count(this->database_);
            if (!count.prepare(QStringLiteral(
                "UPDATE user_password_credentials SET failed_attempts=?,locked_until_ms=? WHERE user_uuid=?")))
            {
                setError(error, count.lastError().text());
                rollback();
                return false;
            }
            const int attempts = locked_until > 0 ? 1 : qMin(password_failure_limit, failures + 1);
            count.addBindValue(attempts);
            count.addBindValue(attempts == password_failure_limit ? now + password_lockout_ms : 0);
            count.addBindValue(uuidBytes(session->user_uuid));
            if (!count.exec() || count.numRowsAffected() != 1)
            {
                setError(error, count.lastError().text());
                rollback();
                return false;
            }
            count.finish();
            if (!commit(error))
            {
                rollback();
                return false;
            }
        }
        else
            rollback();
        setError(error, rejected);
        return false;
    }

    QSqlQuery update(this->database_);
    if (!update.prepare(QStringLiteral(
        "UPDATE user_password_credentials SET salt=?,verifier=?,iterations=?,changed_at_ms=?,"
        "changed_by_uuid=?,failed_attempts=0,locked_until_ms=0 WHERE user_uuid=?")))
    {
        setError(error, update.lastError().text());
        rollback();
        return false;
    }
    update.addBindValue(new_salt);
    update.addBindValue(new_verifier);
    update.addBindValue(password_iterations);
    update.addBindValue(now);
    update.addBindValue(uuidBytes(session->user_uuid));
    update.addBindValue(uuidBytes(session->user_uuid));
    if (!update.exec() || update.numRowsAffected() != 1)
    {
        setError(error, update.lastError().text());
        rollback();
        return false;
    }
    update.finish();
    QSqlQuery sessions(this->database_);
    if (!sessions.prepare(QStringLiteral(
        "UPDATE user_sessions SET revoked_at_ms=? WHERE user_uuid=? AND revoked_at_ms IS NULL")))
    {
        setError(error, sessions.lastError().text());
        rollback();
        return false;
    }
    sessions.addBindValue(now);
    sessions.addBindValue(uuidBytes(session->user_uuid));
    if (!sessions.exec())
    {
        setError(error, sessions.lastError().text());
        rollback();
        return false;
    }
    sessions.finish();
    QSqlQuery recovery(this->database_);
    if (!recovery.prepare(QStringLiteral(
            "UPDATE password_recovery_tokens SET invalidated_at_ms=? "
            "WHERE user_uuid=? AND used_at_ms IS NULL AND invalidated_at_ms IS NULL")))
    {
        setError(error, recovery.lastError().text());
        rollback();
        return false;
    }
    recovery.addBindValue(now);
    recovery.addBindValue(uuidBytes(session->user_uuid));
    if (!recovery.exec())
    {
        setError(error, recovery.lastError().text());
        rollback();
        return false;
    }
    recovery.finish();
    if (!writeSecurityAuditEvent(session->user_uuid, QStringLiteral("credential.change"),
                                 session->user_uuid, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<IssuedSession> Database::loginWithPassword(const QString &username,
    const QByteArray &password, qint64 lifetime_seconds, QString *error)
{
    const QString failure = QStringLiteral("Invalid credentials or temporarily unavailable");
    if (username.isEmpty() || username.size() > 256 || password.size() > 1024 ||
        password.isEmpty() || lifetime_seconds < 1 || lifetime_seconds > 30 * 86400)
    {
        setError(error, failure);
        return std::nullopt;
    }
    QSqlQuery lookup(this->database_);
    if (!lookup.prepare(QStringLiteral(
        "SELECT u.uuid,c.salt,c.verifier,c.iterations,c.failed_attempts,c.locked_until_ms "
        "FROM users u JOIN user_password_credentials c ON c.user_uuid=u.uuid "
        "WHERE u.username=? AND u.disabled_at_ms IS NULL")))
    {
        setError(error, QStringLiteral("Credential lookup failed"));
        return std::nullopt;
    }
    lookup.addBindValue(username);
    if (!lookup.exec())
    {
        setError(error, QStringLiteral("Credential lookup failed"));
        return std::nullopt;
    }
    if (!lookup.next())
    {
        lookup.finish();
        dummyPasswordCheck(password);
        setError(error, failure);
        return std::nullopt;
    }
    const QUuid user_uuid = QUuid::fromRfc4122(lookup.value(0).toByteArray());
    const QByteArray salt = lookup.value(1).toByteArray();
    const QByteArray verifier = lookup.value(2).toByteArray();
    const int iterations = lookup.value(3).toInt();
    const qint64 locked_until = lookup.value(5).toLongLong();
    lookup.finish();
    if (locked_until > QDateTime::currentMSecsSinceEpoch())
    {
        dummyPasswordCheck(password);
        setError(error, failure);
        return std::nullopt;
    }
    const QByteArray derived = derivePassword(password, salt, iterations);
    const bool password_matches = derived.size() == password_hash_bytes &&
        verifier.size() == password_hash_bytes && CRYPTO_memcmp(derived.constData(), verifier.constData(),
                                                                password_hash_bytes) == 0;
    if (!beginWrite(error))
        return std::nullopt;
    // Recheck credential changes, disabled accounts and lockout under the
    // write transaction; never issue a token using a stale password verifier.
    QSqlQuery check(this->database_);
    if (!check.prepare(QStringLiteral(
        "SELECT c.salt,c.verifier,c.iterations,c.failed_attempts,c.locked_until_ms "
        "FROM user_password_credentials c JOIN users u ON u.uuid=c.user_uuid "
        "WHERE u.uuid=? AND u.disabled_at_ms IS NULL")))
    {
        setError(error, check.lastError().text());
        rollback();
        return std::nullopt;
    }
    check.addBindValue(uuidBytes(user_uuid));
    if (!check.exec())
    {
        setError(error, check.lastError().text());
        rollback();
        return std::nullopt;
    }
    const bool present = check.next();
    const bool same_credential = present && check.value(0).toByteArray() == salt &&
        check.value(1).toByteArray() == verifier && check.value(2).toInt() == iterations;
    const int failures = present ? check.value(3).toInt() : 0;
    const qint64 lock_expiry = present ? check.value(4).toLongLong() : 0;
    check.finish();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!present || !same_credential || lock_expiry > now || !password_matches)
    {
        if (present && same_credential && lock_expiry <= now)
        {
            QSqlQuery update(this->database_);
            if (!update.prepare(QStringLiteral(
                "UPDATE user_password_credentials SET failed_attempts=?,locked_until_ms=? WHERE user_uuid=?")))
            {
                setError(error, update.lastError().text());
                rollback();
                return std::nullopt;
            }
            const int count = lock_expiry > 0 ? 1 : qMin(password_failure_limit, failures + 1);
            update.addBindValue(count);
            update.addBindValue(count == password_failure_limit ? now + password_lockout_ms : 0);
            update.addBindValue(uuidBytes(user_uuid));
            if (!update.exec() || !commit(error))
            {
                rollback();
                return std::nullopt;
            }
        }
        else
            rollback();
        setError(error, failure);
        return std::nullopt;
    }
    // Enforce the same per-user quota as administrator-issued sessions. This
    // check occurs under BEGIN IMMEDIATE, after password verification, so
    // concurrent logins cannot exceed the limit.
    if (!hasSessionCapacity(this->database_, user_uuid, now, error))
    {
        // Do not disclose the account's session usage through a login reply.
        setError(error, failure);
        rollback();
        return std::nullopt;
    }
    const QByteArray random = makeSessionToken();
    if (random.size() != 64)
    {
        setError(error, QStringLiteral("Session token generation failed"));
        rollback();
        return std::nullopt;
    }
    IssuedSession session{createUuidV7(), user_uuid, random, now + lifetime_seconds * 1000};
    QSqlQuery insert(this->database_);
    if (!insert.prepare(QStringLiteral(
        "INSERT INTO user_sessions(uuid,user_uuid,token_sha256,created_by_uuid,created_at_ms,expires_at_ms) "
        "VALUES (?,?,?,?,?,?)")))
    {
        setError(error, insert.lastError().text());
        rollback();
        return std::nullopt;
    }
    insert.addBindValue(uuidBytes(session.uuid));
    insert.addBindValue(uuidBytes(user_uuid));
    insert.addBindValue(sessionTokenDigest(QByteArray::fromHex(random)));
    insert.addBindValue(uuidBytes(user_uuid));
    insert.addBindValue(now);
    insert.addBindValue(session.expires_at_ms);
    if (!insert.exec())
    {
        setError(error, insert.lastError().text());
        rollback();
        return std::nullopt;
    }
    insert.finish();
    QSqlQuery reset_failures(this->database_);
    if (!reset_failures.prepare(QStringLiteral(
        "UPDATE user_password_credentials SET failed_attempts=0,locked_until_ms=0 WHERE user_uuid=?")))
    {
        setError(error, reset_failures.lastError().text());
        rollback();
        return std::nullopt;
    }
    reset_failures.addBindValue(uuidBytes(user_uuid));
    if (!reset_failures.exec())
    {
        setError(error, reset_failures.lastError().text());
        rollback();
        return std::nullopt;
    }
    reset_failures.finish();
    if (!writeSecurityAuditEvent(user_uuid, QStringLiteral("session.password_login"), session.uuid, error) ||
        !commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return session;
}


// The reset secret is a capability, not a session. It cannot be used with any
// ordinary authenticated endpoint; only the recovery redemption operation.
namespace
{
QByteArray recoveryDigest(const QByteArray &token)
{
    if (token.size() != 64)
        return {};
    for (const char letter : token)
    {
        if (!((letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f')))
            return {};
    }
    QByteArray data("AOWIS-PASSWORD-RECOVERY-V1\0", 27);
    data.append(QByteArray::fromHex(token));
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}
} // namespace

bool Database::migratePasswordRecoveryV12(QString *error)
{
    if (!beginWrite(error))
        return false;
    const char *const statements[] = {
        R"SQL(CREATE TABLE password_recovery_tokens (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            token_sha256 BLOB NOT NULL UNIQUE CHECK(length(token_sha256)=32),
            issued_by_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            issued_at_ms INTEGER NOT NULL,
            expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>issued_at_ms),
            used_at_ms INTEGER,
            invalidated_at_ms INTEGER,
            CHECK(used_at_ms IS NULL OR used_at_ms>=issued_at_ms),
            CHECK(invalidated_at_ms IS NULL OR invalidated_at_ms>=issued_at_ms),
            CHECK(used_at_ms IS NULL OR invalidated_at_ms IS NULL)
        ))SQL",
        R"SQL(CREATE INDEX recovery_tokens_by_user ON password_recovery_tokens(user_uuid))SQL",
        R"SQL(CREATE TRIGGER recovery_tokens_no_delete BEFORE DELETE ON password_recovery_tokens
            BEGIN SELECT RAISE(ABORT,'recovery tokens cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER recovery_tokens_close_only BEFORE UPDATE ON password_recovery_tokens
            WHEN NEW.uuid!=OLD.uuid OR NEW.user_uuid!=OLD.user_uuid
               OR NEW.token_sha256!=OLD.token_sha256 OR NEW.issued_by_uuid!=OLD.issued_by_uuid
               OR NEW.issued_at_ms!=OLD.issued_at_ms OR NEW.expires_at_ms!=OLD.expires_at_ms
               OR OLD.used_at_ms IS NOT NEW.used_at_ms AND OLD.used_at_ms IS NOT NULL
               OR OLD.invalidated_at_ms IS NOT NEW.invalidated_at_ms AND OLD.invalidated_at_ms IS NOT NULL
            BEGIN SELECT RAISE(ABORT,'recovery identity and closure are immutable'); END)SQL"
    };
    for (const char *statement : statements)
    {
        if (!exec(QString::fromUtf8(statement), error))
        {
            rollback();
            return false;
        }
    }
    if (!setSchemaVersion(12, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<IssuedRecoveryToken> Database::issuePasswordRecovery(
    const OperationContext &admin, const QUuid &user_uuid, qint64 lifetime_seconds, QString *error)
{
    if (!requirePermission(admin, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    if (user_uuid.isNull() || lifetime_seconds < 1 || lifetime_seconds > 3600)
    {
        setError(error, QStringLiteral("Invalid user or recovery expiration (1..3600 seconds)"));
        return std::nullopt;
    }
    QByteArray random(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(random.data()), random.size()) != 1)
    {
        setError(error, QStringLiteral("Recovery token generation failed"));
        return std::nullopt;
    }
    const QByteArray token = random.toHex();
    const QByteArray digest = recoveryDigest(token);
    if (digest.size() != 32)
    {
        setError(error, QStringLiteral("Recovery token hashing failed"));
        return std::nullopt;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    IssuedRecoveryToken result{createUuidV7(), user_uuid, token, now + lifetime_seconds * 1000};
    const bool success = auditedSecurityMutation(admin.actor_uuid,
        QStringLiteral("credential.recovery.issue"), result.uuid, [&]() -> bool {
        QSqlQuery exists(this->database_);
        if (!exists.prepare(QStringLiteral("SELECT 1 FROM users WHERE uuid=? AND disabled_at_ms IS NULL")))
        {
            setError(error, exists.lastError().text());
            return false;
        }
        exists.addBindValue(uuidBytes(user_uuid));
        if (!exists.exec() || !exists.next())
        {
            setError(error, QStringLiteral("User not found or disabled"));
            return false;
        }
        exists.finish();
        // A newly issued token invalidates earlier unredeemed tokens. Keep
        // the old records for documentation and replay rejection.
        QSqlQuery invalidate(this->database_);
        if (!invalidate.prepare(QStringLiteral(
                "UPDATE password_recovery_tokens SET invalidated_at_ms=? "
                "WHERE user_uuid=? AND used_at_ms IS NULL AND invalidated_at_ms IS NULL")))
        {
            setError(error, invalidate.lastError().text());
            return false;
        }
        invalidate.addBindValue(now);
        invalidate.addBindValue(uuidBytes(user_uuid));
        if (!invalidate.exec())
        {
            setError(error, invalidate.lastError().text());
            return false;
        }
        invalidate.finish();
        QSqlQuery insert(this->database_);
        if (!insert.prepare(QStringLiteral(
                "INSERT INTO password_recovery_tokens(uuid,user_uuid,token_sha256,issued_by_uuid,"
                "issued_at_ms,expires_at_ms) VALUES (?,?,?,?,?,?)")))
        {
            setError(error, insert.lastError().text());
            return false;
        }
        insert.addBindValue(uuidBytes(result.uuid));
        insert.addBindValue(uuidBytes(user_uuid));
        insert.addBindValue(digest);
        insert.addBindValue(uuidBytes(admin.actor_uuid));
        insert.addBindValue(now);
        insert.addBindValue(result.expires_at_ms);
        if (!insert.exec())
        {
            setError(error, insert.lastError().text());
            return false;
        }
        return true;
    }, error);
    if (!success)
        return std::nullopt;
    return result;
}

bool Database::redeemPasswordRecovery(const QByteArray &token, const QByteArray &new_password,
                                      QString *error)
{
    if (!validPassword(new_password))
    {
        setError(error, QStringLiteral("Invalid new password"));
        return false;
    }
    const QByteArray digest = recoveryDigest(token);
    if (digest.size() != 32)
    {
        setError(error, QStringLiteral("Invalid or expired recovery token"));
        return false;
    }
    // Reject unknown/expired capabilities before doing a 600k-iteration KDF.
    // This avoids giving anonymous clients an unnecessary CPU-exhaustion path.
    // The capability is checked again inside the write transaction.
    QSqlQuery precheck(this->database_);
    if (!precheck.prepare(QStringLiteral(
            "SELECT 1 FROM password_recovery_tokens t JOIN users u ON u.uuid=t.user_uuid "
            "WHERE t.token_sha256=? AND t.used_at_ms IS NULL AND t.invalidated_at_ms IS NULL "
            "AND t.expires_at_ms>? AND u.disabled_at_ms IS NULL")))
    {
        setError(error, precheck.lastError().text());
        return false;
    }
    precheck.addBindValue(digest);
    precheck.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!precheck.exec())
    {
        setError(error, precheck.lastError().text());
        return false;
    }
    const bool available = precheck.next();
    precheck.finish();
    if (!available)
    {
        setError(error, QStringLiteral("Invalid or expired recovery token"));
        return false;
    }
    // Do the expensive password KDF outside the SQLite write transaction.
    // We recheck and close the capability atomically before using it.
    const QByteArray salt = createSalt();
    const QByteArray verifier = derivePassword(new_password, salt, password_iterations);
    if (salt.size() != password_salt_bytes || verifier.size() != password_hash_bytes)
    {
        setError(error, QStringLiteral("Password derivation failed"));
        return false;
    }
    if (!beginWrite(error))
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery lookup(this->database_);
    if (!lookup.prepare(QStringLiteral(
            "SELECT t.uuid,t.user_uuid FROM password_recovery_tokens t "
            "JOIN users u ON u.uuid=t.user_uuid "
            "WHERE t.token_sha256=? AND t.used_at_ms IS NULL AND t.invalidated_at_ms IS NULL "
            "AND t.expires_at_ms>? AND u.disabled_at_ms IS NULL")))
    {
        setError(error, lookup.lastError().text());
        rollback();
        return false;
    }
    lookup.addBindValue(digest);
    lookup.addBindValue(now);
    if (!lookup.exec())
    {
        setError(error, lookup.lastError().text());
        rollback();
        return false;
    }
    if (!lookup.next())
    {
        lookup.finish();
        setError(error, QStringLiteral("Invalid or expired recovery token"));
        rollback();
        return false;
    }
    const QUuid recovery_uuid = QUuid::fromRfc4122(lookup.value(0).toByteArray());
    const QUuid user_uuid = QUuid::fromRfc4122(lookup.value(1).toByteArray());
    lookup.finish();
    QSqlQuery close_token(this->database_);
    if (!close_token.prepare(QStringLiteral(
            "UPDATE password_recovery_tokens SET used_at_ms=? WHERE uuid=? AND used_at_ms IS NULL "
            "AND invalidated_at_ms IS NULL AND expires_at_ms>?")))
    {
        setError(error, close_token.lastError().text());
        rollback();
        return false;
    }
    close_token.addBindValue(now);
    close_token.addBindValue(uuidBytes(recovery_uuid));
    close_token.addBindValue(now);
    if (!close_token.exec() || close_token.numRowsAffected() != 1)
    {
        setError(error, QStringLiteral("Invalid or expired recovery token"));
        rollback();
        return false;
    }
    close_token.finish();
    QSqlQuery update(this->database_);
    if (!update.prepare(QStringLiteral(
            "INSERT INTO user_password_credentials(user_uuid,salt,verifier,iterations,changed_at_ms,changed_by_uuid) "
            "VALUES(?,?,?,?,?,?) ON CONFLICT(user_uuid) DO UPDATE SET salt=excluded.salt,"
            "verifier=excluded.verifier,iterations=excluded.iterations,changed_at_ms=excluded.changed_at_ms,"
            "changed_by_uuid=excluded.changed_by_uuid,failed_attempts=0,locked_until_ms=0")))
    {
        setError(error, update.lastError().text());
        rollback();
        return false;
    }
    update.addBindValue(uuidBytes(user_uuid));
    update.addBindValue(salt);
    update.addBindValue(verifier);
    update.addBindValue(password_iterations);
    update.addBindValue(now);
    // Redemption proves possession of a capability bound to this account,
    // not participation by the administrator who originally issued it.
    // Attribution is to the affected account; the issuer remains recorded
    // separately in password_recovery_tokens.issued_by_uuid.
    update.addBindValue(uuidBytes(user_uuid));
    if (!update.exec())
    {
        setError(error, update.lastError().text());
        rollback();
        return false;
    }
    update.finish();
    QSqlQuery sessions(this->database_);
    if (!sessions.prepare(QStringLiteral(
            "UPDATE user_sessions SET revoked_at_ms=? WHERE user_uuid=? AND revoked_at_ms IS NULL")))
    {
        setError(error, sessions.lastError().text());
        rollback();
        return false;
    }
    sessions.addBindValue(now);
    sessions.addBindValue(uuidBytes(user_uuid));
    if (!sessions.exec())
    {
        setError(error, sessions.lastError().text());
        rollback();
        return false;
    }
    sessions.finish();
    // A recovered account is the audit actor in the sense of capability
    // ownership, NOT a verified human presenter. Never falsely attribute
    // later token redemption to its issuing administrator.
    if (!writeSecurityAuditEvent(user_uuid, QStringLiteral("credential.recovery.redeem"),
                                 recovery_uuid, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

} // namespace aowis::db
