#ifndef AOWIS_DB_MERKLE_TREE_H
#define AOWIS_DB_MERKLE_TREE_H

#include <functional>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>
#include <QVector>

namespace aowis::db::merkle_tree
{
// Persistent path-compressed, 16-way UUID radix tree. Values are hashes of
// immutable content_store::Kind::Blob objects; tree nodes use Kind::Tree.
// Roots are full SHA-256 hashes. Root depth depends on UUID keys, NEVER on
// the number of revisions sharing this snapshot.
//
// All operations use the caller's SQL transaction. Mutations publish only a
// new root; old roots and their nodes stay intact and independently readable.
struct Entry
{
    QUuid key;
    QByteArray blob_sha256;
};

struct Change
{
    QUuid key;
    // nullopt means delete; otherwise references an existing Blob hash.
    std::optional<QByteArray> blob_sha256;
};

// Creates/reuses the canonical empty tree node.
std::optional<QByteArray> emptyRoot(const QSqlDatabase &database, QString *error = nullptr);

// Missing key: nullopt, no error. Invalid node/root: nullopt, error populated.
// If an error pointer is passed, it is cleared at the start of an operation.
std::optional<QByteArray> lookup(const QSqlDatabase &database, const QByteArray &root,
                                  const QUuid &key, QString *error = nullptr);

// Batch point lookups from an immutable Merkle root. Traverses only paths
// leading to requested UUIDs and shares verified SQL reads across common
// prefixes. Missing UUIDs are omitted (as with lookup()); corruption on a
// visited path is an error. Full integrity verification uses entries().
std::optional<QHash<QUuid, QByteArray>> lookupMany(
    const QSqlDatabase &database, const QByteArray &root,
    const QSet<QUuid> &requested, QString *error = nullptr);

// A read-only, UUID-ordered difference between two immutable snapshots.
// Missing values signify additions/deletions; two present, unequal values
// signify a modification. Blob payloads are never fetched by this operation.
struct Difference
{
    QUuid key;
    std::optional<QByteArray> before_sha256;
    std::optional<QByteArray> after_sha256;
};

struct DiffStats
{
    quint64 tree_nodes_loaded = 0;
    quint64 shared_subtrees_skipped = 0;
    quint64 differences_emitted = 0;
};

using DifferenceVisitor = std::function<bool(const Difference &)>;

// Traverses only differing Merkle subtrees, including when compressed paths
// have different shapes. A visitor returning false stops the scan and causes
// diff() to fail with an error; results emitted earlier are not retracted.
// This is a comparison, NOT a full integrity audit: equal subtrees are skipped
// without reading their descendants. Roots and traversed nodes are verified.
// No SQL transactions or writes are performed; callers must ensure both
// snapshots remain readable throughout the scan.
bool diff(const QSqlDatabase &database, const QByteArray &before_root,
          const QByteArray &after_root, const DifferenceVisitor &visitor,
          QString *error = nullptr, DiffStats *stats = nullptr);

// Replaces or removes a single key, returning the new root. Identical writes
// and missing-key deletions return the original root without new nodes.
std::optional<QByteArray> update(const QSqlDatabase &database, const QByteArray &root,
                                  const Change &change, QString *error = nullptr);

// A deterministic batch. Duplicate keys are rejected. Changes with shared
// UUID prefixes are rewritten together; generated tree nodes are staged in
// memory and only nodes reachable from the final root are stored. The old
// immutable roots remain readable and the caller owns the transaction.
std::optional<QByteArray> apply(const QSqlDatabase &database, const QByteArray &root,
                                 const QVector<Change> &changes, QString *error = nullptr);

// Construct the canonical root for a COMPLETE set of UUID->blob mappings.
// Builds the radix tree in memory and stores immutable nodes in batches,
// avoiding a SQL round trip per node for initial snapshots. Nullopt values
// and duplicate UUIDs are rejected. Roots match apply(emptyRoot(), changes).
std::optional<QByteArray> build(const QSqlDatabase &database,
                                const QVector<Change> &contents, QString *error = nullptr);

// Internal first-seal path ONLY: blob digests have just been returned by a
// successful content_store::putMany in the same SQL transaction. No need to
// fetch all those same blobs a second time. Do not use for arbitrary hashes;
// build() above still checks that every referenced blob exists and is valid.
std::optional<QByteArray> buildFromInsertedBlobs(
    const QSqlDatabase &database, const QVector<Change> &contents,
    QString *error = nullptr);

// Returns entries in UUID byte order, checking all reachable tree nodes.
// Unlike a point lookup, this detects missing/corrupt nodes anywhere below
// the root. Empty results are valid for an empty root.
std::optional<QVector<Entry>> entries(const QSqlDatabase &database,
                                      const QByteArray &root, QString *error = nullptr);

// Internal full-history verification helper. The cache belongs to ONE
// verification request, never to the database connection or process. Immutable
// node payloads already SHA-256-checked in this request can be reused across
// revisions sharing subtrees. Shape/path checks still run for every root.
std::optional<QVector<Entry>> entriesCached(const QSqlDatabase &database,
    const QByteArray &root, QHash<QByteArray, QByteArray> &verified_nodes,
    QString *error = nullptr);
}

#endif // AOWIS_DB_MERKLE_TREE_H
