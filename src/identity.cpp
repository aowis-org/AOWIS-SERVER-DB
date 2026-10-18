#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include "storage_backend.h"

#include "builtin_identity_ids.h"

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

bool run(QSqlQuery &query, QString *error)
{
    if (query.exec())
        return true;
    setError(error, query.lastError().text());
    return false;
}

bool prepare(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.prepare(sql))
        return true;
    setError(error, query.lastError().text());
    return false;
}

bool validKey(const QString &key)
{
    if (key.isEmpty() || key.size() > 128)
        return false;
    for (const QChar character : key)
    {
        const ushort code = character.unicode();
        if ((code < 'a' || code > 'z') && (code < '0' || code > '9') &&
            code != '.' && code != '_' && code != '-')
            return false;
    }
    return true;
}

bool validName(const QString &name)
{
    return !name.trimmed().isEmpty() && name.size() <= 256;
}

QString scopeKey(AccessScopeKind kind)
{
    switch (kind)
    {
    case AccessScopeKind::System: return QStringLiteral("system");
    case AccessScopeKind::Project: return QStringLiteral("project");
    case AccessScopeKind::Branch: return QStringLiteral("branch");
    }
    return {};
}

struct BuiltinPermission
{
    const char *key;
    const char *id;
};

constexpr BuiltinPermission permissions[] = {
    {"security.manage", "01a14202-2800-733c-a60e-74b4379e146a"},
    {"project.create", "01a14202-2800-7275-9a57-9d17cca8cdc5"},
    {"project.read", "01a14202-2800-712a-81ad-be2a0fa8aa02"},
    {"project.branch.create", "01a14202-2800-7290-89d5-97871af6d0ae"},
    {"project.model.write", "01a14202-2800-731d-89ee-2eed49ffcfce"},
    {"project.revision.finalize", "01a14202-2800-7fdc-a21e-938f38efa7e5"},
    {"project.permissions.manage", "01a14202-2800-7988-a929-46a425533b5e"},
    {"project.history.read", "01a14202-2800-71d0-9e6c-c95cd0afc981"},
    {"security.audit.read", internal::security_audit_read_permission_uuid}
};

QUuid pinnedUuid(const char *uuid)
{
    return QUuid(QStringLiteral("{%1}").arg(QString::fromLatin1(uuid)));
}

const QUuid system_admin_role(QStringLiteral("{01a14202-2800-740c-8c71-cc7d7701e6d2}"));
const QUuid project_owner_role(QStringLiteral("{01a14202-2800-7b7a-92fc-435bca39b6fd}"));

} // namespace

bool Database::seedBuiltinSecurity(QString *error)
{
    // Called only while the newly-created schema is still in its transaction.
    // All built-in permissions and grants exist from the very first open.
    QSqlQuery query(this->database_);
    bool ok = prepare(query, QStringLiteral(
        "INSERT INTO roles(uuid,key,builtin) VALUES (?,'system.administrator',1)"), error);
    if (ok)
    {
        query.addBindValue(this->backend_->bindUuid(system_admin_role));
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO roles(uuid,key,builtin) VALUES (?,'project.owner',1)"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(project_owner_role));
            ok = run(query, error);
        }
    }
    for (const BuiltinPermission &permission : permissions)
    {
        if (!ok)
            break;
        ok = prepare(query, QStringLiteral("INSERT INTO permissions(uuid,key,builtin) VALUES (?,?,1)"), error);
        if (ok)
        {
            const QUuid permission_uuid(pinnedUuid(permission.id));
            query.addBindValue(this->backend_->bindUuid(permission_uuid));
            query.addBindValue(QString::fromLatin1(permission.key));
            ok = run(query, error);
        }
        if (ok)
        {
            ok = prepare(query, QStringLiteral(
                "INSERT INTO role_permissions(uuid,role_uuid,permission_uuid) VALUES (?,?,?)"), error);
            if (ok)
            {
                query.addBindValue(this->backend_->bindUuid(createUuidV7()));
                query.addBindValue(this->backend_->bindUuid(system_admin_role));
                query.addBindValue(this->backend_->bindUuid(pinnedUuid(permission.id)));
                ok = run(query, error);
            }
        }
        const QString key = QString::fromLatin1(permission.key);
        const bool project_scoped = key.startsWith(QStringLiteral("project.")) &&
                                    key != QStringLiteral("project.create");
        if (ok && project_scoped)
        {
            ok = prepare(query, QStringLiteral(
                "INSERT INTO role_permissions(uuid,role_uuid,permission_uuid) VALUES (?,?,?)"), error);
            if (ok)
            {
                query.addBindValue(this->backend_->bindUuid(createUuidV7()));
                query.addBindValue(this->backend_->bindUuid(project_owner_role));
                query.addBindValue(this->backend_->bindUuid(pinnedUuid(permission.id)));
                ok = run(query, error);
            }
        }
    }
    return ok;
}

