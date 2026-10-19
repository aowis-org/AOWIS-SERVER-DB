#ifndef AOWIS_DB_MERKLE_TREE_H
#define AOWIS_DB_MERKLE_TREE_H

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QPair>
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

// Replaces or removes a single key, returning the new root. Identical writes
// and missing-key deletions return the original root without new nodes.
std::optional<QByteArray> update(const QSqlDatabase &database, const QByteArray &root,
                                  const Change &change, QString *error = nullptr);

// A deterministic batch. Duplicate keys are rejected. Every intermediate
// root is private to the calling transaction; only the returned root is used.
std::optional<QByteArray> apply(const QSqlDatabase &database, const QByteArray &root,
                                 const QVector<Change> &changes, QString *error = nullptr);

// Construct the canonical root for a COMPLETE set of UUID->blob mappings.
// Builds the radix tree in memory and stores immutable nodes in batches,
// avoiding a SQL round trip per node for initial snapshots. Nullopt values
// and duplicate UUIDs are rejected. Roots match apply(emptyRoot(), changes).
std::optional<QByteArray> build(const QSqlDatabase &database,
                                const QVector<Change> &contents, QString *error = nullptr);

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
