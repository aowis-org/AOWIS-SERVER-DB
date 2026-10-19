#ifndef AOWIS_DB_REVISION_ROOT_H
#define AOWIS_DB_REVISION_ROOT_H

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QSqlDatabase>
#include <QSet>
#include <QString>
#include <QUuid>
#include <QVector>

#include "merkle_tree.h"

namespace aowis::db
{
class StorageBackend;

// Seals the exact revision-object mapping as an immutable Merkle root.
// Call only in the same WRITE transaction that finalizes the revision.
// Every object (including tombstones) has an independently hashed, canonical
// record containing kind, per-object version, deletion flag and content.
std::optional<QByteArray> buildRevisionRoot(const QSqlDatabase &database,
                                             const StorageBackend &backend,
                                             const QUuid &revision_uuid,
                                             const QByteArray &base_root = {},
                                             QString *error = nullptr);

// Immutable object record retrieved from the Merkle root. The root (rather
// than a SQL revision-object row) is the authority for finalized snapshots.
// The decoder validates the exact binary record encoding and content digest.
struct MerkleObjectRecord
{
    QUuid uuid;
    QString kind;
    QByteArray content;
    qint64 version = 0;
    bool deleted = false;
};

// A missing UUID reports "Object not found"; corruption reports a different
// error. Both methods return exact historical states without consulting the
// revision_objects SQL materialization or traversing parent revisions.
std::optional<MerkleObjectRecord> readRevisionObject(
    const QSqlDatabase &database, const QByteArray &root,
    const QUuid &object_uuid, QString *error = nullptr);
std::optional<QVector<MerkleObjectRecord>> readRevisionObjects(
    const QSqlDatabase &database, const QByteArray &root, QString *error = nullptr);

// Decode a bounded, ordered slice of UUID/content-hash entries obtained from
// merkle_tree::entries(). Batch-fetches the immutable blobs without rewalking
// the tree for each slice; includes tombstones. The caller owns the verified
// tree entry ordering and chooses its memory budget.
std::optional<QVector<MerkleObjectRecord>> readRevisionObjectEntries(
    const QSqlDatabase &database, const QVector<merkle_tree::Entry> &entries,
    QString *error = nullptr);

// Fetch selected UUIDs from an immutable snapshot by traversing only their
// shared Merkle search paths in bounded SQL batches. No whole-tree traversal
// is needed for sparse requests. Missing objects are an error (including
// missing references in a network root). Full integrity checks use a separate
// whole-tree traversal.
std::optional<QVector<MerkleObjectRecord>> readRevisionObjectsSelected(
    const QSqlDatabase &database, const QByteArray &root,
    const QSet<QUuid> &requested, QString *error = nullptr);

// Materialize one NEW editable revision from an immutable finalized root.
// This is a transient working-head index, not historical authoritative data.
// Includes deleted identities and versions; validates each source hash.
// Caller owns the write transaction and destination revision (which must be empty).
// PostgreSQL inserts bounded batches, avoiding a SQL round trip per object.
bool materializeWorkingObjectsFromRoot(const QSqlDatabase &database,
                                       const StorageBackend &backend,
                                       const QUuid &working_revision_uuid,
                                       const QByteArray &finalized_root,
                                       QString *error = nullptr);

// Internal optimistic-concurrency guard for one editable SQL head. Compares
// its temporary working materialization to the immutable parent snapshot.
// Never use this as the finalized-snapshot integrity verifier.
bool verifyWorkingObjectsAgainstRoot(const QSqlDatabase &database,
                                     const StorageBackend &backend,
                                     const QUuid &working_revision_uuid,
                                     const QByteArray &root,
                                     QString *error = nullptr);

// Full integrity check over every reachable immutable tree node and record.
// Historical SQL object rows do not exist. Journal and manifest verification
// is handled independently by Database::verifyProjectUnchecked().
bool verifyRevisionRoot(const QSqlDatabase &database,
                        const QByteArray &root, QString *error = nullptr);

// Reuses SHA-256-verified immutable nodes and blobs across the finalized roots
// checked during a single project verification. Never retain these caches
// across calls: a new request must recheck physical database contents.
bool verifyRevisionRootCached(const QSqlDatabase &database,
    const QByteArray &root, QHash<QByteArray, QByteArray> &verified_nodes,
    QSet<QByteArray> &verified_blobs, QString *error = nullptr);

} // namespace aowis::db

#endif // AOWIS_DB_REVISION_ROOT_H