std::optional<UserInfo> Database::bootstrapAdministrator(const QString &username, QString *error)
{
    if (!validName(username))
    {
        setError(error, QStringLiteral("Invalid administrator username"));
        return std::nullopt;
    }
    if (!beginWrite(error))
        return std::nullopt;

    QSqlQuery query(this->database_);
    bool ok = query.exec(QStringLiteral("SELECT COUNT(*) FROM users")) && query.next();
    if (!ok || query.value(0).toLongLong() != 0)
    {
        setError(error, QStringLiteral("Administrator bootstrap already completed"));
        rollback();
        return std::nullopt;
    }
    const UserInfo user{createUuidV7(), username.trimmed()};
    ok = prepare(query, QStringLiteral(
        "INSERT INTO users(uuid,username,created_at_ms) VALUES(?,?,?)"), error);
    if (ok)
    {
        query.addBindValue(this->backend_->bindUuid(user.uuid));
        query.addBindValue(user.username);
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,scope_kind,"
            "scope_uuid,created_by_uuid,created_at_ms) VALUES (?,'user',?,?,'system',NULL,?,?)"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(createUuidV7()));
            query.addBindValue(this->backend_->bindUuid(user.uuid));
            query.addBindValue(this->backend_->bindUuid(system_admin_role));
            query.addBindValue(this->backend_->bindUuid(user.uuid));
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            ok = run(query, error);
        }
    }
    if (ok)
        ok = writeSecurityAuditEvent(user.uuid, QStringLiteral("administrator.bootstrap"), user.uuid, error);
    if (ok)
        ok = commit(error);
    if (!ok)
    {
        rollback();
        return std::nullopt;
    }
    return user;
}

bool Database::hasPermission(const OperationContext &actor, const QString &permission_key,
                             const AccessScope &scope, QString *error) const
{
    if (actor.actor_uuid.isNull() || !validKey(permission_key))
        return false;
    if ((scope.kind == AccessScopeKind::System && !scope.resource_uuid.isNull()) ||
        (scope.kind != AccessScopeKind::System && scope.resource_uuid.isNull()))
        return false;

    // The requested branch can inherit project-wide grants. Projects themselves
    // only inherit system grants. A branch grant never authorizes a sibling.
    QUuid project_uuid;
    if (scope.kind == AccessScopeKind::Project)
        project_uuid = scope.resource_uuid;
    else if (scope.kind == AccessScopeKind::Branch)
    {
        const std::optional<BranchInfo> resource = branchUnchecked(scope.resource_uuid, error);
        if (!resource)
            return false;
        project_uuid = resource->project_uuid;
    }

    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(R"SQL(
        SELECT 1 FROM users u
        JOIN role_assignments a ON (
            (a.principal_kind='user' AND a.principal_uuid=u.uuid) OR
            (a.principal_kind='group' AND EXISTS (
                SELECT 1 FROM group_memberships m JOIN groups g ON m.group_uuid=g.uuid
                WHERE m.user_uuid=u.uuid AND m.group_uuid=a.principal_uuid
                  AND m.revoked_at_ms IS NULL AND g.disabled_at_ms IS NULL)))
        JOIN role_permissions rp ON rp.role_uuid=a.role_uuid AND rp.revoked_at_ms IS NULL
        JOIN permissions p ON p.uuid=rp.permission_uuid
        WHERE u.uuid=? AND u.disabled_at_ms IS NULL AND a.revoked_at_ms IS NULL
          AND p.key=? AND (
            a.scope_kind='system' OR
            (a.scope_kind='project' AND a.scope_uuid=?) OR
            (a.scope_kind='branch' AND a.scope_uuid=?)) LIMIT 1
    )SQL"), error))
        return false;
    query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
    query.addBindValue(permission_key);
    // Missing scope UUIDs are SQL NULL, never an empty SQLite BLOB.
    query.addBindValue(scope.kind == AccessScopeKind::System
                           ? QVariant() : this->backend_->bindUuid(project_uuid));
    query.addBindValue(scope.kind == AccessScopeKind::Branch
                           ? this->backend_->bindUuid(scope.resource_uuid) : QVariant());
    if (!run(query, error))
        return false;
    return query.next();
}

bool Database::requirePermission(const OperationContext &actor, const QString &key,
                                 const AccessScope &scope, QString *error) const
{
    if (hasPermission(actor, key, scope, error))
        return true;
    setError(error, QStringLiteral("Permission denied: %1").arg(key));
    return false;
}

