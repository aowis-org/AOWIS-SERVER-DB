#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

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
    {"security.audit.read", "01a14202-2800-731d-a688-db84f1d900b5"}
};

QUuid pinnedUuid(const char *uuid)
{
    return QUuid(QStringLiteral("{%1}").arg(QString::fromLatin1(uuid)));
}

const QUuid system_admin_role(QStringLiteral("{01a14202-2800-740c-8c71-cc7d7701e6d2}"));
const QUuid project_owner_role(QStringLiteral("{01a14202-2800-7b7a-92fc-435bca39b6fd}"));

} // namespace

bool Database::migrateSecurityV2(QString *error)
{
    if (!beginWrite(error))
        return false;

    // All v1 hashes remain in their original format. Existing rows have an
    // unknown actor (NULL); never invent authorship for historical records.
    const char *const statements[] = {
        R"SQL(CREATE TABLE users (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            username TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(username)>0),
            created_at_ms INTEGER NOT NULL,
            disabled_at_ms INTEGER
        ))SQL",
        R"SQL(CREATE TABLE groups (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            name TEXT NOT NULL COLLATE NOCASE UNIQUE CHECK(length(name)>0),
            created_at_ms INTEGER NOT NULL,
            disabled_at_ms INTEGER
        ))SQL",
        R"SQL(CREATE TABLE roles (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            key TEXT NOT NULL UNIQUE,
            builtin INTEGER NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))
        ))SQL",
        R"SQL(CREATE TABLE permissions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            key TEXT NOT NULL UNIQUE,
            builtin INTEGER NOT NULL DEFAULT 0 CHECK(builtin IN (0,1))
        ))SQL",
        R"SQL(CREATE TABLE group_memberships (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            group_uuid BLOB NOT NULL REFERENCES groups(uuid) ON DELETE RESTRICT,
            user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
            created_by_uuid BLOB NOT NULL REFERENCES users(uuid),
            created_at_ms INTEGER NOT NULL,
            UNIQUE(group_uuid,user_uuid)
        ))SQL",
        R"SQL(CREATE TABLE role_permissions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            role_uuid BLOB NOT NULL REFERENCES roles(uuid) ON DELETE RESTRICT,
            permission_uuid BLOB NOT NULL REFERENCES permissions(uuid) ON DELETE RESTRICT,
            UNIQUE(role_uuid,permission_uuid)
        ))SQL",
        R"SQL(CREATE TABLE role_assignments (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            principal_kind TEXT NOT NULL CHECK(principal_kind IN ('user','group')),
            principal_uuid BLOB NOT NULL CHECK(length(principal_uuid)=16),
            role_uuid BLOB NOT NULL REFERENCES roles(uuid) ON DELETE RESTRICT,
            scope_kind TEXT NOT NULL CHECK(scope_kind IN ('system','project','branch')),
            scope_uuid BLOB CHECK(scope_uuid IS NULL OR length(scope_uuid)=16),
            created_by_uuid BLOB NOT NULL REFERENCES users(uuid),
            created_at_ms INTEGER NOT NULL,
            CHECK((scope_kind='system' AND scope_uuid IS NULL) OR
                  (scope_kind IN ('project','branch') AND scope_uuid IS NOT NULL))
        ))SQL",
        R"SQL(CREATE UNIQUE INDEX role_assignment_unique ON role_assignments (
            principal_kind,principal_uuid,role_uuid,scope_kind,COALESCE(hex(scope_uuid),'')))SQL",
        R"SQL(CREATE TRIGGER role_assignment_valid BEFORE INSERT ON role_assignments
            WHEN NOT (
                (NEW.principal_kind='user' AND EXISTS(SELECT 1 FROM users WHERE uuid=NEW.principal_uuid)) OR
                (NEW.principal_kind='group' AND EXISTS(SELECT 1 FROM groups WHERE uuid=NEW.principal_uuid)))
              OR NOT (
                NEW.scope_kind='system' OR
                (NEW.scope_kind='project' AND EXISTS(SELECT 1 FROM projects WHERE uuid=NEW.scope_uuid)) OR
                (NEW.scope_kind='branch' AND EXISTS(SELECT 1 FROM branches WHERE uuid=NEW.scope_uuid)))
            BEGIN SELECT RAISE(ABORT,'invalid principal or scope'); END)SQL",
        R"SQL(CREATE TRIGGER role_assignment_no_update BEFORE UPDATE ON role_assignments
            BEGIN SELECT RAISE(ABORT,'role assignment is immutable; revoke then reassign'); END)SQL",
        R"SQL(ALTER TABLE projects ADD COLUMN created_by_uuid BLOB REFERENCES users(uuid))SQL",
        R"SQL(ALTER TABLE branches ADD COLUMN created_by_uuid BLOB REFERENCES users(uuid))SQL",
        R"SQL(ALTER TABLE revisions ADD COLUMN created_by_uuid BLOB REFERENCES users(uuid))SQL",
        R"SQL(ALTER TABLE revisions ADD COLUMN finalized_by_uuid BLOB REFERENCES users(uuid))SQL",
        R"SQL(ALTER TABLE revisions ADD COLUMN hash_format_version INTEGER NOT NULL DEFAULT 1
            CHECK(hash_format_version IN (1,2)))SQL",
        R"SQL(CREATE TRIGGER project_creator_required BEFORE INSERT ON projects
            WHEN NEW.created_by_uuid IS NULL OR length(NEW.created_by_uuid)!=16
            BEGIN SELECT RAISE(ABORT,'project creator required'); END)SQL",
        R"SQL(CREATE TRIGGER branch_creator_required BEFORE INSERT ON branches
            WHEN NEW.created_by_uuid IS NULL OR length(NEW.created_by_uuid)!=16
            BEGIN SELECT RAISE(ABORT,'branch creator required'); END)SQL",
        R"SQL(CREATE TRIGGER revision_creator_required BEFORE INSERT ON revisions
            WHEN NEW.created_by_uuid IS NULL OR length(NEW.created_by_uuid)!=16
            BEGIN SELECT RAISE(ABORT,'revision creator required'); END)SQL",
        R"SQL(CREATE TRIGGER finalize_actor_required BEFORE UPDATE OF state ON revisions
            WHEN NEW.state='finalized' AND (NEW.hash_format_version!=2 OR
                NEW.finalized_by_uuid IS NULL OR length(NEW.finalized_by_uuid)!=16)
            BEGIN SELECT RAISE(ABORT,'V2 finalization actor required'); END)SQL"
    };

    for (const char *statement : statements)
    {
        if (!exec(QString::fromUtf8(statement), error))
        {
            rollback();
            return false;
        }
    }

    QSqlQuery query(this->database_);
    bool ok = prepare(query, QStringLiteral(
        "INSERT INTO roles(uuid,key,builtin) VALUES (?,'system.administrator',1)"), error);
    if (ok)
    {
        query.addBindValue(uuidBytes(system_admin_role));
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO roles(uuid,key,builtin) VALUES (?,'project.owner',1)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(project_owner_role));
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
            query.addBindValue(uuidBytes(permission_uuid));
            query.addBindValue(QString::fromLatin1(permission.key));
            ok = run(query, error);
        }
        if (ok)
        {
            ok = prepare(query, QStringLiteral(
                "INSERT INTO role_permissions(uuid,role_uuid,permission_uuid) VALUES (?,?,?)"), error);
            if (ok)
            {
                query.addBindValue(uuidBytes(createUuidV7()));
                query.addBindValue(uuidBytes(system_admin_role));
                query.addBindValue(uuidBytes(pinnedUuid(permission.id)));
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
                query.addBindValue(uuidBytes(createUuidV7()));
                query.addBindValue(uuidBytes(project_owner_role));
                query.addBindValue(uuidBytes(pinnedUuid(permission.id)));
                ok = run(query, error);
            }
        }
    }
    if (ok)
        ok = exec(QStringLiteral("PRAGMA user_version = 2"), error) && commit(error);
    if (!ok)
        rollback();
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
        query.addBindValue(uuidBytes(user.uuid));
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
            query.addBindValue(uuidBytes(createUuidV7()));
            query.addBindValue(uuidBytes(user.uuid));
            query.addBindValue(uuidBytes(system_admin_role));
            query.addBindValue(uuidBytes(user.uuid));
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            ok = run(query, error);
        }
    }
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
                WHERE m.user_uuid=u.uuid AND m.group_uuid=a.principal_uuid AND g.disabled_at_ms IS NULL)))
        JOIN role_permissions rp ON rp.role_uuid=a.role_uuid
        JOIN permissions p ON p.uuid=rp.permission_uuid
        WHERE u.uuid=? AND u.disabled_at_ms IS NULL AND p.key=? AND (
            a.scope_kind='system' OR
            (a.scope_kind='project' AND a.scope_uuid=?) OR
            (a.scope_kind='branch' AND a.scope_uuid=?)) LIMIT 1
    )SQL"), error))
        return false;
    query.addBindValue(uuidBytes(actor.actor_uuid));
    query.addBindValue(permission_key);
    query.addBindValue(scope.kind == AccessScopeKind::System ? QByteArray() : uuidBytes(project_uuid));
    query.addBindValue(scope.kind == AccessScopeKind::Branch ? uuidBytes(scope.resource_uuid) : QByteArray());
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
    query.addBindValue(uuidBytes(result.uuid));
    query.addBindValue(result.username);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return run(query, error) ? std::optional<UserInfo>(result) : std::nullopt;
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
    query.addBindValue(uuidBytes(result.uuid));
    query.addBindValue(result.name);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return run(query, error) ? std::optional<GroupInfo>(result) : std::nullopt;
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
    query.addBindValue(uuidBytes(result.uuid));
    query.addBindValue(result.key);
    return run(query, error) ? std::optional<RoleInfo>(result) : std::nullopt;
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
    query.addBindValue(uuidBytes(result.uuid));
    query.addBindValue(result.key);
    return run(query, error) ? std::optional<PermissionInfo>(result) : std::nullopt;
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
    return RoleInfo{QUuid::fromRfc4122(query.value(0).toByteArray()), query.value(1).toString()};
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
    return PermissionInfo{QUuid::fromRfc4122(query.value(0).toByteArray()), query.value(1).toString()};
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
    query.addBindValue(uuidBytes(uuid));
    query.addBindValue(uuidBytes(group_uuid));
    query.addBindValue(uuidBytes(user_uuid));
    query.addBindValue(uuidBytes(actor.actor_uuid));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return run(query, error) ? std::optional<QUuid>(uuid) : std::nullopt;
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
    query.addBindValue(uuidBytes(uuid));
    query.addBindValue(uuidBytes(role_uuid));
    query.addBindValue(uuidBytes(permission_uuid));
    return run(query, error) ? std::optional<QUuid>(uuid) : std::nullopt;
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
    query.addBindValue(uuidBytes(uuid));
    query.addBindValue(kind == PrincipalKind::User ? QStringLiteral("user") : QStringLiteral("group"));
    query.addBindValue(uuidBytes(principal_uuid));
    query.addBindValue(uuidBytes(role_uuid));
    query.addBindValue(scopeKey(scope.kind));
    query.addBindValue(scope.kind == AccessScopeKind::System ? QVariant() : QVariant(uuidBytes(scope.resource_uuid)));
    query.addBindValue(uuidBytes(actor.actor_uuid));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return run(query, error) ? std::optional<QUuid>(uuid) : std::nullopt;
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
