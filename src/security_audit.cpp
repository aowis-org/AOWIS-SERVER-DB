#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include "builtin_identity_ids.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
QByteArray uuidBytes(const QUuid &uuid)
{
    return uuid.toRfc4122();
}

void setError(QString *error, const QString &value)
{
    if (error != nullptr)
        *error = value;
}

void appendInt64(QByteArray &buffer, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        buffer.append(static_cast<char>((value >> shift) & 0xffU));
}

QByteArray eventHash(const SecurityAuditEvent &event)
{
    // Format V1: domain separator, previous hash, sequence, UTC ms, event,
    // actor, subject UUIDs, and length-prefixed UTF-8 action. Frozen format.
    QByteArray bytes("AOWIS-SECURITY-AUDIT-V1", 23);
    bytes.append('\0');
    bytes.append(event.previous_sha256);
    appendInt64(bytes, static_cast<quint64>(event.sequence));
    appendInt64(bytes, static_cast<quint64>(event.occurred_at_ms));
    bytes.append(uuidBytes(event.uuid));
    bytes.append(uuidBytes(event.actor_uuid));
    bytes.append(uuidBytes(event.subject_uuid));
    const QByteArray action = event.action.toUtf8();
    appendInt64(bytes, static_cast<quint64>(action.size()));
    bytes.append(action);
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

SecurityAuditEvent readEvent(const QSqlQuery &query)
{
    SecurityAuditEvent event;
    event.sequence = query.value(0).toLongLong();
    event.uuid = QUuid::fromRfc4122(query.value(1).toByteArray());
    event.actor_uuid = QUuid::fromRfc4122(query.value(2).toByteArray());
    event.subject_uuid = QUuid::fromRfc4122(query.value(3).toByteArray());
    event.action = query.value(4).toString();
    event.occurred_at_ms = query.value(5).toLongLong();
    event.previous_sha256 = query.value(6).toByteArray();
    event.chain_sha256 = query.value(7).toByteArray();
    return event;
}

QString eventSelect()
{
    return QStringLiteral("SELECT sequence,uuid,actor_uuid,subject_uuid,action,occurred_at_ms,"
                          "previous_sha256,chain_sha256 FROM security_audit_events");
}
} // namespace

