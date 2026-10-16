#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aowis::db
{
namespace
{
constexpr int schema_version = 13;
constexpr int digest_length = 32;

QByteArray uuidBytes(const QUuid &uuid)
{
    return uuid.toRfc4122();
}

QUuid readUuid(const QVariant &value)
{
    return QUuid::fromRfc4122(value.toByteArray());
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

bool prepare(QSqlQuery &query, const QString &statement, QString *error)
{
    if (query.prepare(statement))
        return true;
    setError(error, query.lastError().text());
    return false;
}

void appendInt64(QByteArray &bytes, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(QByteArray &bytes, const QByteArray &field)
{
    appendInt64(bytes, static_cast<quint64>(field.size()));
    bytes.append(field);
}

// V1 is explicitly domain-separated and byte-order-defined. Do not change
// this encoding for old finalized revisions; introduce a new hash version.
QByteArray chainDigest(const QUuid &project_uuid,
                       const QUuid &branch_uuid,
                       const QUuid &revision_uuid,
                       const QVector<QPair<QUuid, QByteArray>> &parents,
                       qint64 number,
                       qint64 created_at_ms,
                       qint64 finalized_at_ms,
                       const QString &title,
                       const QString &message,
                       const QByteArray &snapshot_hash,
                       int hash_version,
                       const QUuid &creator_uuid,
                       const QUuid &finalizer_uuid)
{
    if (hash_version != 1 && hash_version != 2)
        return {};
    QByteArray bytes = hash_version == 1
        ? QByteArray("AOWIS-REVISION-DAG-V1\0", 22)
        : QByteArray("AOWIS-REVISION-DAG-V2\0", 22);
    bytes.append(uuidBytes(project_uuid));
    bytes.append(uuidBytes(branch_uuid));
    bytes.append(uuidBytes(revision_uuid));
    if (hash_version == 2)
    {
        bytes.append(uuidBytes(creator_uuid));
        bytes.append(uuidBytes(finalizer_uuid));
        appendInt64(bytes, static_cast<quint64>(created_at_ms));
    }
    appendInt64(bytes, static_cast<quint64>(number));
    appendInt64(bytes, static_cast<quint64>(finalized_at_ms));
    appendField(bytes, title.toUtf8());
    appendField(bytes, message.toUtf8());
    bytes.append(snapshot_hash);
    appendInt64(bytes, static_cast<quint64>(parents.size()));
    for (const QPair<QUuid, QByteArray> &parent : parents)
    {
        bytes.append(uuidBytes(parent.first));
        bytes.append(parent.second);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool isValidName(const QString &name)
{
    return !name.trimmed().isEmpty() && name.size() <= 256;
}

} // namespace

Database::Database()
    : connection_name_(QStringLiteral("aowis-db-%1").arg(createUuidV7().toString(QUuid::WithoutBraces)))
{
}

Database::~Database()
{
    close();
}

bool Database::openSqlite(const QString &file_path, QString *error)
{
    if (this->database_.isValid())
    {
        setError(error, QStringLiteral("Database is already open"));
        return false;
    }

    this->database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), this->connection_name_);
    this->database_.setDatabaseName(file_path);
    if (!this->database_.open())
    {
        setError(error, this->database_.lastError().text());
        close();
        return false;
    }

    if (!exec(QStringLiteral("PRAGMA foreign_keys = ON"), error) ||
        !exec(QStringLiteral("PRAGMA busy_timeout = 5000"), error) ||
        !exec(QStringLiteral("PRAGMA journal_mode = WAL"), error) ||
        !migrate(error))
    {
        close();
        return false;
    }

    // A schema version can be set even when a database has been damaged or
    // incompletely upgraded. Do not silently accept a V9 database without its
    // security audit table and immutability triggers.
    QString audit_schema_error;
    {
        QSqlQuery audit_schema(this->database_);
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
        close();
        return false;
    }
    // Without these V10 authorization columns and guards, a damaged database
    // could restore previously revoked access.
    QSqlQuery grant_schema(this->database_);
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
        close();
        return false;
    }
    // Fail closed when a database claims V11 but credentials were omitted.
    // Keep this query finished before later schema DDL on this connection.
    QSqlQuery credentials_schema(this->database_);
    if (!credentials_schema.exec(QStringLiteral(
            "SELECT user_uuid,salt,verifier,iterations,changed_by_uuid,failed_attempts,locked_until_ms "
            "FROM user_password_credentials LIMIT 0")))
    {
        setError(error, QStringLiteral("SQLite V11 password credential schema is incomplete"));
        close();
        return false;
    }
    credentials_schema.finish();
    QSqlQuery recovery_schema(this->database_);
    if (!recovery_schema.exec(QStringLiteral(
            "SELECT uuid,user_uuid,token_sha256,issued_by_uuid,expires_at_ms,used_at_ms,invalidated_at_ms "
            "FROM password_recovery_tokens LIMIT 0")))
    {
        setError(error, QStringLiteral("SQLite V12 password recovery schema is incomplete"));
        close();
        return false;
    }
    recovery_schema.finish();
    QSqlQuery recovery_guards(this->database_);
    if (!recovery_guards.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
            "'recovery_tokens_no_delete','recovery_tokens_close_only')")) ||
        !recovery_guards.next() || recovery_guards.value(0).toInt() != 2)
    {
        setError(error, QStringLiteral("SQLite V12 recovery token immutability guards are missing"));
        close();
        return false;
    }
    recovery_guards.finish();
    // Avoid silently accepting an incomplete V13 database: redaction must be
    // one-way and protected by its migrated trigger.
    QSqlQuery redaction_guard(this->database_);
    if (!redaction_guard.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='trigger' "
            "AND name='user_sessions_immutable_identity_v13' LIMIT 1")) ||
        !redaction_guard.next())
    {
        setError(error, QStringLiteral("SQLite V13 session-redaction guard is missing"));
        close();
        return false;
    }
    redaction_guard.finish();
    return true;
}

