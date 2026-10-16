#ifndef AOWIS_DB_IDENTITY_H
#define AOWIS_DB_IDENTITY_H

#include <QString>
#include <QtGlobal>
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
    bool disabled = false;
};

struct GroupInfo
{
    QUuid uuid;
    QString name;
    bool disabled = false;
};

struct RoleInfo
{
    QUuid uuid;
    QString key;
    bool builtin = false;
};

struct PermissionInfo
{
    QUuid uuid;
    QString key;
    bool builtin = false;
};

// UUIDv7 order makes UUID keyset pagination efficient and stable even while
// new identities and grants are created. A page contains at most 200 entries.
struct SecurityListOptions
{
    QUuid after_uuid;
    int limit = 100;
    bool include_inactive = false;
};

struct GroupMembershipInfo
{
    QUuid uuid;
    QUuid group_uuid;
    QUuid user_uuid;
    QUuid created_by_uuid;
    qint64 created_at_ms = 0;
    bool revoked = false;
};

struct RolePermissionGrantInfo
{
    QUuid uuid;
    QUuid role_uuid;
    QUuid permission_uuid;
    bool revoked = false;
};

struct RoleAssignmentInfo
{
    QUuid uuid;
    PrincipalKind principal_kind = PrincipalKind::User;
    QUuid principal_uuid;
    QUuid role_uuid;
    AccessScope scope;
    QUuid created_by_uuid;
    qint64 created_at_ms = 0;
    bool revoked = false;
};

} // namespace aowis::db

#endif // AOWIS_DB_IDENTITY_H
