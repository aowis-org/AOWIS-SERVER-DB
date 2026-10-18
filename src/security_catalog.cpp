#include <aowis/db/database.h>

#include "storage_backend.h"

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

bool selectPage(QSqlQuery &query, const StorageBackend &backend, const QString &sql,
                const QVector<QVariant> &filter_values,
                const SecurityListOptions &options, QString *error)
{
    if (options.limit < 1 || options.limit > 200)
    {
        setError(error, QStringLiteral("Security catalog page limit must be 1..200"));
        return false;
    }
    if (!query.prepare(sql))
    {
        setError(error, query.lastError().text());
        return false;
    }
    for (const QVariant &value : filter_values)
        query.addBindValue(value);
    // A zero UUID is the lower-bound cursor, not SQL NULL.
    query.addBindValue(backend.bindUuid(options.after_uuid));
    query.addBindValue(options.limit);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return false;
    }
    return true;
}

QString activeSql(const QString &field, bool include_inactive)
{
    return include_inactive ? QString() : QStringLiteral(" AND %1 IS NULL").arg(field);
}

QString pagedSql(const QString &prefix, const QString &uuid_column,
                 const QString &inactive_column, bool include_inactive)
{
    return prefix + activeSql(inactive_column, include_inactive) +
        QStringLiteral(" AND %1 > ? ORDER BY %1 LIMIT ?").arg(uuid_column);
}

} // namespace

QVector<UserInfo> Database::securityUsers(const OperationContext &actor,
                                          const SecurityListOptions &options, QString *error) const
{
    QVector<UserInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, pagedSql(QStringLiteral(
            "SELECT uuid,username,disabled_at_ms FROM users WHERE 1=1"),
            QStringLiteral("uuid"), QStringLiteral("disabled_at_ms"), options.include_inactive),
            {}, options, error))
        return records;
    while (query.next())
        records.append(UserInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                                !query.value(2).isNull()});
    return records;
}

QVector<GroupInfo> Database::securityGroups(const OperationContext &actor,
                                            const SecurityListOptions &options, QString *error) const
{
    QVector<GroupInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, pagedSql(QStringLiteral(
            "SELECT uuid,name,disabled_at_ms FROM groups WHERE 1=1"),
            QStringLiteral("uuid"), QStringLiteral("disabled_at_ms"), options.include_inactive),
            {}, options, error))
        return records;
    while (query.next())
        records.append(GroupInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                                 !query.value(2).isNull()});
    return records;
}

QVector<RoleInfo> Database::securityRoles(const OperationContext &actor,
                                          const SecurityListOptions &options, QString *error) const
{
    QVector<RoleInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, QStringLiteral(
            "SELECT uuid,key,builtin FROM roles WHERE uuid>? ORDER BY uuid LIMIT ?"),
            {}, options, error))
        return records;
    while (query.next())
        records.append(RoleInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                                query.value(2).toBool()});
    return records;
}

QVector<PermissionInfo> Database::securityPermissions(const OperationContext &actor,
                                                       const SecurityListOptions &options, QString *error) const
{
    QVector<PermissionInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, QStringLiteral(
            "SELECT uuid,key,builtin FROM permissions WHERE uuid>? ORDER BY uuid LIMIT ?"),
            {}, options, error))
        return records;
    while (query.next())
        records.append(PermissionInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                                      query.value(2).toBool()});
    return records;
}

QVector<GroupMembershipInfo> Database::securityGroupMembers(const OperationContext &actor,
                                                              const QUuid &group_uuid,
                                                              const SecurityListOptions &options,
                                                              QString *error) const
{
    QVector<GroupMembershipInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    if (group_uuid.isNull())
    {
        setError(error, QStringLiteral("Invalid group UUID"));
        return records;
    }
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, pagedSql(QStringLiteral(
            "SELECT uuid,group_uuid,user_uuid,created_by_uuid,created_at_ms,revoked_at_ms "
            "FROM group_memberships WHERE group_uuid=?"),
            QStringLiteral("uuid"), QStringLiteral("revoked_at_ms"), options.include_inactive),
            {this->backend_->bindUuid(group_uuid)}, options, error))
        return records;
    while (query.next())
        records.append(GroupMembershipInfo{
            this->backend_->readUuid(query.value(0)),
            this->backend_->readUuid(query.value(1)),
            this->backend_->readUuid(query.value(2)),
            this->backend_->readUuid(query.value(3)),
            query.value(4).toLongLong(), !query.value(5).isNull()});
    return records;
}

QVector<RolePermissionGrantInfo> Database::securityRolePermissions(const OperationContext &actor,
                                                                    const QUuid &role_uuid,
                                                                    const SecurityListOptions &options,
                                                                    QString *error) const
{
    QVector<RolePermissionGrantInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    if (role_uuid.isNull())
    {
        setError(error, QStringLiteral("Invalid role UUID"));
        return records;
    }
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, pagedSql(QStringLiteral(
            "SELECT uuid,role_uuid,permission_uuid,revoked_at_ms "
            "FROM role_permissions WHERE role_uuid=?"),
            QStringLiteral("uuid"), QStringLiteral("revoked_at_ms"), options.include_inactive),
            {this->backend_->bindUuid(role_uuid)}, options, error))
        return records;
    while (query.next())
        records.append(RolePermissionGrantInfo{
            this->backend_->readUuid(query.value(0)),
            this->backend_->readUuid(query.value(1)),
            this->backend_->readUuid(query.value(2)),
            !query.value(3).isNull()});
    return records;
}

QVector<RoleAssignmentInfo> Database::securityRoleAssignments(const OperationContext &actor,
                                                                PrincipalKind kind,
                                                                const QUuid &principal_uuid,
                                                                const SecurityListOptions &options,
                                                                QString *error) const
{
    QVector<RoleAssignmentInfo> records;
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return records;
    if (principal_uuid.isNull())
    {
        setError(error, QStringLiteral("Invalid principal UUID"));
        return records;
    }
    QSqlQuery query(this->database_);
    if (!selectPage(query, *this->backend_, pagedSql(QStringLiteral(
            "SELECT uuid,principal_kind,principal_uuid,role_uuid,scope_kind,scope_uuid,"
            "created_by_uuid,created_at_ms,revoked_at_ms FROM role_assignments "
            "WHERE principal_kind=? AND principal_uuid=?"),
            QStringLiteral("uuid"), QStringLiteral("revoked_at_ms"), options.include_inactive),
            {QVariant(kind == PrincipalKind::User ? QStringLiteral("user") : QStringLiteral("group")),
             this->backend_->bindUuid(principal_uuid)}, options, error))
        return records;
    while (query.next())
    {
        const QString scope_kind = query.value(4).toString();
        AccessScope scope = AccessScope::system();
        if (scope_kind == QStringLiteral("project"))
            scope = AccessScope::project(this->backend_->readUuid(query.value(5)));
        else if (scope_kind == QStringLiteral("branch"))
            scope = AccessScope::branch(this->backend_->readUuid(query.value(5)));
        records.append(RoleAssignmentInfo{
            this->backend_->readUuid(query.value(0)), kind,
            this->backend_->readUuid(query.value(2)),
            this->backend_->readUuid(query.value(3)), scope,
            this->backend_->readUuid(query.value(6)),
            query.value(7).toLongLong(), !query.value(8).isNull()});
    }
    return records;
}

} // namespace aowis::db