void Database::close()
{
    if (!this->database_.isValid())
        return;

    const QString name = this->connection_name_;
    this->database_.close();
    this->database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool Database::isOpen() const
{
    return this->database_.isOpen();
}

bool Database::exec(const QString &sql, QString *error) const
{
    QSqlQuery query(this->database_);
    if (query.exec(sql))
        return true;
    setError(error, query.lastError().text());
    return false;
}

bool Database::beginWrite(QString *error) const
{
    return exec(QStringLiteral("BEGIN IMMEDIATE"), error);
}

bool Database::commit(QString *error) const
{
    return exec(QStringLiteral("COMMIT"), error);
}

void Database::rollback() const
{
    exec(QStringLiteral("ROLLBACK"));
}

bool Database::migrate(QString *error)
{
    QSqlQuery version(this->database_);
    if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next())
    {
        setError(error, version.lastError().text());
        return false;
    }
    const int current = version.value(0).toInt();
    // V10 renames and drops grant tables. Release the active PRAGMA cursor
    // before running any DDL; SQLite otherwise reports SQLITE_LOCKED on DROP.
    version.finish();
    if (current > schema_version)
    {
        setError(error, QStringLiteral("Database schema is newer than this library"));
        return false;
    }
    if (current == schema_version)
        return true;
    if (current == 12)
        return migrateSessionRedactionV13(error);
    if (current == 11)
        return migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 10)
        return migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 9)
        return migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 8)
        return migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 7)
        return migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 6)
        return migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 5)
        return migrateHydraulicMembershipV6(error) && migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 4)
        return migrateObjectGroupsV5(error) && migrateHydraulicMembershipV6(error) && migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 3)
        return migrateObjectsV4(error) && migrateObjectGroupsV5(error) && migrateHydraulicMembershipV6(error) && migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 2)
        return migrateHistoryV3(error) && migrateObjectsV4(error) && migrateObjectGroupsV5(error) && migrateHydraulicMembershipV6(error) && migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current == 1)
        return migrateSecurityV2(error) && migrateHistoryV3(error) && migrateObjectsV4(error) && migrateObjectGroupsV5(error) && migrateHydraulicMembershipV6(error) && migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
    if (current != 0 || !beginWrite(error))
        return false;

    // SQLite schema v1. All primary identifiers are RFC 4122 16-byte BLOBs.
    const char *const statements[] = {
        R"SQL(CREATE TABLE projects (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            name TEXT NOT NULL CHECK(length(name)>0),
            created_at_ms INTEGER NOT NULL,
            archived_at_ms INTEGER
        ))SQL",
        R"SQL(CREATE TABLE branches (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            project_uuid BLOB NOT NULL REFERENCES projects(uuid) ON DELETE RESTRICT,
            name TEXT NOT NULL,
            head_revision_uuid BLOB REFERENCES revisions(uuid) DEFERRABLE INITIALLY DEFERRED,
            UNIQUE(project_uuid, name),
            UNIQUE(uuid, project_uuid)
        ))SQL",
        R"SQL(CREATE TABLE revisions (
            uuid BLOB PRIMARY KEY NOT NULL CHECK(length(uuid)=16),
            project_uuid BLOB NOT NULL,
            branch_uuid BLOB NOT NULL,
            number INTEGER NOT NULL CHECK(number>=1),
            state TEXT NOT NULL CHECK(state IN ('working','finalized')),
            generation INTEGER NOT NULL DEFAULT 0 CHECK(generation>=0),
            created_at_ms INTEGER NOT NULL,
            finalized_at_ms INTEGER,
            title TEXT NOT NULL DEFAULT '',
            message TEXT NOT NULL DEFAULT '',
            snapshot_sha256 BLOB CHECK(snapshot_sha256 IS NULL OR length(snapshot_sha256)=32),
            chain_sha256 BLOB CHECK(chain_sha256 IS NULL OR length(chain_sha256)=32),
            FOREIGN KEY (branch_uuid, project_uuid) REFERENCES branches(uuid, project_uuid),
            UNIQUE(branch_uuid, number),
            CHECK(state!='finalized' OR (finalized_at_ms IS NOT NULL AND snapshot_sha256 IS NOT NULL AND chain_sha256 IS NOT NULL))
        ))SQL",
        R"SQL(CREATE TABLE revision_parents (
            revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            parent_position INTEGER NOT NULL CHECK(parent_position>=0),
            parent_revision_uuid BLOB NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            PRIMARY KEY(revision_uuid,parent_position),
            UNIQUE(revision_uuid,parent_revision_uuid)
        ))SQL",
        R"SQL(CREATE UNIQUE INDEX one_working_revision_per_branch
            ON revisions(branch_uuid) WHERE state='working')SQL",
        R"SQL(CREATE INDEX revisions_by_project ON revisions(project_uuid))SQL",
        R"SQL(CREATE TABLE snapshots (
            revision_uuid BLOB PRIMARY KEY NOT NULL REFERENCES revisions(uuid) ON DELETE RESTRICT,
            format_version INTEGER NOT NULL DEFAULT 1 CHECK(format_version=1),
            content BLOB NOT NULL
        ))SQL",
        R"SQL(CREATE TRIGGER finalized_revision_readonly_update
            BEFORE UPDATE ON revisions WHEN OLD.state='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revisions are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_revision_readonly_delete
            BEFORE DELETE ON revisions WHEN OLD.state='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revisions cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER immutable_revision_identity
            BEFORE UPDATE ON revisions WHEN OLD.uuid!=NEW.uuid
                OR OLD.project_uuid!=NEW.project_uuid OR OLD.branch_uuid!=NEW.branch_uuid
                OR OLD.number!=NEW.number
            BEGIN SELECT RAISE(ABORT, 'revision identity and ancestry are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_parents_readonly_insert
            BEFORE INSERT ON revision_parents
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized revision parents are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER parents_readonly_update
            BEFORE UPDATE ON revision_parents
            BEGIN SELECT RAISE(ABORT, 'revision parent edges are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER parents_readonly_delete
            BEFORE DELETE ON revision_parents
            BEGIN SELECT RAISE(ABORT, 'revision parent edges cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER parent_must_be_finalized_in_same_project
            BEFORE INSERT ON revision_parents
            WHEN NOT EXISTS (
                SELECT 1 FROM revisions child JOIN revisions parent
                    ON parent.uuid=NEW.parent_revision_uuid
                WHERE child.uuid=NEW.revision_uuid AND parent.state='finalized'
                    AND parent.project_uuid=child.project_uuid)
            BEGIN SELECT RAISE(ABORT, 'parent must be finalized in same project'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_snapshot_readonly_update
            BEFORE UPDATE ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots are immutable'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_snapshot_readonly_delete
            BEFORE DELETE ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=OLD.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots cannot be deleted'); END)SQL",
        R"SQL(CREATE TRIGGER finalized_snapshot_readonly_insert
            BEFORE INSERT ON snapshots
            WHEN (SELECT state FROM revisions WHERE uuid=NEW.revision_uuid)='finalized'
            BEGIN SELECT RAISE(ABORT, 'finalized snapshots cannot be inserted'); END)SQL",
        R"SQL(CREATE TRIGGER branch_head_must_be_working
            BEFORE UPDATE OF head_revision_uuid ON branches
            WHEN NEW.head_revision_uuid IS NOT NULL AND NOT EXISTS (
                SELECT 1 FROM revisions WHERE uuid=NEW.head_revision_uuid
                AND branch_uuid=NEW.uuid AND state='working')
            BEGIN SELECT RAISE(ABORT, 'branch head must be its working revision'); END)SQL",
        R"SQL(CREATE TRIGGER finalize_requires_snapshot
            BEFORE UPDATE OF state ON revisions WHEN NEW.state='finalized'
            AND NOT EXISTS(SELECT 1 FROM snapshots WHERE revision_uuid=NEW.uuid)
            BEGIN SELECT RAISE(ABORT, 'cannot finalize without a snapshot'); END)SQL"
    };

    for (const char *statement : statements)
    {
        if (!exec(QString::fromUtf8(statement), error))
        {
            rollback();
            return false;
        }
    }
    if (!exec(QStringLiteral("PRAGMA user_version = 1"), error) || !commit(error))
    {
        rollback();
        return false;
    }
    return migrateSecurityV2(error) && migrateHistoryV3(error) && migrateObjectsV4(error) && migrateObjectGroupsV5(error) && migrateHydraulicMembershipV6(error) && migrateHydraulicReferencesV7(error) && migrateSessionsV8(error) && migrateSecurityAuditV9(error) && migrateSecurityAdminV10(error) && migratePasswordCredentialsV11(error) && migratePasswordRecoveryV12(error) && migrateSessionRedactionV13(error);
}