std::optional<UserInfo> Database::createUser(const OperationContext &actor, const QString &username, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    if (!validName(username))
    {
        setError(error, QStringLiteral("Invalid username"));
        return std::nullopt;
    }
    const UserInfo result{createUuidV7(), username.trimmed()};
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("INSERT INTO users(uuid,username,created_at_ms) VALUES(?,?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(result.uuid));
    query.addBindValue(result.username);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("user.create"),
        result.uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<UserInfo>(result) : std::nullopt;
}

std::optional<GroupInfo> Database::createGroup(const OperationContext &actor, const QString &name, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    if (!validName(name))
    {
        setError(error, QStringLiteral("Invalid group name"));
        return std::nullopt;
    }
    const GroupInfo result{createUuidV7(), name.trimmed()};
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("INSERT INTO groups(uuid,name,created_at_ms) VALUES(?,?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(result.uuid));
    query.addBindValue(result.name);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("group.create"),
        result.uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<GroupInfo>(result) : std::nullopt;
}

std::optional<RoleInfo> Database::createRole(const OperationContext &actor, const QString &key, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    if (!validKey(key))
    {
        setError(error, QStringLiteral("Invalid role key"));
        return std::nullopt;
    }
    const RoleInfo result{createUuidV7(), key};
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("INSERT INTO roles(uuid,key) VALUES(?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(result.uuid));
    query.addBindValue(result.key);
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("role.create"),
        result.uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<RoleInfo>(result) : std::nullopt;
}

std::optional<PermissionInfo> Database::createPermission(const OperationContext &actor, const QString &key, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    if (!validKey(key))
    {
        setError(error, QStringLiteral("Invalid permission key"));
        return std::nullopt;
    }
    const PermissionInfo result{createUuidV7(), key};
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("INSERT INTO permissions(uuid,key) VALUES(?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(result.uuid));
    query.addBindValue(result.key);
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("permission.create"),
        result.uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<PermissionInfo>(result) : std::nullopt;
}

std::optional<RoleInfo> Database::role(const OperationContext &actor, const QString &key, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("SELECT uuid,key FROM roles WHERE key=?"), error))
        return std::nullopt;
    query.addBindValue(key);
    if (!run(query, error) || !query.next())
        return std::nullopt;
    return RoleInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString()};
}

std::optional<PermissionInfo> Database::permission(const OperationContext &actor, const QString &key, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("SELECT uuid,key FROM permissions WHERE key=?"), error))
        return std::nullopt;
    query.addBindValue(key);
    if (!run(query, error) || !query.next())
        return std::nullopt;
    return PermissionInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString()};
}

std::optional<QUuid> Database::addGroupMember(const OperationContext &actor, const QUuid &group_uuid,
                                              const QUuid &user_uuid, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    const QUuid uuid = createUuidV7();
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
        "INSERT INTO group_memberships(uuid,group_uuid,user_uuid,created_by_uuid,created_at_ms)"
        "VALUES(?,?,?,?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(uuid));
    query.addBindValue(this->backend_->bindUuid(group_uuid));
    query.addBindValue(this->backend_->bindUuid(user_uuid));
    query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("group.member.add"),
        uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<QUuid>(uuid) : std::nullopt;
}

std::optional<QUuid> Database::grantRolePermission(const OperationContext &actor,
                                                   const QUuid &role_uuid,
                                                   const QUuid &permission_uuid, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    const QUuid uuid = createUuidV7();
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
        "INSERT INTO role_permissions(uuid,role_uuid,permission_uuid) VALUES(?,?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(uuid));
    query.addBindValue(this->backend_->bindUuid(role_uuid));
    query.addBindValue(this->backend_->bindUuid(permission_uuid));
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("role.permission.grant"),
        uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<QUuid>(uuid) : std::nullopt;
}

std::optional<QUuid> Database::assignRole(const OperationContext &actor, PrincipalKind kind,
                                          const QUuid &principal_uuid, const QUuid &role_uuid,
                                          const AccessScope &scope, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("security.manage"), AccessScope::system(), error))
        return std::nullopt;
    if ((scope.kind == AccessScopeKind::System && !scope.resource_uuid.isNull()) ||
        (scope.kind != AccessScopeKind::System && scope.resource_uuid.isNull()))
    {
        setError(error, QStringLiteral("Invalid role scope"));
        return std::nullopt;
    }
    const QUuid uuid = createUuidV7();
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
        "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,scope_kind,"
        "scope_uuid,created_by_uuid,created_at_ms) VALUES(?,?,?,?,?,?,?,?)"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(uuid));
    query.addBindValue(kind == PrincipalKind::User ? QStringLiteral("user") : QStringLiteral("group"));
    query.addBindValue(this->backend_->bindUuid(principal_uuid));
    query.addBindValue(this->backend_->bindUuid(role_uuid));
    query.addBindValue(scopeKey(scope.kind));
    query.addBindValue(scope.kind == AccessScopeKind::System ? QVariant() : this->backend_->bindUuid(scope.resource_uuid));
    query.addBindValue(this->backend_->bindUuid(actor.actor_uuid));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    const bool saved = auditedSecurityMutation(actor.actor_uuid, QStringLiteral("role.assign"),
        uuid, [&]() { return run(query, error); }, error);
    return saved ? std::optional<QUuid>(uuid) : std::nullopt;
}

