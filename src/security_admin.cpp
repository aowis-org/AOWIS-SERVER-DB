#include <aowis/db/database.h>

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

bool hasColumn(const QSqlDatabase &database, const QString &table,
               const QString &column, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
    {
        setError(error, query.lastError().text());
        return false;
    }
    while (query.next())
    {
        if (query.value(1).toString() == column)
            return true;
    }
    return false;
}

// Revoke rows rather than deleting them. Re-granting creates another UUID;
// historical grants remain identifiable, including for security auditing.
const char *const lifecycle_sql[] = {
    R"SQL(CREATE TABLE group_memberships (
        uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
        group_uuid BLOB NOT NULL REFERENCES groups(uuid) ON DELETE RESTRICT,
        user_uuid BLOB NOT NULL REFERENCES users(uuid) ON DELETE RESTRICT,
        created_by_uuid BLOB NOT NULL REFERENCES users(uuid),
        created_at_ms INTEGER NOT NULL,
        revoked_at_ms INTEGER,
        revoked_by_uuid BLOB REFERENCES users(uuid),
        CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
    ))SQL",
    R"SQL(CREATE UNIQUE INDEX group_membership_active_unique
        ON group_memberships(group_uuid,user_uuid) WHERE revoked_at_ms IS NULL)SQL",
    R"SQL(CREATE TABLE role_permissions (
        uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
        role_uuid BLOB NOT NULL REFERENCES roles(uuid) ON DELETE RESTRICT,
        permission_uuid BLOB NOT NULL REFERENCES permissions(uuid) ON DELETE RESTRICT,
        revoked_at_ms INTEGER,
        revoked_by_uuid BLOB REFERENCES users(uuid),
        CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
    ))SQL",
    R"SQL(CREATE UNIQUE INDEX role_permission_active_unique
        ON role_permissions(role_uuid,permission_uuid) WHERE revoked_at_ms IS NULL)SQL",
    R"SQL(CREATE TABLE role_assignments (
        uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
        principal_kind TEXT NOT NULL CHECK(principal_kind IN ('user','group')),
        principal_uuid BLOB NOT NULL CHECK(length(principal_uuid)=16),
        role_uuid BLOB NOT NULL REFERENCES roles(uuid) ON DELETE RESTRICT,
        scope_kind TEXT NOT NULL CHECK(scope_kind IN ('system','project','branch')),
        scope_uuid BLOB CHECK(scope_uuid IS NULL OR length(scope_uuid)=16),
        created_by_uuid BLOB NOT NULL REFERENCES users(uuid),
        created_at_ms INTEGER NOT NULL,
        revoked_at_ms INTEGER,
        revoked_by_uuid BLOB REFERENCES users(uuid),
        CHECK((scope_kind='system' AND scope_uuid IS NULL) OR
              (scope_kind IN ('project','branch') AND scope_uuid IS NOT NULL)),
        CHECK((revoked_at_ms IS NULL AND revoked_by_uuid IS NULL) OR
              (revoked_at_ms IS NOT NULL AND revoked_by_uuid IS NOT NULL))
    ))SQL",
    R"SQL(CREATE UNIQUE INDEX role_assignment_active_unique ON role_assignments (
        principal_kind,principal_uuid,role_uuid,scope_kind,COALESCE(hex(scope_uuid),''))
        WHERE revoked_at_ms IS NULL)SQL",
    R"SQL(CREATE TRIGGER role_assignment_valid BEFORE INSERT ON role_assignments
        WHEN NOT (
            (NEW.principal_kind='user' AND EXISTS(SELECT 1 FROM users WHERE uuid=NEW.principal_uuid)) OR
            (NEW.principal_kind='group' AND EXISTS(SELECT 1 FROM groups WHERE uuid=NEW.principal_uuid)))
          OR NOT (
            NEW.scope_kind='system' OR
            (NEW.scope_kind='project' AND EXISTS(SELECT 1 FROM projects WHERE uuid=NEW.scope_uuid)) OR
            (NEW.scope_kind='branch' AND EXISTS(SELECT 1 FROM branches WHERE uuid=NEW.scope_uuid)))
        BEGIN SELECT RAISE(ABORT,'invalid principal or scope'); END)SQL",
    R"SQL(CREATE TRIGGER group_membership_revoke_only BEFORE UPDATE ON group_memberships
        WHEN NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NEW.uuid AND OLD.group_uuid IS NEW.group_uuid
              AND OLD.user_uuid IS NEW.user_uuid AND OLD.created_by_uuid IS NEW.created_by_uuid
              AND OLD.created_at_ms IS NEW.created_at_ms)
        BEGIN SELECT RAISE(ABORT,'membership can only be revoked once'); END)SQL",
    R"SQL(CREATE TRIGGER group_membership_no_delete BEFORE DELETE ON group_memberships
        BEGIN SELECT RAISE(ABORT,'membership history cannot be deleted'); END)SQL",
    R"SQL(CREATE TRIGGER role_permission_revoke_only BEFORE UPDATE ON role_permissions
        WHEN NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NEW.uuid AND OLD.role_uuid IS NEW.role_uuid
              AND OLD.permission_uuid IS NEW.permission_uuid)
        BEGIN SELECT RAISE(ABORT,'role permission can only be revoked once'); END)SQL",
    R"SQL(CREATE TRIGGER role_permission_no_delete BEFORE DELETE ON role_permissions
        BEGIN SELECT RAISE(ABORT,'role permission history cannot be deleted'); END)SQL",
    R"SQL(CREATE TRIGGER role_assignment_revoke_only BEFORE UPDATE ON role_assignments
        WHEN NOT (OLD.revoked_at_ms IS NULL AND NEW.revoked_at_ms IS NOT NULL
              AND NEW.revoked_by_uuid IS NOT NULL
              AND OLD.uuid IS NEW.uuid AND OLD.principal_kind IS NEW.principal_kind
              AND OLD.principal_uuid IS NEW.principal_uuid AND OLD.role_uuid IS NEW.role_uuid
              AND OLD.scope_kind IS NEW.scope_kind AND OLD.scope_uuid IS NEW.scope_uuid
              AND OLD.created_by_uuid IS NEW.created_by_uuid
              AND OLD.created_at_ms IS NEW.created_at_ms)
        BEGIN SELECT RAISE(ABORT,'role assignment can only be revoked once'); END)SQL",
    R"SQL(CREATE TRIGGER role_assignment_no_delete BEFORE DELETE ON role_assignments
        BEGIN SELECT RAISE(ABORT,'role assignment history cannot be deleted'); END)SQL"
};

} // namespace