std::optional<ProjectCreation> Database::createProjectUnchecked(const QUuid &actor_uuid, const QString &name, QString *error)
{
    if (!isValidName(name))
    {
        setError(error, QStringLiteral("Project name must contain 1..256 characters"));
        return std::nullopt;
    }
    if (!beginWrite(error))
        return std::nullopt;
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("project.create"), AccessScope::system(), error))
    {
        rollback();
        return std::nullopt;
    }

    const ProjectCreation result{createUuidV7(), createUuidV7(), createUuidV7()};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(this->database_);
    bool ok = prepare(query, QStringLiteral(
        "INSERT INTO projects (uuid,name,created_at_ms,created_by_uuid) VALUES (?,?,?,?)"), error);
    if (ok)
    {
        query.addBindValue(uuidBytes(result.project_uuid));
        query.addBindValue(name.trimmed());
        query.addBindValue(now);
        query.addBindValue(uuidBytes(actor_uuid));
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO branches (uuid,project_uuid,name,created_by_uuid) VALUES (?,?,'main',?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.main_branch_uuid));
            query.addBindValue(uuidBytes(result.project_uuid));
            query.addBindValue(uuidBytes(actor_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revisions (uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid) "
            "VALUES (?,?,?,1,'working',?,?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.working_revision_uuid));
            query.addBindValue(uuidBytes(result.project_uuid));
            query.addBindValue(uuidBytes(result.main_branch_uuid));
            query.addBindValue(now);
            query.addBindValue(uuidBytes(actor_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE branches SET head_revision_uuid=? WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.working_revision_uuid));
            query.addBindValue(uuidBytes(result.main_branch_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        // The creating user owns the project. Other users with the global
        // project.create permission must not create projects they cannot read.
        QSqlQuery role(this->database_);
        ok = prepare(role, QStringLiteral("SELECT uuid FROM roles WHERE key='project.owner'"), error);
        if (ok)
            ok = run(role, error) && role.next();
        if (ok)
        {
            const QByteArray role_uuid = role.value(0).toByteArray();
            role.finish();
            ok = prepare(query, QStringLiteral(
                "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,scope_kind,"
                "scope_uuid,created_by_uuid,created_at_ms) VALUES (?,'user',?,?,'project',?,?,?)"), error);
            if (ok)
            {
                query.addBindValue(uuidBytes(createUuidV7()));
                query.addBindValue(uuidBytes(actor_uuid));
                query.addBindValue(role_uuid);
                query.addBindValue(uuidBytes(result.project_uuid));
                query.addBindValue(uuidBytes(actor_uuid));
                query.addBindValue(now);
                ok = run(query, error);
            }
        }
    }
    if (ok)
        ok = commit(error);
    if (!ok)
    {
        rollback();
        return std::nullopt;
    }
    return result;
}

std::optional<ProjectInfo> Database::projectUnchecked(const QUuid &project_uuid, QString *error) const
{
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("SELECT uuid,name,created_by_uuid FROM projects WHERE uuid=?"), error))
        return std::nullopt;
    query.addBindValue(uuidBytes(project_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Project not found"));
        return std::nullopt;
    }
    return ProjectInfo{readUuid(query.value(0)), query.value(1).toString(),
                       readUuid(query.value(2))};
}

QVector<ProjectInfo> Database::projectsUnchecked(QString *error) const
{
    QVector<ProjectInfo> result;
    QSqlQuery query(this->database_);
    if (!query.exec(QStringLiteral("SELECT uuid,name,created_by_uuid FROM projects ORDER BY created_at_ms,uuid")))
    {
        setError(error, query.lastError().text());
        return result;
    }
    while (query.next())
        result.append(ProjectInfo{readUuid(query.value(0)), query.value(1).toString(),
                                  readUuid(query.value(2))});
    return result;
}

QVector<BranchInfo> Database::branchesUnchecked(const QUuid &project_uuid, QString *error) const
{
    QVector<BranchInfo> result;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,project_uuid,name,head_revision_uuid,created_by_uuid FROM branches "
            "WHERE project_uuid=? ORDER BY name"), error))
        return result;
    query.addBindValue(uuidBytes(project_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
        result.append(BranchInfo{readUuid(query.value(0)), readUuid(query.value(1)),
                                 query.value(2).toString(), readUuid(query.value(3)), readUuid(query.value(4))});
    return result;
}

QVector<RevisionInfo> Database::revisionsUnchecked(const QUuid &branch_uuid, QString *error) const
{
    QVector<RevisionInfo> result;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid FROM revisions WHERE branch_uuid=? ORDER BY number"), error))
        return result;
    query.addBindValue(uuidBytes(branch_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
    {
        const std::optional<RevisionInfo> item = revisionUnchecked(readUuid(query.value(0)), error);
        if (!item)
            return {};
        result.append(*item);
    }
    return result;
}

std::optional<BranchInfo> Database::branchUnchecked(const QUuid &branch_uuid, QString *error) const
{
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,project_uuid,name,head_revision_uuid,created_by_uuid FROM branches WHERE uuid=?"), error))
        return std::nullopt;
    query.addBindValue(uuidBytes(branch_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Branch not found"));
        return std::nullopt;
    }
    return BranchInfo{readUuid(query.value(0)), readUuid(query.value(1)),
                      query.value(2).toString(), readUuid(query.value(3)), readUuid(query.value(4))};
}

std::optional<RevisionInfo> Database::revisionUnchecked(const QUuid &revision_uuid, QString *error) const
{
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,project_uuid,branch_uuid,number,state,generation,"
            "snapshot_sha256,chain_sha256,created_at_ms,created_by_uuid,finalized_by_uuid,hash_format_version "
            "FROM revisions WHERE uuid=?"), error))
        return std::nullopt;
    query.addBindValue(uuidBytes(revision_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Revision not found"));
        return std::nullopt;
    }
    RevisionInfo result;
    result.uuid = readUuid(query.value(0));
    result.project_uuid = readUuid(query.value(1));
    result.branch_uuid = readUuid(query.value(2));
    result.number = query.value(3).toLongLong();
    result.created_at_ms = query.value(8).toLongLong();
    result.finalized = query.value(4).toString() == QStringLiteral("finalized");
    result.generation = query.value(5).toLongLong();
    result.snapshot_sha256 = query.value(6).toByteArray();
    result.chain_sha256 = query.value(7).toByteArray();
    result.created_by_uuid = readUuid(query.value(9));
    result.finalized_by_uuid = readUuid(query.value(10));
    result.hash_version = query.value(11).toInt();
    QSqlQuery parents(this->database_);
    if (!prepare(parents, QStringLiteral(
            "SELECT parent_revision_uuid FROM revision_parents WHERE revision_uuid=? "
            "ORDER BY parent_position"), error))
        return std::nullopt;
    parents.addBindValue(uuidBytes(revision_uuid));
    if (!run(parents, error))
        return std::nullopt;
    while (parents.next())
        result.parent_revision_uuids.append(readUuid(parents.value(0)));
    return result;
}

std::optional<BranchInfo> Database::createBranchUnchecked(const QUuid &actor_uuid, const QUuid &project_uuid,
                                                 const QString &name,
                                                 const QUuid &base_finalized_revision_uuid,
                                                 QString *error)
{
    if (!isValidName(name))
    {
        setError(error, QStringLiteral("Branch name must contain 1..256 characters"));
        return std::nullopt;
    }
    if (!beginWrite(error))
        return std::nullopt;
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("project.branch.create"),
                           AccessScope::project(project_uuid), error))
    {
        rollback();
        return std::nullopt;
    }

    const std::optional<RevisionInfo> base = revisionUnchecked(base_finalized_revision_uuid, error);
    if (!base || !base->finalized || base->project_uuid != project_uuid)
    {
        setError(error, QStringLiteral("Branch base must be a finalized revision in the same project"));
        rollback();
        return std::nullopt;
    }
    const BranchInfo result{createUuidV7(), project_uuid, name.trimmed(), createUuidV7(), actor_uuid};
    QSqlQuery query(this->database_);
    bool ok = prepare(query, QStringLiteral(
        "INSERT INTO branches (uuid,project_uuid,name,created_by_uuid) VALUES (?,?,?,?)"), error);
    if (ok)
    {
        query.addBindValue(uuidBytes(result.uuid));
        query.addBindValue(uuidBytes(project_uuid));
        query.addBindValue(result.name);
        query.addBindValue(uuidBytes(actor_uuid));
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revisions (uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid) "
            "VALUES (?,?,?,1,'working',?,?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.head_revision_uuid));
            query.addBindValue(uuidBytes(project_uuid));
            query.addBindValue(uuidBytes(result.uuid));
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            query.addBindValue(uuidBytes(actor_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) "
            "VALUES (?,0,?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.head_revision_uuid));
            query.addBindValue(uuidBytes(base_finalized_revision_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO snapshots(revision_uuid,format_version,content) "
            "SELECT ?,format_version,content FROM snapshots WHERE revision_uuid=?"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.head_revision_uuid));
            query.addBindValue(uuidBytes(base_finalized_revision_uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (ok)
        ok = copyObjects(base_finalized_revision_uuid, result.head_revision_uuid, error);
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE revisions SET object_mode=(SELECT object_mode FROM revisions WHERE uuid=?) WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(base_finalized_revision_uuid));
            query.addBindValue(uuidBytes(result.head_revision_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE branches SET head_revision_uuid=? WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.head_revision_uuid));
            query.addBindValue(uuidBytes(result.uuid));
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
    return result;
}

bool Database::replaceWorkingSnapshotUnchecked(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                      qint64 expected_generation,
                                      const QByteArray &canonical_snapshot,
                                      QString *error)
{
    if (!beginWrite(error))
        return false;
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("project.model.write"),
                           AccessScope::branch(branch_uuid), error))
    {
        rollback();
        return false;
    }
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> r = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!r || r->finalized || r->generation != expected_generation)
    {
        setError(error, QStringLiteral("Working revision changed, or branch not found"));
        rollback();
        return false;
    }

    const std::optional<int> mode = objectMode(r->uuid, error);
    if (!mode || *mode != 0)
    {
        setError(error, QStringLiteral("Object-mode revision cannot use whole-snapshot operations"));
        rollback();
        return false;
    }
    const bool ok = applySnapshotOperation(*r, actor_uuid, canonical_snapshot,
                                           QStringLiteral("replace"), QUuid(), error);
    if (!ok || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
}

std::optional<QByteArray> Database::snapshotUnchecked(const QUuid &revision_uuid, QString *error) const
{
    const std::optional<int> mode = objectMode(revision_uuid, error);
    if (!mode)
        return std::nullopt;
    if (*mode == 1)
        return objectManifest(revision_uuid, error);
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("SELECT content FROM snapshots WHERE revision_uuid=?"), error))
        return std::nullopt;
    query.addBindValue(uuidBytes(revision_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Snapshot not found"));
        return std::nullopt;
    }
    return query.value(0).toByteArray();
}