std::optional<ProjectCreation> Database::createProject(const OperationContext &actor,
                                                        const QString &name, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("project.create"), AccessScope::system(), error))
        return std::nullopt;
    return createProjectUnchecked(actor.actor_uuid, name, error);
}

std::optional<ProjectInfo> Database::project(const OperationContext &actor,
                                              const QUuid &uuid, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("project.read"), AccessScope::project(uuid), error))
        return std::nullopt;
    return projectUnchecked(uuid, error);
}

QVector<ProjectInfo> Database::projects(const OperationContext &actor, QString *error) const
{
    QVector<ProjectInfo> result;
    const QVector<ProjectInfo> all = projectsUnchecked(error);
    for (const ProjectInfo &project : all)
    {
        if (hasPermission(actor, QStringLiteral("project.read"), AccessScope::project(project.uuid)))
            result.append(project);
    }
    return result;
}

QVector<BranchInfo> Database::branches(const OperationContext &actor,
                                        const QUuid &project_uuid, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("project.read"), AccessScope::project(project_uuid), error))
        return {};
    return branchesUnchecked(project_uuid, error);
}

QVector<RevisionInfo> Database::revisions(const OperationContext &actor,
                                           const QUuid &branch_uuid, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("project.read"), AccessScope::branch(branch_uuid), error))
        return {};
    return revisionsUnchecked(branch_uuid, error);
}

std::optional<BranchInfo> Database::branch(const OperationContext &actor,
                                           const QUuid &uuid, QString *error) const
{
    if (!requirePermission(actor, QStringLiteral("project.read"), AccessScope::branch(uuid), error))
        return std::nullopt;
    return branchUnchecked(uuid, error);
}

std::optional<RevisionInfo> Database::revision(const OperationContext &actor,
                                               const QUuid &uuid, QString *error) const
{
    const std::optional<RevisionInfo> record = revisionUnchecked(uuid, error);
    if (!record || !requirePermission(actor, QStringLiteral("project.read"),
                                      AccessScope::branch(record->branch_uuid), error))
        return std::nullopt;
    return record;
}

std::optional<BranchInfo> Database::createBranch(const OperationContext &actor,
                                                 const QUuid &project_uuid, const QString &name,
                                                 const QUuid &base_uuid, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("project.branch.create"),
                           AccessScope::project(project_uuid), error))
        return std::nullopt;
    return createBranchUnchecked(actor.actor_uuid, project_uuid, name, base_uuid, error);
}

bool Database::replaceWorkingSnapshot(const OperationContext &actor,
                                      const QUuid &branch_uuid, qint64 generation,
                                      const QByteArray &bytes, QString *error)
{
    if (!requirePermission(actor, QStringLiteral("project.model.write"),
                           AccessScope::branch(branch_uuid), error))
        return false;
    return replaceWorkingSnapshotUnchecked(actor.actor_uuid, branch_uuid, generation, bytes, error);
}

std::optional<QByteArray> Database::snapshot(const OperationContext &actor,
                                             const QUuid &revision_uuid, QString *error) const
{
    const std::optional<RevisionInfo> record = revisionUnchecked(revision_uuid, error);
    if (!record || !requirePermission(actor, QStringLiteral("project.read"),
                                      AccessScope::branch(record->branch_uuid), error))
        return std::nullopt;
    return snapshotUnchecked(revision_uuid, error);
}

std::optional<Finalization> Database::finalize(const OperationContext &actor,
                                                const QUuid &branch_uuid, qint64 generation,
                                                const QString &title, const QString &message,
                                                QString *error)
{
    if (!requirePermission(actor, QStringLiteral("project.revision.finalize"),
                           AccessScope::branch(branch_uuid), error))
        return std::nullopt;
    return finalizeUnchecked(actor.actor_uuid, branch_uuid, generation, title, message, error);
}

IntegrityReport Database::verifyProject(const OperationContext &actor, const QUuid &project_uuid) const
{
    if (!requirePermission(actor, QStringLiteral("project.read"), AccessScope::project(project_uuid), nullptr))
        return {false, 0, {}, QStringLiteral("Permission denied")};
    return verifyProjectUnchecked(project_uuid);
}

} // namespace aowis::db
