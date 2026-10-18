#include "sqlite_backend.h"

#include <QSqlError>
#include <QSqlQuery>

namespace aowis::db::sqlite_backend
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

bool exec(const QSqlDatabase &database, const QString &statement, QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement))
        return true;
    setError(error, query.lastError().text());
    return false;
}
}

bool open(QSqlDatabase &database, const QString &connection_name,
          const QString &file_path, QString *error)
{
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
    database.setDatabaseName(file_path);
    if (database.open())
        return true;
    setError(error, database.lastError().text());
    return false;
}

bool configure(const QSqlDatabase &database, QString *error)
{
    return exec(database, QStringLiteral("PRAGMA foreign_keys = ON"), error) &&
           exec(database, QStringLiteral("PRAGMA busy_timeout = 5000"), error) &&
           exec(database, QStringLiteral("PRAGMA journal_mode = WAL"), error);
}

bool verifySchema(const QSqlDatabase &database, QString *error)
{
    // A schema version can be set even when a database has been damaged or
    // incompletely upgraded. Do not silently accept a V9 database without its
    // security audit table and immutability triggers.
    QString audit_schema_error;
    {
        QSqlQuery audit_schema(database);
        if (!audit_schema.exec(QStringLiteral(
                "SELECT sequence,uuid,actor_uuid,subject_uuid,action,occurred_at_ms,"
                "previous_sha256,chain_sha256 FROM security_audit_events LIMIT 0")))
        {
            audit_schema_error = QStringLiteral("SQLite V9 audit schema is missing or incomplete: %1")
                                     .arg(audit_schema.lastError().text());
        }
        else
        {
            audit_schema.finish();
            if (!audit_schema.exec(QStringLiteral(
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN "
                    "('security_audit_readonly_update','security_audit_readonly_delete')")) ||
                !audit_schema.next() || audit_schema.value(0).toInt() != 2)
            {
                audit_schema_error = QStringLiteral(
                    "SQLite V9 security audit immutability triggers are missing");
            }
        }
    }
    if (!audit_schema_error.isEmpty())
    {
        setError(error, audit_schema_error);
        return false;
    }
    // Without these V10 authorization columns and guards, a damaged database
    // could restore previously revoked access.
    QSqlQuery grant_schema(database);
    const bool valid_columns = grant_schema.exec(QStringLiteral(
        "SELECT m.revoked_at_ms,m.revoked_by_uuid,"
        "p.revoked_at_ms,p.revoked_by_uuid,a.revoked_at_ms,a.revoked_by_uuid "
        "FROM group_memberships m,role_permissions p,role_assignments a LIMIT 0"));
    grant_schema.finish();
    const bool valid_guards = grant_schema.exec(QStringLiteral(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
        "'group_membership_revoke_only','group_membership_no_delete',"
        "'role_permission_revoke_only','role_permission_no_delete',"
        "'role_assignment_revoke_only','role_assignment_no_delete','role_assignment_valid')")) &&
        grant_schema.next() && grant_schema.value(0).toInt() == 7;
    if (!valid_columns || !valid_guards)
    {
        setError(error, QStringLiteral("SQLite V10 security grant schema is incomplete"));
        return false;
    }
    // Fail closed when a database claims V11 but credentials were omitted.
    // Keep this query finished before later schema DDL on this connection.
    QSqlQuery credentials_schema(database);
    if (!credentials_schema.exec(QStringLiteral(
            "SELECT user_uuid,salt,verifier,iterations,changed_by_uuid,failed_attempts,locked_until_ms "
            "FROM user_password_credentials LIMIT 0")))
    {
        setError(error, QStringLiteral("SQLite V11 password credential schema is incomplete"));
        return false;
    }
    credentials_schema.finish();
    QSqlQuery recovery_schema(database);
    if (!recovery_schema.exec(QStringLiteral(
            "SELECT uuid,user_uuid,token_sha256,issued_by_uuid,expires_at_ms,used_at_ms,invalidated_at_ms "
            "FROM password_recovery_tokens LIMIT 0")))
    {
        setError(error, QStringLiteral("SQLite V12 password recovery schema is incomplete"));
        return false;
    }
    recovery_schema.finish();
    QSqlQuery recovery_guards(database);
    if (!recovery_guards.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
            "'recovery_tokens_no_delete','recovery_tokens_close_only')")) ||
        !recovery_guards.next() || recovery_guards.value(0).toInt() != 2)
    {
        setError(error, QStringLiteral("SQLite V12 recovery token immutability guards are missing"));
        return false;
    }
    recovery_guards.finish();
    // Avoid silently accepting an incomplete V13 database: redaction must be
    // one-way and protected by its migrated trigger.
    QSqlQuery redaction_guard(database);
    if (!redaction_guard.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='trigger' "
            "AND name='user_sessions_immutable_identity_v13' LIMIT 1")) ||
        !redaction_guard.next())
    {
        setError(error, QStringLiteral("SQLite V13 session-redaction guard is missing"));
        return false;
    }
    redaction_guard.finish();
    return true;
}

std::optional<int> schemaVersion(const QSqlDatabase &database, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    const int version = query.value(0).toInt();
    // Finish the cursor before migration DDL; open PRAGMA cursors can block
    // table replacement and trigger SQLITE_LOCKED.
    query.finish();
    return version;
}

bool setSchemaVersion(const QSqlDatabase &database, int version, QString *error)
{
    if (version < 1 || version > current_schema_version)
    {
        setError(error, QStringLiteral("Invalid SQLite schema version"));
        return false;
    }
    // SQLite PRAGMAs cannot use query parameters; the integer is bounded.
    return exec(database, QStringLiteral("PRAGMA user_version = %1").arg(version), error);
}

bool beginWrite(const QSqlDatabase &database, QString *error)
{
    // BEGIN IMMEDIATE reserves the single SQLite writer before any edits.
    return exec(database, QStringLiteral("BEGIN IMMEDIATE"), error);
}

bool commit(const QSqlDatabase &database, QString *error)
{
    return exec(database, QStringLiteral("COMMIT"), error);
}

void rollback(const QSqlDatabase &database)
{
    exec(database, QStringLiteral("ROLLBACK"), nullptr);
}

} // namespace aowis::db::sqlite_backend
