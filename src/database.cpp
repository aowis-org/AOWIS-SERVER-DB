#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include "sqlite_backend.h"
#include "postgres_backend.h"
#include "revision_root.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace aowis::db
{
namespace
{

constexpr int digest_length = 32;

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

// One pre-release chain format. The exact bytes are deterministic and include
// the complete model snapshot, operation history, authors and ordered parents.
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
                       const QUuid &creator_uuid,
                       const QUuid &finalizer_uuid,
                       const QByteArray &history_hash,
                       const QByteArray &object_root)
{
    if (snapshot_hash.size() != digest_length || history_hash.size() != digest_length ||
        object_root.size() != digest_length)
        return {};
    QByteArray bytes("AOWIS-REVISION-DAG-V1\0", 22);
    bytes.append(uuidBytes(project_uuid));
    bytes.append(uuidBytes(branch_uuid));
    bytes.append(uuidBytes(revision_uuid));
    bytes.append(uuidBytes(creator_uuid));
    bytes.append(uuidBytes(finalizer_uuid));
    appendInt64(bytes, static_cast<quint64>(created_at_ms));
    appendInt64(bytes, static_cast<quint64>(number));
    appendInt64(bytes, static_cast<quint64>(finalized_at_ms));
    appendField(bytes, title.toUtf8());
    appendField(bytes, message.toUtf8());
    bytes.append(snapshot_hash);
    bytes.append(history_hash);
    bytes.append(object_root);
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
    return openWithBackend(std::make_unique<SqliteBackend>(), file_path, error);
}

bool Database::openPostgres(const QString &database_name, const QString &schema_name,
                            QString *error)
{
    return openWithBackend(std::make_unique<PostgresBackend>(schema_name), database_name, error);
}

bool Database::openWithBackend(std::unique_ptr<StorageBackend> backend,
                               const QString &location, QString *error)
{
    if (this->database_.isValid())
    {
        setError(error, QStringLiteral("Database is already open"));
        return false;
    }
    this->backend_ = std::move(backend);
    if (!this->backend_->open(this->database_, this->connection_name_, location, error) ||
        !this->backend_->configure(this->database_, error) ||
        !initializeSchema(error) ||
        !this->backend_->verifySchema(this->database_, error))
    {
        close();
        return false;
    }
    return true;
}

void Database::close()
{
    if (!this->database_.isValid())
    {
        this->backend_.reset();
        return;
    }

    const QString name = this->connection_name_;
    this->database_.close();
    this->database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
    this->backend_.reset();
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
    return this->backend_ && this->backend_->beginWrite(this->database_, error);
}

bool Database::commit(QString *error) const
{
    return this->backend_ && this->backend_->commit(this->database_, error);
}

void Database::rollback() const
{
    if (this->backend_)
        this->backend_->rollback(this->database_);
}

bool Database::initializeSchema(QString *error)
{
    const std::optional<bool> existing = this->backend_->hasSchema(this->database_, error);
    if (!existing)
        return false;
    if (*existing)
        return true; // An existing database must pass verifySchema; never upgrade it.

    if (!beginWrite(error))
        return false;
    if (!this->backend_->createSchema(this->database_, error) ||
        !seedBuiltinSecurity(error) || !commit(error))
    {
        rollback();
        return false;
    }
    return true;
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
        query.addBindValue(this->backend_->bindUuid(result.project_uuid));
        query.addBindValue(name.trimmed());
        query.addBindValue(now);
        query.addBindValue(this->backend_->bindUuid(actor_uuid));
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO branches (uuid,project_uuid,name,created_by_uuid) VALUES (?,?,'main',?)"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.main_branch_uuid));
            query.addBindValue(this->backend_->bindUuid(result.project_uuid));
            query.addBindValue(this->backend_->bindUuid(actor_uuid));
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
            query.addBindValue(this->backend_->bindUuid(result.working_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(result.project_uuid));
            query.addBindValue(this->backend_->bindUuid(result.main_branch_uuid));
            query.addBindValue(now);
            query.addBindValue(this->backend_->bindUuid(actor_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE branches SET head_revision_uuid=? WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.working_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(result.main_branch_uuid));
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
            const QUuid role_uuid = this->backend_->readUuid(role.value(0));
            role.finish();
            if (role_uuid.isNull())
            {
                setError(error, QStringLiteral("Invalid built-in project.owner role UUID"));
                ok = false;
            }
            if (ok)
                ok = prepare(query, QStringLiteral(
                    "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,scope_kind,"
                    "scope_uuid,created_by_uuid,created_at_ms) VALUES (?,'user',?,?,'project',?,?,?)"), error);
            if (ok)
            {
                query.addBindValue(this->backend_->bindUuid(createUuidV7()));
                query.addBindValue(this->backend_->bindUuid(actor_uuid));
                query.addBindValue(this->backend_->bindUuid(role_uuid));
                query.addBindValue(this->backend_->bindUuid(result.project_uuid));
                query.addBindValue(this->backend_->bindUuid(actor_uuid));
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
    query.addBindValue(this->backend_->bindUuid(project_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Project not found"));
        return std::nullopt;
    }
    return ProjectInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                       this->backend_->readUuid(query.value(2))};
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
        result.append(ProjectInfo{this->backend_->readUuid(query.value(0)), query.value(1).toString(),
                                  this->backend_->readUuid(query.value(2))});
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
    query.addBindValue(this->backend_->bindUuid(project_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
        result.append(BranchInfo{this->backend_->readUuid(query.value(0)), this->backend_->readUuid(query.value(1)),
                                 query.value(2).toString(), this->backend_->readUuid(query.value(3)), this->backend_->readUuid(query.value(4))});
    return result;
}

QVector<RevisionInfo> Database::revisionsUnchecked(const QUuid &branch_uuid, QString *error) const
{
    QVector<RevisionInfo> result;
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid FROM revisions WHERE branch_uuid=? ORDER BY number"), error))
        return result;
    query.addBindValue(this->backend_->bindUuid(branch_uuid));
    if (!run(query, error))
        return result;
    while (query.next())
    {
        const std::optional<RevisionInfo> item = revisionUnchecked(this->backend_->readUuid(query.value(0)), error);
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
    query.addBindValue(this->backend_->bindUuid(branch_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Branch not found"));
        return std::nullopt;
    }
    return BranchInfo{this->backend_->readUuid(query.value(0)), this->backend_->readUuid(query.value(1)),
                      query.value(2).toString(), this->backend_->readUuid(query.value(3)), this->backend_->readUuid(query.value(4))};
}

std::optional<RevisionInfo> Database::revisionUnchecked(const QUuid &revision_uuid, QString *error) const
{
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral(
            "SELECT uuid,project_uuid,branch_uuid,number,state,generation,"
            "snapshot_sha256,chain_sha256,created_at_ms,created_by_uuid,finalized_by_uuid,object_root_sha256 "
            "FROM revisions WHERE uuid=?"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(revision_uuid));
    if (!run(query, error))
        return std::nullopt;
    if (!query.next())
    {
        setError(error, QStringLiteral("Revision not found"));
        return std::nullopt;
    }
    RevisionInfo result;
    result.uuid = this->backend_->readUuid(query.value(0));
    result.project_uuid = this->backend_->readUuid(query.value(1));
    result.branch_uuid = this->backend_->readUuid(query.value(2));
    result.number = query.value(3).toLongLong();
    result.created_at_ms = query.value(8).toLongLong();
    result.finalized = query.value(4).toString() == QStringLiteral("finalized");
    result.generation = query.value(5).toLongLong();
    result.snapshot_sha256 = query.value(6).toByteArray();
    result.chain_sha256 = query.value(7).toByteArray();
    result.object_root_sha256 = query.value(11).toByteArray();
    result.created_by_uuid = this->backend_->readUuid(query.value(9));
    result.finalized_by_uuid = this->backend_->readUuid(query.value(10));
    QSqlQuery parents(this->database_);
    if (!prepare(parents, QStringLiteral(
            "SELECT parent_revision_uuid FROM revision_parents WHERE revision_uuid=? "
            "ORDER BY parent_position"), error))
        return std::nullopt;
    parents.addBindValue(this->backend_->bindUuid(revision_uuid));
    if (!run(parents, error))
        return std::nullopt;
    while (parents.next())
        result.parent_revision_uuids.append(this->backend_->readUuid(parents.value(0)));
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
    // Branch creation copies the base revision's full snapshot and objects.
    // An actor allowed to create branches must also be able to read that base,
    // including when it belongs to a different branch in the same project.
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("project.read"),
                           AccessScope::branch(base->branch_uuid), error))
    {
        rollback();
        return std::nullopt;
    }
    const BranchInfo result{createUuidV7(), project_uuid, name.trimmed(), createUuidV7(), actor_uuid};
    QSqlQuery query(this->database_);
    bool ok = prepare(query, QStringLiteral(
        "INSERT INTO branches (uuid,project_uuid,name,created_by_uuid) VALUES (?,?,?,?)"), error);
    if (ok)
    {
        query.addBindValue(this->backend_->bindUuid(result.uuid));
        query.addBindValue(this->backend_->bindUuid(project_uuid));
        query.addBindValue(result.name);
        query.addBindValue(this->backend_->bindUuid(actor_uuid));
        ok = run(query, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revisions (uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid) "
            "VALUES (?,?,?,1,'working',?,?)"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.head_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(project_uuid));
            query.addBindValue(this->backend_->bindUuid(result.uuid));
            query.addBindValue(QDateTime::currentMSecsSinceEpoch());
            query.addBindValue(this->backend_->bindUuid(actor_uuid));
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
            query.addBindValue(this->backend_->bindUuid(result.head_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(base_finalized_revision_uuid));
            ok = run(query, error);
        }
    }
    const std::optional<int> base_mode = ok ? objectMode(base_finalized_revision_uuid, error) : std::nullopt;
    if (ok && !base_mode)
        ok = false;
    // Object-mode working revisions reconstruct their current manifest from
    // editable objects; they must not copy a full immutable manifest blob.
    if (ok && *base_mode == 0)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO snapshots(revision_uuid,format_version,content) "
            "SELECT ?,format_version,content FROM snapshots WHERE revision_uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.head_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(base_finalized_revision_uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (ok && *base_mode == 1)
    {
        // The immutable Merkle snapshot is authoritative for object contents.
        // Historical revision_objects rows are not a source of truth and will
        // be removed entirely in the finalized-row cutover.
        if (base->object_root_sha256.size() != digest_length)
        {
            setError(error, QStringLiteral("Finalized base revision is missing its Merkle root"));
            ok = false;
        }
        else
        {
            ok = materializeWorkingObjectsFromRoot(
                this->database_, *this->backend_, result.head_revision_uuid,
                base->object_root_sha256, error);
        }
        if (ok)
            ok = rebuildWorkingHydraulicIndexes(result.head_revision_uuid, error);
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE revisions SET object_mode=(SELECT object_mode FROM revisions WHERE uuid=?) WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(base_finalized_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(result.head_revision_uuid));
            ok = run(query, error);
        }
    }
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE branches SET head_revision_uuid=? WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.head_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(result.uuid));
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
    {
        const std::optional<RevisionInfo> info = revisionUnchecked(revision_uuid, error);
        if (!info)
            return std::nullopt;
        if (!info->finalized)
            return objectManifest(revision_uuid, error);
        // Finalized object-mode revisions have an immutable stored manifest.
        // Never reconstruct historical bytes from transient SQL model rows.
    }
    QSqlQuery query(this->database_);
    if (!prepare(query, QStringLiteral("SELECT content FROM snapshots WHERE revision_uuid=?"), error))
        return std::nullopt;
    query.addBindValue(this->backend_->bindUuid(revision_uuid));
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
    const std::optional<Finalization> result = finalizeInWriteTransaction(
        actor_uuid, branch_uuid, expected_generation, title, message, error);
    if (!result || !commit(error))
    {
        rollback();
        return std::nullopt;
    }
    return result;
}

std::optional<Finalization> Database::finalizeInWriteTransaction(const QUuid &actor_uuid, const QUuid &branch_uuid,
                                               qint64 expected_generation,
                                               const QString &title,
                                               const QString &message,
                                               QString *error)
{
    if (!requirePermission(OperationContext{actor_uuid}, QStringLiteral("project.revision.finalize"),
                           AccessScope::branch(branch_uuid), error))
    {
        return std::nullopt;
    }
    const std::optional<BranchInfo> b = branchUnchecked(branch_uuid, error);
    const std::optional<RevisionInfo> r = b ? revisionUnchecked(b->head_revision_uuid, error) : std::nullopt;
    if (!r || r->finalized || r->generation != expected_generation)
    {
        setError(error, QStringLiteral("Working revision changed, or branch not found"));
        return std::nullopt;
    }
    const std::optional<int> mode = objectMode(r->uuid, error);
    if (!mode)
        return std::nullopt;
    // Publish the Merkle root only at finalization, within one transaction.
    // Temporary working rows are removed after sealing; immutable trees
    // alone represent finalized model snapshots.
    QByteArray object_root(digest_length, '\0');
    if (*mode == 1)
    {
        QByteArray inherited_root;
        if (!r->parent_revision_uuids.isEmpty())
        {
            const std::optional<RevisionInfo> first_parent =
                revisionUnchecked(r->parent_revision_uuids.first(), error);
            if (!first_parent || !first_parent->finalized)
                return std::nullopt;
            inherited_root = first_parent->object_root_sha256;
        }
        const std::optional<QByteArray> sealed = buildRevisionRoot(
            this->database_, *this->backend_, r->uuid, inherited_root, error);
        if (!sealed)
            return std::nullopt;
        object_root = *sealed;
    }
    const std::optional<QByteArray> body = snapshotUnchecked(r->uuid, error);
    if (!body)
    {
        return std::nullopt;
    }
    if (*mode == 1)
    {
        QSqlQuery stored(this->database_);
        if (!prepare(stored, QStringLiteral(
                "INSERT INTO snapshots(revision_uuid,format_version,content) VALUES (?,1,?) "
                "ON CONFLICT(revision_uuid) DO UPDATE SET content=excluded.content"), error))
        {
                return std::nullopt;
        }
        stored.addBindValue(this->backend_->bindUuid(r->uuid));
        stored.addBindValue(*body);
        if (!run(stored, error))
        {
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
                return std::nullopt;
        }
        parent_hashes.append(qMakePair(parent_uuid, parent->chain_sha256));
    }
    const std::optional<QByteArray> operation_digest = historyDigest(r->uuid, error);
    if (!operation_digest)
    {
        return std::nullopt;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Finalization result;
    result.finalized_revision_uuid = r->uuid;
    result.snapshot_sha256 = QCryptographicHash::hash(*body, QCryptographicHash::Sha256);
    result.chain_sha256 = chainDigest(r->project_uuid, r->branch_uuid, r->uuid,
                                     parent_hashes, r->number, r->created_at_ms, now,
                                     title, message, result.snapshot_sha256,
                                     r->created_by_uuid, actor_uuid, *operation_digest, object_root);
    result.new_working_revision_uuid = createUuidV7();

    // The immutable root now owns the finalized snapshot, but the *existing*
    // SQL working state is already validated. For object-mode revisions hand
    // that state directly to the next working head instead of decoding every
    // Merkle blob and rebuilding the hydraulic indexes after finalization.
    //
    // "sealing" exists only inside this write transaction: it releases the
    // one-working-revision uniqueness slot while preserving the old SQL rows
    // long enough for set-based INSERT ... SELECT statements. No committed
    // revision may remain in that state. On any error the caller rolls back.
    QSqlQuery query(this->database_);
    bool ok = true;
    if (*mode == 1)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE revisions SET state='sealing' "
            "WHERE uuid=? AND state='working' AND generation=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(r->uuid));
            query.addBindValue(expected_generation);
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }

    const auto sealOldRevision = [&](const QString &expected_state) -> bool
    {
        QSqlQuery update(this->database_);
        if (!prepare(update, QStringLiteral(
                "UPDATE revisions SET state='finalized', finalized_at_ms=?, title=?, message=?, "
                "snapshot_sha256=?,chain_sha256=?, finalized_by_uuid=?, "
                "operation_history_sha256=?, object_root_sha256=? "
                "WHERE uuid=? AND state=? AND generation=?"), error))
            return false;
        update.addBindValue(now);
        update.addBindValue(title.isNull() ? QStringLiteral("") : title);
        update.addBindValue(message.isNull() ? QStringLiteral("") : message);
        update.addBindValue(result.snapshot_sha256);
        update.addBindValue(result.chain_sha256);
        update.addBindValue(this->backend_->bindUuid(actor_uuid));
        update.addBindValue(*operation_digest);
        update.addBindValue(*mode == 1 ? QVariant(object_root) : QVariant());
        update.addBindValue(this->backend_->bindUuid(r->uuid));
        update.addBindValue(expected_state);
        update.addBindValue(expected_generation);
        return run(update, error) && update.numRowsAffected() == 1;
    };

    // For opaque snapshot revisions the old ordering is retained: finalize
    // before inserting the successor, since there is no SQL model to pass on.
    if (ok && *mode == 0)
        ok = sealOldRevision(QStringLiteral("working"));

    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revisions (uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,object_mode) "
            "VALUES (?,?,?,?, 'working',?,?,?)"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.new_working_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(r->project_uuid));
            query.addBindValue(this->backend_->bindUuid(branch_uuid));
            query.addBindValue(r->number + 1);
            query.addBindValue(now);
            query.addBindValue(this->backend_->bindUuid(actor_uuid));
            query.addBindValue(*mode);
            ok = run(query, error);
        }
    }
    // Object-mode rows are NOT retained as historical storage. They move from
    // one temporary editable SQL head to the next and are discarded from the
    // old revision before its immutable state is published. No root traversal,
    // payload decoding, or rebuilding of hydraulic indexes is needed here.
    if (ok && *mode == 1)
    {
        const QStringList transfers{
            QStringLiteral(
                "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
                "SELECT ?,object_uuid,kind,content,version,deleted "
                "FROM revision_objects WHERE revision_uuid=?"),
            QStringLiteral(
                "INSERT INTO hydraulic_network_members(revision_uuid,network_uuid,entity_uuid) "
                "SELECT ?,network_uuid,entity_uuid FROM hydraulic_network_members WHERE revision_uuid=?"),
            QStringLiteral(
                "INSERT INTO hydraulic_network_references"
                "(revision_uuid,network_uuid,source_uuid,target_uuid,field) "
                "SELECT ?,network_uuid,source_uuid,target_uuid,field "
                "FROM hydraulic_network_references WHERE revision_uuid=?")
        };
        for (const QString &statement : transfers)
        {
            QSqlQuery transfer(this->database_);
            if (!prepare(transfer, statement, error))
            {
                ok = false;
                break;
            }
            transfer.addBindValue(this->backend_->bindUuid(result.new_working_revision_uuid));
            transfer.addBindValue(this->backend_->bindUuid(r->uuid));
            if (!run(transfer, error))
            {
                ok = false;
                break;
            }
        }
        if (ok)
        {
            for (const QString &table : {QStringLiteral("hydraulic_network_references"),
                                         QStringLiteral("hydraulic_network_members"),
                                         QStringLiteral("revision_objects")})
            {
                QSqlQuery purge(this->database_);
                if (!prepare(purge, QStringLiteral("DELETE FROM %1 WHERE revision_uuid=?").arg(table), error))
                {
                    ok = false;
                    break;
                }
                purge.addBindValue(this->backend_->bindUuid(r->uuid));
                if (!run(purge, error))
                {
                    ok = false;
                    break;
                }
            }
        }
        if (ok)
            ok = sealOldRevision(QStringLiteral("sealing"));
    }

    // The sealed manifest belongs only to its finalized predecessor.
    if (ok && *mode == 0)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO snapshots (revision_uuid,format_version,content) "
            "SELECT ?,format_version,content FROM snapshots WHERE revision_uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.new_working_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(r->uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    // Both predecessors are finalized before publishing the ancestry edge.
    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) "
            "VALUES (?,0,?)"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.new_working_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(r->uuid));
            ok = run(query, error);
        }
    }

    if (ok)
    {
        ok = prepare(query, QStringLiteral(
            "UPDATE branches SET head_revision_uuid=? WHERE uuid=?"), error);
        if (ok)
        {
            query.addBindValue(this->backend_->bindUuid(result.new_working_revision_uuid));
            query.addBindValue(this->backend_->bindUuid(branch_uuid));
            ok = run(query, error) && query.numRowsAffected() == 1;
        }
    }
    if (!ok)
    {
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
        "snapshot_sha256,chain_sha256,created_by_uuid,finalized_by_uuid,created_at_ms,object_root_sha256 FROM revisions "
        "WHERE project_uuid=? AND state='finalized' ORDER BY finalized_at_ms,uuid"), &error))
    {
        report.error = error;
        return report;
    }
    revisions.addBindValue(this->backend_->bindUuid(project_uuid));
    if (!run(revisions, &error))
    {
        report.error = error;
        return report;
    }

    // A project may have thousands of highly overlapping immutable roots.
    // Keep verified content only for THIS invocation; every subsequent
    // verification starts clean and independently checks database bytes.
    QHash<QByteArray, QByteArray> verified_merkle_nodes;
    QSet<QByteArray> verified_merkle_blobs;
    while (revisions.next())
    {
        const QUuid current_uuid = this->backend_->readUuid(revisions.value(0));
        const QUuid branch_uuid = this->backend_->readUuid(revisions.value(1));
        const qint64 number = revisions.value(2).toLongLong();
        const qint64 when = revisions.value(3).toLongLong();
        const QString title = revisions.value(4).toString();
        const QString message = revisions.value(5).toString();
        const QByteArray recorded_snapshot_hash = revisions.value(6).toByteArray();
        const QByteArray recorded_chain_hash = revisions.value(7).toByteArray();
        const QUuid creator_uuid = this->backend_->readUuid(revisions.value(8));
        const QUuid finalizer_uuid = this->backend_->readUuid(revisions.value(9));
        const qint64 created_at_ms = revisions.value(10).toLongLong();
        const QByteArray stored_root = revisions.value(11).toByteArray();

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
            if (stored_root.size() != digest_length ||
                !verifyRevisionRootCached(this->database_, stored_root,
                    verified_merkle_nodes, verified_merkle_blobs, &error))
            {
                report.error = QStringLiteral("Finalized revision Merkle root mismatch: %1").arg(error);
                return report;
            }
            // Rebuild the manifest using the sealed Merkle objects AND the
            // independent event/group journal. No finalized SQL object rows
            // are retained; modifying the journal must still be detected.
            const std::optional<QByteArray> reconstructed = objectManifest(current_uuid, &error);
            if (!reconstructed)
            {
                report.error = QStringLiteral("Cannot reconstruct finalized object manifest: %1").arg(error);
                return report;
            }
            if (!body || *reconstructed != *body)
            {
                report.error = QStringLiteral("Finalized object manifest differs from persisted SQL history");
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
        operation_seal.addBindValue(this->backend_->bindUuid(current_uuid));
        if (!run(operation_seal, &error) || !operation_seal.next())
        {
            report.error = error;
            return report;
        }
        const QByteArray expected_history_hash = operation_seal.value(0).toByteArray();
        operation_seal.finish();
        if (expected_history_hash.size() != digest_length)
        {
            report.error = QStringLiteral("Finalized operation history seal is missing or invalid");
            return report;
        }
        const std::optional<QByteArray> actual_history_hash = historyDigest(current_uuid, &error);
        if (!actual_history_hash || expected_history_hash != *actual_history_hash)
        {
            report.error = QStringLiteral("Finalized operation history hash mismatch: %1").arg(error);
            return report;
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
                        creator_uuid, finalizer_uuid, expected_history_hash,
                        *mode == 1 ? stored_root : QByteArray(digest_length, '\0')) != recorded_chain_hash)
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
