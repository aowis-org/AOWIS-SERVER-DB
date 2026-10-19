#ifndef AOWIS_DB_CONTENT_STORE_H
#define AOWIS_DB_CONTENT_STORE_H

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QVector>
#include <QSqlDatabase>
#include <QString>

namespace aowis::db::content_store
{
// Internal foundation for the future Merkle snapshot tree. The public
// Database API is unchanged until revision roots and indexed lookups exist.
// A Blob holds exact model-object bytes. A Tree holds a future immutable
// Merkle-tree node; the two types have distinct hash identities.
enum class Kind
{
    Blob,
    Tree
};

// Domain-separated, deterministic across SQLite and PostgreSQL.
QByteArray digest(Kind kind, const QByteArray &bytes);

// A content object whose immutable identity is calculated at construction.
// Keeping the payload and its hash together lets the Merkle builder reuse
// the digest needed by its parent without hashing the same node at insertion.
// No API accepts an independently supplied (and potentially incorrect) hash.
class PreparedContent
{
public:
    PreparedContent(Kind kind, const QByteArray &payload);

    Kind kind() const { return this->kind_; }
    const QByteArray &payload() const { return this->payload_; }
    const QByteArray &sha256() const { return this->sha256_; }

private:
    Kind kind_;
    QByteArray payload_;
    QByteArray sha256_;
};

// Insert once; an existing identical (hash, kind, bytes) is reused. This does
// NOT start or commit a transaction. Callers own the surrounding transaction.
std::optional<QByteArray> put(const QSqlDatabase &database, Kind kind,
                              const QByteArray &bytes, QString *error = nullptr);

// Insert a group of immutable objects in bounded SQL batches. Returned hashes
// correspond to the input order; duplicates are stored once. Existing rows
// are checked against the exact payload, including on hash conflicts.
// The caller owns the SQL transaction, as with put().
std::optional<QVector<QByteArray>> putMany(
    const QSqlDatabase &database, Kind kind, const QVector<QByteArray> &payloads,
    QString *error = nullptr);

// Insert an already prepared batch without hashing its payloads again.
// The PreparedContent constructor, not the caller, establishes each digest.
// The SQL conflict path still reads and verifies existing stored content.
// This has the same transaction and deduplication semantics as putMany().
std::optional<QVector<QByteArray>> putManyPrepared(
    const QSqlDatabase &database, Kind kind, const QVector<PreparedContent> &contents,
    QString *error = nullptr);

// nullopt means absent OR invalid; error distinguishes them. Every read checks
// the complete SHA-256 preimage, not just the supplied database key.
std::optional<QByteArray> get(const QSqlDatabase &database, Kind kind,
                              const QByteArray &hash, QString *error = nullptr);

// Fetch a complete set of immutable objects in bounded SQL batches. Every
// returned payload is checked against its content hash. Missing/corrupt rows
// fail the whole read; duplicated requested hashes are fetched only once.
// No writes or transactions are performed by this operation.
std::optional<QHash<QByteArray, QByteArray>> getMany(
    const QSqlDatabase &database, Kind kind, const QVector<QByteArray> &hashes,
    QString *error = nullptr);

} // namespace aowis::db::content_store

#endif // AOWIS_DB_CONTENT_STORE_H
