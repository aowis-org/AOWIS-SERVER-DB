#ifndef AOWIS_DB_DATABASE_H
#define AOWIS_DB_DATABASE_H

#include <aowis/db/identity.h>

#include <optional>

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

namespace aowis::db
{

struct ProjectInfo
{
    QUuid uuid;
    QString name;
    QUuid created_by_uuid; // Null for imported schema-v1 records.
};

struct ProjectCreation
{
    QUuid project_uuid;
    QUuid main_branch_uuid;
    QUuid working_revision_uuid;
};

struct BranchInfo
{
    QUuid uuid;
    QUuid project_uuid;
    QString name;
    QUuid head_revision_uuid;
    QUuid created_by_uuid; // Null for imported schema-v1 records.
};

struct RevisionInfo
{
    QUuid uuid;
    QUuid project_uuid;
    QUuid branch_uuid;
    // Ordered parent list. Multiple parents allow future merge commits.
    QVector<QUuid> parent_revision_uuids;
    qint64 number = 0;
    qint64 created_at_ms = 0;
    bool finalized = false;
    qint64 generation = 0;
    QByteArray snapshot_sha256;
    QByteArray chain_sha256;
    QUuid created_by_uuid;
    QUuid finalized_by_uuid;
    int hash_version = 1;
};

struct Finalization
{
    QUuid finalized_revision_uuid;
    QByteArray snapshot_sha256;
    QByteArray chain_sha256;
    QUuid new_working_revision_uuid;
};

struct IntegrityReport
{
    bool valid = false;
    qsizetype checked_revisions = 0;
    QUuid failed_revision_uuid;
    QString error;
};

// A connection is bound to the constructing thread, like QSqlDatabase itself.
// A single Database is not shared across threads. Each thread needs its own
// connection, while SQLite serializes the writers using BEGIN IMMEDIATE.
class Database final
{
public:
    Database();
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;

    bool openSqlite(const QString &file_path, QString *error = nullptr);
    void close();
    bool isOpen() const;

    // Explicit local-only one-time bootstrap. An HTTP route must never call
    // this method. Refuses any subsequent call, even by an administrator.
    std::optional<UserInfo> bootstrapAdministrator(const QString &username, QString *error = nullptr);

    std::optional<UserInfo> createUser(const OperationContext &actor, const QString &username, QString *error = nullptr);
    std::optional<GroupInfo> createGroup(const OperationContext &actor, const QString &name, QString *error = nullptr);
    std::optional<RoleInfo> createRole(const OperationContext &actor, const QString &key, QString *error = nullptr);
    std::optional<PermissionInfo> createPermission(const OperationContext &actor, const QString &key, QString *error = nullptr);
    std::optional<RoleInfo> role(const OperationContext &actor, const QString &key, QString *error = nullptr) const;
    std::optional<PermissionInfo> permission(const OperationContext &actor, const QString &key, QString *error = nullptr) const;
    std::optional<QUuid> addGroupMember(const OperationContext &actor, const QUuid &group_uuid, const QUuid &user_uuid, QString *error = nullptr);
    std::optional<QUuid> grantRolePermission(const OperationContext &actor, const QUuid &role_uuid, const QUuid &permission_uuid, QString *error = nullptr);
    std::optional<QUuid> assignRole(const OperationContext &actor, PrincipalKind kind, const QUuid &principal_uuid,
                                   const QUuid &role_uuid, const AccessScope &scope, QString *error = nullptr);
    bool hasPermission(const OperationContext &actor, const QString &permission_key,
                       const AccessScope &scope, QString *error = nullptr) const;

    std::optional<ProjectCreation> createProject(const OperationContext &actor, const QString &name, QString *error = nullptr);
    std::optional<ProjectInfo> project(const OperationContext &actor, const QUuid &project_uuid, QString *error = nullptr) const;
    QVector<ProjectInfo> projects(const OperationContext &actor, QString *error = nullptr) const;
    QVector<BranchInfo> branches(const OperationContext &actor, const QUuid &project_uuid, QString *error = nullptr) const;
    QVector<RevisionInfo> revisions(const OperationContext &actor, const QUuid &branch_uuid, QString *error = nullptr) const;
    std::optional<BranchInfo> createBranch(const OperationContext &actor, const QUuid &project_uuid,
                                           const QString &name, const QUuid &base_finalized_revision_uuid,
                                           QString *error = nullptr);
    std::optional<BranchInfo> branch(const OperationContext &actor, const QUuid &branch_uuid, QString *error = nullptr) const;
    std::optional<RevisionInfo> revision(const OperationContext &actor, const QUuid &revision_uuid, QString *error = nullptr) const;

    // Temporary opaque canonical-snapshot boundary. Model serialization and
    // operation-history logging are separate forthcoming work.
    bool replaceWorkingSnapshot(const OperationContext &actor, const QUuid &branch_uuid,
                                qint64 expected_generation, const QByteArray &canonical_snapshot,
                                QString *error = nullptr);
    std::optional<QByteArray> snapshot(const OperationContext &actor, const QUuid &revision_uuid,
                                       QString *error = nullptr) const;
    std::optional<Finalization> finalize(const OperationContext &actor, const QUuid &branch_uuid,
                                         qint64 expected_generation, const QString &title,
                                         const QString &message, QString *error = nullptr);
    IntegrityReport verifyProject(const OperationContext &actor, const QUuid &project_uuid) const;

private:
    bool exec(const QString &sql, QString *error = nullptr) const;
    bool beginWrite(QString *error = nullptr) const;
    bool commit(QString *error = nullptr) const;
    void rollback() const;
    bool migrate(QString *error);
    bool migrateSecurityV2(QString *error);
    bool requirePermission(const OperationContext &actor, const QString &key,
                           const AccessScope &scope, QString *error) const;

    // Only internally accessible, after authorization at the public boundary.
    std::optional<ProjectCreation> createProjectUnchecked(const QUuid &actor_uuid, const QString &name, QString *error);
    std::optional<ProjectInfo> projectUnchecked(const QUuid &project_uuid, QString *error) const;
    QVector<ProjectInfo> projectsUnchecked(QString *error) const;
    QVector<BranchInfo> branchesUnchecked(const QUuid &project_uuid, QString *error) const;
    QVector<RevisionInfo> revisionsUnchecked(const QUuid &branch_uuid, QString *error) const;
    std::optional<BranchInfo> createBranchUnchecked(const QUuid &actor_uuid, const QUuid &project_uuid,
                                                   const QString &name, const QUuid &base_revision_uuid, QString *error);
    std::optional<BranchInfo> branchUnchecked(const QUuid &branch_uuid, QString *error) const;
    std::optional<RevisionInfo> revisionUnchecked(const QUuid &revision_uuid, QString *error) const;
    bool replaceWorkingSnapshotUnchecked(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                         qint64 expected_generation,
                                         const QByteArray &snapshot, QString *error);
    std::optional<QByteArray> snapshotUnchecked(const QUuid &revision_uuid, QString *error) const;
    std::optional<Finalization> finalizeUnchecked(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                                 qint64 expected_generation, const QString &title,
                                                 const QString &message, QString *error);
    IntegrityReport verifyProjectUnchecked(const QUuid &project_uuid) const;

    QString connection_name_;
    QSqlDatabase database_;
};

} // namespace aowis::db

#endif // AOWIS_DB_DATABASE_H