bool Database::migrateSecurityAdminV10(QString *error)
{
    if (!beginWrite(error))
        return false;

    // The V9 test fixtures sometimes downgrade user_version while preserving
    // existing newer tables. Avoid rewriting an already upgraded catalog.
    const bool membership_upgraded = hasColumn(this->database_, QStringLiteral("group_memberships"),
                                               QStringLiteral("revoked_at_ms"), error);
    const bool permission_upgraded = hasColumn(this->database_, QStringLiteral("role_permissions"),
                                               QStringLiteral("revoked_at_ms"), error);
    const bool assignment_upgraded = hasColumn(this->database_, QStringLiteral("role_assignments"),
                                               QStringLiteral("revoked_at_ms"), error);
    if (membership_upgraded || permission_upgraded || assignment_upgraded)
    {
        if (!(membership_upgraded && permission_upgraded && assignment_upgraded))
        {
            setError(error, QStringLiteral("Partially upgraded security-grant tables"));
            rollback();
            return false;
        }
        // Older-version migration fixtures can preserve the V10 table layout
        // while removing newer triggers. Restore the V10 safeguards as well.
        for (int i = 1; i < 13; ++i)
        {
            if (i == 2 || i == 4)
                continue; // Skip CREATE TABLE role_permissions and role_assignments.
            if (!exec(QString::fromUtf8(lifecycle_sql[i]).replace(
                    QStringLiteral("CREATE TRIGGER "), QStringLiteral("CREATE TRIGGER IF NOT EXISTS "))
                    .replace(QStringLiteral("CREATE UNIQUE INDEX "),
                             QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS ")), error))
            {
                rollback();
                return false;
            }
        }
        if (!exec(QStringLiteral("PRAGMA user_version = 10"), error) || !commit(error))
        {
            rollback();
            return false;
        }
        return true;
    }

    const char *const rename_sql[] = {
        "DROP TRIGGER role_assignment_no_update",
        "DROP TRIGGER role_assignment_valid",
        "ALTER TABLE group_memberships RENAME TO old_group_memberships_v9",
        "ALTER TABLE role_permissions RENAME TO old_role_permissions_v9",
        "ALTER TABLE role_assignments RENAME TO old_role_assignments_v9"
    };
    for (const char *statement : rename_sql)
    {
        if (!exec(QString::fromLatin1(statement), error))
        {
            rollback();
            return false;
        }
    }
    for (const char *statement : lifecycle_sql)
    {
        if (!exec(QString::fromUtf8(statement), error))
        {
            rollback();
            return false;
        }
    }
    const char *const copy_sql[] = {
        "INSERT INTO group_memberships(uuid,group_uuid,user_uuid,created_by_uuid,created_at_ms) "
        "SELECT uuid,group_uuid,user_uuid,created_by_uuid,created_at_ms FROM old_group_memberships_v9",
        "INSERT INTO role_permissions(uuid,role_uuid,permission_uuid) "
        "SELECT uuid,role_uuid,permission_uuid FROM old_role_permissions_v9",
        "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,scope_kind,scope_uuid,created_by_uuid,created_at_ms) "
        "SELECT uuid,principal_kind,principal_uuid,role_uuid,scope_kind,scope_uuid,created_by_uuid,created_at_ms "
        "FROM old_role_assignments_v9",
        "DROP TABLE old_group_memberships_v9",
        "DROP TABLE old_role_permissions_v9",
        "DROP TABLE old_role_assignments_v9"
    };
    for (const char *statement : copy_sql)
    {
        if (!exec(QString::fromLatin1(statement), error))
        {
            rollback();
            return false;
        }
    }
    if (!exec(QStringLiteral("PRAGMA user_version = 10"), error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

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
            query.addBindValue(uuidBytes(actor.actor_uuid));
            query.addBindValue(uuidBytes(membership_uuid));
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
            query.addBindValue(uuidBytes(actor.actor_uuid));
            query.addBindValue(uuidBytes(grant_uuid));
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
            query.addBindValue(uuidBytes(actor.actor_uuid));
            query.addBindValue(uuidBytes(assignment_uuid));
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
            query.addBindValue(uuidBytes(user_uuid));
            if (!query.exec() || query.numRowsAffected() != 1)
            {
                setError(error, QStringLiteral("User not found or already active"));
                return false;
            }
            return true;
        }, error);
}

} // namespace aowis::db