std::optional<Finalization> Database::finalizeUnchecked(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                               qint64 expected_generation,
                                               const QString &title,
                                               const QString &message,
                                               QString *error)
{
    if (!beginWrite(error))
        return std::nullopt;
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("project.revision.finalize"),
                           AccessScope::branch(branch_uuid), error))
    {
        rollback();
        return std::nullopt;
    }
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> r = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!r || r->finalized || r->generation != expected_generation)
    {
        setError(error, QStringLiteral("Working revision changed, or branch not found"));
        rollback();
        return std::nullopt;
    }
    const std::optional<int> mode = objectMode(r->uuid, error);
    const std::optional<QByteArray> body = mode ? snapshotUnchecked(r->uuid, error) : std::nullopt;
    if (!body)
    {
        rollback();
        return std::nullopt;
    }
    if (*mode == 1)
    {
        QSqlQuery stored(this->database_);
        if (!prepare(stored, QStringLiteral(
                "INSERT INTO snapshots(revision_uuid,format_version,content) VALUES (?,1,?) "
                "ON CONFLICT(revision_uuid) DO UPDATE SET content=excluded.content"), error))
        {
            rollback();
            return std::nullopt;
        }
        stored.addBindValue(uuidBytes(r->uuid));
        stored.addBindValue(*body);
        if (!run(stored, error))
        {
            rollback();
            return std::nullopt;
        }
    }
    QVector<QPair<QUuid, QByteArray>> parent_hashes;
    for (const QUuid &parent_uuid : r->parent_revision_uuids)
    {
        const std::optional<RevisionInfo> parent = revisionUnchecked(parent_uuid, error);
        if (!parent || !parent->finalized || parent->project_uuid != r->project_uuid ||
            parent->chain_sha256.size() != digest_length)
        {
            setError(error, QStringLiteral("Invalid finalized parent revision"));
            rollback();
            return std::nullopt;
        }
        parent_hashes.append(qMakePair(parent_uuid, parent->chain_sha256));
    }
    const std::optional<QByteArray> operation_digest = historyDigest(r->uuid, error);
    if (!operation_digest)
    {
        rollback();
        return std::nullopt;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Finalization result;
    result.finalized_revision_uuid = r->uuid;
    result.snapshot_sha256 = QCryptographicHash::hash(*body, QCryptographicHash::Sha256);
    result.chain_sha256 = chainDigest(r->project_uuid, r->branch_uuid, r->uuid,
                                     parent_hashes, r->number, r->created_at_ms, now,
                                     title, message, result.snapshot_sha256,
                                     2, r->created_by_uuid, actor_uuid);
    result.new_working_revision_uuid = createUuidV7();

    QSqlQuery query(this->database_);
    bool ok = prepare(query, QStringLiteral(
        "UPDATE revisions SET state='finalized', finalized_at_ms=?, title=?, message=?, "
        "snapshot_sha256=?,chain_sha256=?, finalized_by_uuid=?, hash_format_version=2, "
        "operation_history_sha256=? "
        "WHERE uuid=? AND state='working' AND generation=?"), error);
    if (ok)
    {
        query.addBindValue(now);
        query.addBindValue(title.isNull() ? QStringLiteral("") : title);
        query.addBindValue(message.isNull() ? QStringLiteral("") : message);
        query.addBindValue(result.snapshot_sha256);
        query.addBindValue(result.chain_sha256);
        query.addBindValue(uuidBytes(actor_uuid));
        query.addBindValue(*operation_digest);
        query.addBindValue(uuidBytes(r->uuid));
        query.addBindValue(expected_generation);
        ok = run(query, error) && query.numRowsAffected() == 1;
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revisions (uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,object_mode) "
            "VALUES (?,?,?,?, 'working',?,?,?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.new_working_revision_uuid));
            query.addBindValue(uuidBytes(r->project_uuid));
            query.addBindValue(uuidBytes(branch_uuid));
            query.addBindValue(r->number + 1);
            query.addBindValue(now);
            query.addBindValue(uuidBytes(actor_uuid));
            query.addBindValue(*mode);
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) "
            "VALUES (?,0,?)"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.new_working_revision_uuid));
            query.addBindValue(uuidBytes(r->uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO snapshots (revision_uuid,format_version,content) "
            "SELECT ?,format_version,content FROM snapshots WHERE revision_uuid=?"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.new_working_revision_uuid));
            query.addBindValue(uuidBytes(r->uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (ok && *mode == 1)
        ok = copyObjects(r->uuid, result.new_working_revision_uuid, error);
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE branches SET head_revision_uuid=? WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(uuidBytes(result.new_working_revision_uuid));
            query.addBindValue(uuidBytes(branch_uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (ok)
        ok = commit(error);
    if (!ok)
    {
        rollback();
        return std::nullopt;
    }
    return result;
}

IntegrityReport Database::verifyProjectUnchecked(const QUuid &project_uuid) const
{
    IntegrityReport report;
    QString missing_error;
    if (!projectUnchecked(project_uuid, &missing_error))
    {
        report.error = missing_error;
        return report;
    }
    QSqlQuery revisions(this->database_);
    QString error;
    if (!prepare(revisions, QStringLiteral(
        "SELECT uuid,branch_uuid,number,finalized_at_ms,title,message,"
        "snapshot_sha256,chain_sha256,hash_format_version,created_by_uuid,finalized_by_uuid,created_at_ms FROM revisions "
        "WHERE project_uuid=? AND state='finalized' ORDER BY finalized_at_ms,uuid"), &error))
    {
        report.error = error;
        return report;
    }
    revisions.addBindValue(uuidBytes(project_uuid));
    if (!run(revisions, &error))
    {
        report.error = error;
        return report;
    }

    while (revisions.next())
    {
        const QUuid current_uuid = readUuid(revisions.value(0));
        const QUuid branch_uuid = readUuid(revisions.value(1));
        const qint64 number = revisions.value(2).toLongLong();
        const qint64 when = revisions.value(3).toLongLong();
        const QString title = revisions.value(4).toString();
        const QString message = revisions.value(5).toString();
        const QByteArray recorded_snapshot_hash = revisions.value(6).toByteArray();
        const QByteArray recorded_chain_hash = revisions.value(7).toByteArray();
        const int hash_version = revisions.value(8).toInt();
        const QUuid creator_uuid = readUuid(revisions.value(9));
        const QUuid finalizer_uuid = readUuid(revisions.value(10));
        const qint64 created_at_ms = revisions.value(11).toLongLong();

        report.failed_revision_uuid = current_uuid;
        const std::optional<QByteArray> body = snapshotUnchecked(current_uuid, &error);
        const std::optional<int> mode = objectMode(current_uuid, &error);
        if (!mode)
        {
            report.error = error;
            return report;
        }
        if (*mode == 1)
        {
            QSqlQuery stored(this->database_);
            if (!prepare(stored, QStringLiteral("SELECT content FROM snapshots WHERE revision_uuid=?"), &error))
            {
                report.error = error;
                return report;
            }
            stored.addBindValue(uuidBytes(current_uuid));
            if (!run(stored, &error) || !stored.next() || !body || stored.value(0).toByteArray() != *body)
            {
                report.error = QStringLiteral("Finalized object manifest differs from persisted snapshot");
                return report;
            }
        }
        if (!body || QCryptographicHash::hash(*body, QCryptographicHash::Sha256) != recorded_snapshot_hash)
        {
            report.error = QStringLiteral("Missing or corrupted finalized snapshot: %1").arg(error);
            return report;
        }
        const std::optional<RevisionInfo> current = revisionUnchecked(current_uuid, &error);
        if (!current)
        {
            report.error = error;
            return report;
        }
        QSqlQuery operation_seal(this->database_);
        if (!prepare(operation_seal, QStringLiteral(
                "SELECT operation_history_sha256 FROM revisions WHERE uuid=?"), &error))
        {
            report.error = error;
            return report;
        }
        operation_seal.addBindValue(uuidBytes(current_uuid));
        if (!run(operation_seal, &error) || !operation_seal.next())
        {
            report.error = error;
            return report;
        }
        if (!operation_seal.value(0).isNull())
        {
            const QByteArray expected_history_hash = operation_seal.value(0).toByteArray();
            operation_seal.finish();
            const std::optional<QByteArray> actual_history_hash = historyDigest(current_uuid, &error);
            if (!actual_history_hash || expected_history_hash != *actual_history_hash)
            {
                report.error = QStringLiteral("Finalized operation history hash mismatch: %1").arg(error);
                return report;
            }
        }

        QVector<QPair<QUuid, QByteArray>> parent_hashes;
        for (const QUuid &parent_uuid : current->parent_revision_uuids)
        {
            const std::optional<RevisionInfo> parent = revisionUnchecked(parent_uuid, &error);
            if (!parent || !parent->finalized || parent->project_uuid != project_uuid ||
                parent->chain_sha256.size() != digest_length)
            {
                report.error = QStringLiteral("Missing or invalid revision parent");
                return report;
            }
            parent_hashes.append(qMakePair(parent_uuid, parent->chain_sha256));
        }
        if (chainDigest(project_uuid, branch_uuid, current_uuid, parent_hashes,
                        number, created_at_ms, when, title, message, recorded_snapshot_hash,
                        hash_version, creator_uuid, finalizer_uuid) != recorded_chain_hash)
        {
            report.error = QStringLiteral("Revision chain hash mismatch");
            return report;
        }
        ++report.checked_revisions;
    }
    report.valid = true;
    report.failed_revision_uuid = QUuid();
    return report;
}

} // namespace aowis::db
