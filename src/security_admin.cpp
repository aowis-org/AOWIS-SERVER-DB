#include <aowis/db/database.h>

#include "storage_backend.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

} // namespace

bool Database::removeGroupMember(const OperationContext &actor, const QUuid &membership_uuid,
                                 QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    return auditedSecurityMutation(actor.actor_uuid, QStringLiteral("group.member.remove"),
        membership_uuid, [&]() -> bool {
            QSqlQuery query(this->database_);
            if (!query.prepare(QStringLiteral(
                    "UPDATE group_memberships SET revoked_at_ms=?,revoked_by_uuid=? "
                    "WHERE uuid=? AND revoked_at_ms IS NULL")))
            {
                setError(error, query.lastError().text());
                return false;
            }
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
            query.addBindValue(this->backend_->bindUuid(membership_uuid));
            if (!query.exec() || query.numRowsAffected() != 1)
            {
                setError(error, QStringLiteral("Membership not found or already revoked"));
                return false;
            }
            return true;
        }, error);
}

bool Database::revokeRolePermission(const OperationContext &actor, const QUuid &grant_uuid,
                                    QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    return auditedSecurityMutation(actor.actor_uuid, QStringLiteral("role.permission.revoke"),
        grant_uuid, [&]() -> bool {
            QSqlQuery query(this->database_);
            if (!query.prepare(QStringLiteral(
                    "UPDATE role_permissions SET revoked_at_ms=?,revoked_by_uuid=? "
                    "WHERE uuid=? AND revoked_at_ms IS NULL")))
            {
                setError(error, query.lastError().text());
                return false;
            }
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
            query.addBindValue(this->backend_->bindUuid(grant_uuid));
            if (!query.exec() || query.numRowsAffected() != 1)
            {
                setError(error, QStringLiteral("Role permission not found or already revoked"));
                return false;
            }
            return true;
        }, error);
}

bool Database::revokeRoleAssignment(const OperationContext &actor, const QUuid &assignment_uuid,
                                    QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    return auditedSecurityMutation(actor.actor_uuid, QStringLiteral("role.assignment.revoke"),
        assignment_uuid, [&]() -> bool {
            QSqlQuery query(this->database_);
            if (!query.prepare(QStringLiteral(
                    "UPDATE role_assignments SET revoked_at_ms=?,revoked_by_uuid=? "
                    "WHERE uuid=? AND revoked_at_ms IS NULL")))
            {
                setError(error, query.lastError().text());
                return false;
            }
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
            query.addBindValue(this->backend_->bindUuid(assignment_uuid));
            if (!query.exec() || query.numRowsAffected() != 1)
            {
                setError(error, QStringLiteral("Role assignment not found or already revoked"));
                return false;
            }
            return true;
        }, error);
}

bool Database::enableUser(const OperationContext &actor, const QUuid &user_uuid, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return false;
    return auditedSecurityMutation(actor.actor_uuid, QStringLiteral("user.enable"),
        user_uuid, [&]() -> bool {
            QSqlQuery query(this->database_);
            if (!query.prepare(QStringLiteral(
                    "UPDATE users SET disabled_at_ms=NULL WHERE uuid=? AND disabled_at_ms IS NOT NULL")))
            {
                setError(error, query.lastError().text());
                return false;
            }
            query.addBindValue(this->backend_->bindUuid(user_uuid));
            if (!query.exec() || query.numRowsAffected() != 1)
            {
                setError(error, QStringLiteral("User not found or already active"));
                return false;
            }
            return true;
        }, error);
}

} // namespace aowis::db