bool Database::migrateSecurityAuditV9(QString *error)
{
    if (!beginWrite(error))
        return false;
    const char *const statements[] = {
        R"SQL(CREATE TABLE security_audit_events (
            sequence INTEGER PRIMARY KEY AUTOINCREMENT,
            uuid BLOB NOT NULL UNIQUE CHECK(length(uuid)=16),
            actor_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            subject_uuid BLOB NOT NULL CHECK(length(subject_uuid)=16),
            action TEXT NOT NULL CHECK(length(action)>0),
            occurred_at_ms INTEGER NOT NULL,
            previous_sha256 BLOB NOT NULL CHECK(length(previous_sha256)=32),
            chain_sha256 BLOB NOT NULL CHECK(length(chain_sha256)=32)
        ))SQL",
        R"SQL(CREATE INDEX security_audit_by_actor ON security_audit_events(actor_uuid,sequence))SQL",
        R"SQL(CREATE TRIGGER security_audit_readonly_update
            BEFORE UPDATE ON security_audit_events
            BEGIN SELECT RAISE(ABORT,'security audit events are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER security_audit_readonly_delete
            BEFORE DELETE ON security_audit_events
            BEGIN SELECT RAISE(ABORT,'security audit events cannot be deleted'); END)SQL"
    };
    for (const char *statement : statements)
    {
        if (!exec(QString::fromUtf8(statement), error))
        {
            rollback();
            return false;
        }
    }
    // V8 installations already have their V2 permissions catalog. The new
    // security.audit.read permission was not present when they were created,
    // so migrating just the audit table would lock administrators out.
    // Reuse the pinned built-in UUID, and preserve existing role assignments.
    const QUuid audit_permission_uuid(QStringLiteral("{%1}").arg(
        QString::fromLatin1(internal::security_audit_read_permission_uuid)));
    QSqlQuery permission(this->database_);
    if (!permission.prepare(QStringLiteral(
            "INSERT INTO permissions(uuid,key,builtin) VALUES (?,'security.audit.read',1) "
            "ON CONFLICT(key) DO NOTHING")))
    {
        setError(error, permission.lastError().text());
        rollback();
        return false;
    }
    permission.addBindValue(uuidBytes(audit_permission_uuid));
    if (!permission.exec())
    {
        setError(error, permission.lastError().text());
        rollback();
        return false;
    }
    permission.finish();

    // The permission UUID and built-in flag must be correct even when a
    // database already contains a row with that permission key.
    QSqlQuery canonical(this->database_);
    if (!canonical.prepare(QStringLiteral(
            "SELECT uuid,builtin FROM permissions WHERE key='security.audit.read'")) ||
        !canonical.exec() || !canonical.next() ||
        canonical.value(0).toByteArray() != uuidBytes(audit_permission_uuid) ||
        canonical.value(1).toInt() != 1)
    {
        setError(error, QStringLiteral("Conflicting built-in security.audit.read permission"));
        rollback();
        return false;
    }
    canonical.finish();

    // Ensure the system administrator role can actually read the journal.
    // Existing grants are not duplicated, and other roles are not modified.
    QSqlQuery assignment(this->database_);
    if (!assignment.prepare(QStringLiteral(R"SQL(
        INSERT INTO role_permissions(uuid,role_uuid,permission_uuid)
        SELECT ?,r.uuid,p.uuid FROM roles r CROSS JOIN permissions p
        WHERE r.key='system.administrator' AND p.key='security.audit.read'
          AND NOT EXISTS (SELECT 1 FROM role_permissions rp
                          WHERE rp.role_uuid=r.uuid AND rp.permission_uuid=p.uuid)
    )SQL")))
    {
        setError(error, assignment.lastError().text());
        rollback();
        return false;
    }
    assignment.addBindValue(uuidBytes(createUuidV7()));
    if (!assignment.exec())
    {
        setError(error, assignment.lastError().text());
        rollback();
        return false;
    }
    assignment.finish();

    QSqlQuery grant_check(this->database_);
    if (!grant_check.exec(QStringLiteral(R"SQL(
        SELECT 1 FROM role_permissions rp
        JOIN roles r ON r.uuid=rp.role_uuid
        JOIN permissions p ON p.uuid=rp.permission_uuid
        WHERE r.key='system.administrator' AND p.key='security.audit.read'
    )SQL")) || !grant_check.next())
    {
        setError(error, QStringLiteral("Missing system administrator audit-read grant"));
        rollback();
        return false;
    }
    grant_check.finish();

    if (!setSchemaVersion(9, error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

bool Database::writeSecurityAuditEvent(const QUuid &actor_uuid, const QString &action,
                                        const QUuid &subject_uuid, QString *error)
{
    // Caller must hold BEGIN IMMEDIATE. All business changes and audit records
    // commit together; do not expose this helper as an external API.
    QSqlQuery last(this->database_);
    if (!last.exec(QStringLiteral("SELECT sequence,chain_sha256 FROM security_audit_events "
                                  "ORDER BY sequence DESC LIMIT 1")))
    {
        setError(error, last.lastError().text());
        return false;
    }
    SecurityAuditEvent event;
    event.uuid = createUuidV7();
    event.actor_uuid = actor_uuid;
    event.subject_uuid = subject_uuid;
    event.action = action;
    event.occurred_at_ms = QDateTime::currentMSecsSinceEpoch();
    event.previous_sha256 = QByteArray(32, '\0');
    event.sequence = 1;
    if (last.next())
    {
        event.sequence = last.value(0).toLongLong() + 1;
        event.previous_sha256 = last.value(1).toByteArray();
    }
    if (event.previous_sha256.size() != 32)
    {
        setError(error, QStringLiteral("Invalid preceding security audit hash"));
        return false;
    }
    event.chain_sha256 = eventHash(event);
    QSqlQuery insert(this->database_);
    if (!insert.prepare(QStringLiteral(
        "INSERT INTO security_audit_events (sequence,uuid,actor_uuid,subject_uuid,action,"
        "occurred_at_ms,previous_sha256,chain_sha256) VALUES(?,?,?,?,?,?,?,?)")))
    {
        setError(error, insert.lastError().text());
        return false;
    }
    insert.addBindValue(event.sequence);
    insert.addBindValue(uuidBytes(event.uuid));
    insert.addBindValue(uuidBytes(event.actor_uuid));
    insert.addBindValue(uuidBytes(event.subject_uuid));
    insert.addBindValue(event.action);
    insert.addBindValue(event.occurred_at_ms);
    insert.addBindValue(event.previous_sha256);
    insert.addBindValue(event.chain_sha256);
    if (!insert.exec())
    {
        setError(error, insert.lastError().text());
        return false;
    }
    return true;
}

bool Database::auditedSecurityMutation(const QUuid &actor_uuid, const QString &action,
                                        const QUuid &subject_uuid,
                                        const std::function<bool()> &mutation, QString *error)
{
    if (!beginWrite(error))
        return false;
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("security.manage"),
                           AccessScope::system(), error) || !mutation())
    {
        rollback();
        return false;
    }
    // Never let a security change remove the last active system manager.
    // Otherwise disabling an administrator or revoking their sole grant could
    // permanently lock every user out of the supported administration API.
    QSqlQuery managers(this->database_);
    if (!managers.exec(QStringLiteral(R"SQL(
        SELECT 1 FROM users u JOIN role_assignments a ON (
            (a.principal_kind='user' AND a.principal_uuid=u.uuid) OR
            (a.principal_kind='group' AND EXISTS(
                SELECT 1 FROM group_memberships m JOIN groups g ON g.uuid=m.group_uuid
                WHERE m.user_uuid=u.uuid AND m.group_uuid=a.principal_uuid
                AND m.revoked_at_ms IS NULL AND g.disabled_at_ms IS NULL)))
        JOIN role_permissions rp ON rp.role_uuid=a.role_uuid AND rp.revoked_at_ms IS NULL
        JOIN permissions p ON p.uuid=rp.permission_uuid
        WHERE u.disabled_at_ms IS NULL AND a.revoked_at_ms IS NULL
          AND a.scope_kind='system' AND p.key='security.manage' LIMIT 1
    )SQL")) || !managers.next())
    {
        setError(error, QStringLiteral("Cannot remove the last active system administrator"));
        rollback();
        return false;
    }
    managers.finish();
    if (!writeSecurityAuditEvent(actor_uuid, action, subject_uuid, error))
    {
        rollback();
        return false;
    }
    if (!commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

QVector<SecurityAuditEvent> Database::securityAudit(const OperationContext &actor,
                                                    qint64 after_sequence, int limit,
                                                    QString *error) const
{
    QVector<SecurityAuditEvent> result;
    if (!requirePermission(actor, QStringLiteral("security.audit.read"), AccessScope::system(), error))
        return result;
    if (after_sequence < 0 || limit < 1 || limit > 1000)
    {
        setError(error, QStringLiteral("Invalid security audit page (limit 1..1000)"));
        return result;
    }
    QSqlQuery query(this->database_);
    if (!query.prepare(eventSelect() + QStringLiteral(" WHERE sequence>? ORDER BY sequence LIMIT ?")))
    {
        setError(error, query.lastError().text());
        return result;
    }
    query.addBindValue(after_sequence);
    query.addBindValue(limit);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return result;
    }
    while (query.next())
        result.append(readEvent(query));
    return result;
}

SecurityAuditReport Database::verifySecurityAudit(const OperationContext &actor) const
{
    SecurityAuditReport report;
    if (!requirePermission(actor, QStringLiteral("security.audit.read"), AccessScope::system(), &report.error))
        return report;
    QSqlQuery query(this->database_);
    if (!query.exec(eventSelect() + QStringLiteral(" ORDER BY sequence")))
    {
        report.error = query.lastError().text();
        return report;
    }
    QByteArray previous(32, '\0');
    qint64 expected_sequence = 1;
    while (query.next())
    {
        const SecurityAuditEvent event = readEvent(query);
        report.failed_sequence = event.sequence;
        if (event.sequence != expected_sequence || event.uuid.isNull() ||
            event.actor_uuid.isNull() || event.subject_uuid.isNull() ||
            event.previous_sha256 != previous || event.chain_sha256 != eventHash(event))
        {
            report.error = QStringLiteral("Security audit chain mismatch at sequence %1")
                               .arg(event.sequence);
            return report;
        }
        previous = event.chain_sha256;
        ++expected_sequence;
        ++report.checked_events;
    }
    report.valid = true;
    report.failed_sequence = 0;
    return report;
}

} // namespace aowis::db
