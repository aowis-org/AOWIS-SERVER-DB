#include <aowis/db/database.h>
#include "storage_backend.h"
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
    // Frozen audit encoding: domain separator, previous hash, sequence, UTC ms, event,
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

SecurityAuditEvent readEvent(const QSqlQuery &query, const StorageBackend &backend)
{
    SecurityAuditEvent event;
    event.sequence = query.value(0).toLongLong();
    event.uuid = backend.readUuid(query.value(1));
    event.actor_uuid = backend.readUuid(query.value(2));
    event.subject_uuid = backend.readUuid(query.value(3));
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
    insert.addBindValue(this->backend_->bindUuid(event.uuid));
    insert.addBindValue(this->backend_->bindUuid(event.actor_uuid));
    insert.addBindValue(this->backend_->bindUuid(event.subject_uuid));
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
        result.append(readEvent(query, *this->backend_));
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
        const SecurityAuditEvent event = readEvent(query, *this->backend_);
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
