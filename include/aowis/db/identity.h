#ifndef AOWIS_DB_IDENTITY_H
#define AOWIS_DB_IDENTITY_H

#include <QString>
#include <QUuid>

namespace aowis::db
{

// A trusted host constructs this from an authenticated identity. This type
// is NOT a credential; future HTTP clients must never supply it directly.
struct OperationContext
{
    QUuid actor_uuid;
};

enum class AccessScopeKind
{
    System,
    Project,
    Branch
};

struct AccessScope
{
    AccessScopeKind kind = AccessScopeKind::System;
    QUuid resource_uuid;

    static AccessScope system() { return {}; }
    static AccessScope project(const QUuid &uuid) { return {AccessScopeKind::Project, uuid}; }
    static AccessScope branch(const QUuid &uuid) { return {AccessScopeKind::Branch, uuid}; }
};

enum class PrincipalKind
{
    User,
    Group
};

struct UserInfo
{
    QUuid uuid;
    QString username;
};

struct GroupInfo
{
    QUuid uuid;
    QString name;
};

struct RoleInfo
{
    QUuid uuid;
    QString key;
};

struct PermissionInfo
{
    QUuid uuid;
    QString key;
};

} // namespace aowis::db

#endif // AOWIS_DB_IDENTITY_H
