#include "content_store.h"
#include "performance_profile.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QStringList>

namespace aowis::db::content_store
{
namespace
{
void setError(QString *error, const QString &text)
{
    if (error != nullptr)
        *error = text;
}

QString kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::Blob: return QStringLiteral("blob");
    case Kind::Tree: return QStringLiteral("tree");
    }
    return {};
}

void appendU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(static_cast<char>((value >> shift) & 0xffU));
}
}

QByteArray digest(Kind kind, const QByteArray &bytes)
{
    // Fixed binary preimage: "AOWIS-CONTENT-V1" + NUL, 1-byte kind tag,
    // unsigned 64-bit big-endian length, and the exact uncompressed bytes.
    // Never use the SQL backend's binary/text representation in this hash.
    QByteArray prefix("AOWIS-CONTENT-V1\0", 17);
    prefix.append(kind == Kind::Blob ? '\x01' : '\x02');
    appendU64(prefix, static_cast<quint64>(bytes.size()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(prefix);
    hash.addData(bytes);
    return hash.result();
}

std::optional<QByteArray> get(const QSqlDatabase &database, Kind kind,
                              const QByteArray &hash, QString *error)
{
    if (hash.size() != 32)
    {
        setError(error, QStringLiteral("Invalid content SHA-256 length"));
        return std::nullopt;
    }
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT kind, payload FROM content_objects WHERE sha256=?")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(hash);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    if (!query.next())
    {
        setError(error, QStringLiteral("Content object not found"));
        return std::nullopt;
    }
    if (query.value(0).toString() != kindName(kind) || query.value(1).isNull())
    {
        setError(error, QStringLiteral("Content object kind or payload mismatch"));
        return std::nullopt;
    }
    const QByteArray result = query.value(1).toByteArray();
    if (digest(kind, result) != hash)
    {
        setError(error, QStringLiteral("Content object SHA-256 mismatch"));
        return std::nullopt;
    }
    return result;
}

std::optional<QHash<QByteArray, QByteArray>> getMany(
    const QSqlDatabase &database, Kind kind, const QVector<QByteArray> &hashes,
    QString *error)
{
    if (error != nullptr)
        error->clear();
    QSet<QByteArray> unique;
    QVector<QByteArray> requested;
    requested.reserve(hashes.size());
    for (const QByteArray &hash : hashes)
    {
        if (hash.size() != 32)
        {
            setError(error, QStringLiteral("Invalid content SHA-256 length"));
            return std::nullopt;
        }
        if (!unique.contains(hash))
        {
            unique.insert(hash);
            requested.append(hash);
        }
    }

    QHash<QByteArray, QByteArray> result;
    result.reserve(requested.size());
    constexpr qsizetype batch_size = 256;
    for (qsizetype start = 0; start < requested.size(); start += batch_size)
    {
        const qsizetype count = qMin(batch_size, requested.size() - start);
        QStringList placeholders;
        for (qsizetype i = 0; i < count; ++i)
            placeholders.append(QStringLiteral("?"));
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(
                "SELECT sha256,kind,payload FROM content_objects WHERE sha256 IN (%1)")
                .arg(placeholders.join(QLatin1Char(',')))))
        {
            setError(error, query.lastError().text());
            return std::nullopt;
        }
        QSet<QByteArray> batch;
        for (qsizetype i = 0; i < count; ++i)
        {
            const QByteArray &hash = requested.at(start + i);
            query.addBindValue(hash);
            batch.insert(hash);
        }
        if (!query.exec())
        {
            setError(error, query.lastError().text());
            return std::nullopt;
        }
        while (query.next())
        {
            const QByteArray hash = query.value(0).toByteArray();
            if (!batch.contains(hash) || result.contains(hash) ||
                query.value(1).toString() != kindName(kind) || query.value(2).isNull())
            {
                setError(error, QStringLiteral("Invalid content object in batched read"));
                return std::nullopt;
            }
            const QByteArray payload = query.value(2).toByteArray();
            if (digest(kind, payload) != hash)
            {
                setError(error, QStringLiteral("Content object SHA-256 mismatch"));
                return std::nullopt;
            }
            result.insert(hash, payload);
        }
        if (query.lastError().isValid())
        {
            setError(error, query.lastError().text());
            return std::nullopt;
        }
        if (result.size() < start + count)
        {
            setError(error, QStringLiteral("Content object not found in batched read"));
            return std::nullopt;
        }
    }
    return result;
}

std::optional<QVector<QByteArray>> putMany(
    const QSqlDatabase &database, Kind kind, const QVector<QByteArray> &payloads,
    QString *error)
{
    performance_profile::PhaseTimer phases(kind == Kind::Blob ? "content_blobs" : "content_nodes");
    if (error != nullptr)
        error->clear();

    QVector<QByteArray> results;
    results.reserve(payloads.size());
    QHash<QByteArray, QByteArray> unique;
    QVector<QByteArray> ordered;
    unique.reserve(payloads.size());
    ordered.reserve(payloads.size());
    for (const QByteArray &payload : payloads)
    {
        const QByteArray bytes = payload.isNull() ? QByteArray("") : payload;
        const QByteArray hash = digest(kind, bytes);
        results.append(hash);
        if (unique.contains(hash))
        {
            if (unique.value(hash) != bytes)
            {
                setError(error, QStringLiteral("Content digest collision in batch"));
                return std::nullopt;
            }
        }
        else
        {
            unique.insert(hash, bytes);
            ordered.append(hash);
        }
    }

    phases.mark("hash_and_deduplicate", payloads.size());
    constexpr qsizetype batch_size = 256;
    if (database.driverName() == QStringLiteral("QPSQL"))
    {
        // One parameter and one round trip per batch, not one round trip per
        // object. Hex/base64 are transport encodings, NOT hashed data.
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(
                "INSERT INTO content_objects(sha256,kind,payload) "
                "SELECT decode(src.hash_hex,'hex'), ?, decode(src.payload_b64,'base64') "
                "FROM jsonb_to_recordset(CAST(? AS jsonb)) "
                "AS src(hash_hex text,payload_b64 text) "
                "ON CONFLICT(sha256) DO NOTHING")))
        {
            setError(error, query.lastError().text());
            return std::nullopt;
        }
        for (qsizetype start = 0; start < ordered.size(); start += batch_size)
        {
            QJsonArray rows;
            const qsizetype end = qMin(start + batch_size, ordered.size());
            for (qsizetype i = start; i < end; ++i)
            {
                const QByteArray &hash = ordered.at(i);
                QJsonObject row;
                row.insert(QStringLiteral("hash_hex"), QString::fromLatin1(hash.toHex()));
                row.insert(QStringLiteral("payload_b64"),
                           QString::fromLatin1(unique.value(hash).toBase64()));
                rows.append(row);
            }
            query.bindValue(0, kindName(kind));
            query.bindValue(1, QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));
            if (!query.exec())
            {
                setError(error, query.lastError().text());
                return std::nullopt;
            }
            query.finish();
        }
    }
    else
    {
        // A complete Merkle snapshot can contain tens of thousands of
        // immutable nodes/blobs. Batch SQLite insert statements too, while
        // respecting its historical 999-parameter limit (64 * 3 = 192).
        constexpr qsizetype sqlite_batch_size = 64;
        QSqlQuery query(database);
        for (qsizetype start = 0; start < ordered.size(); start += sqlite_batch_size)
        {
            const qsizetype count = qMin(sqlite_batch_size, ordered.size() - start);
            QString values;
            for (qsizetype index = 0; index < count; ++index)
                values += index == 0 ? QStringLiteral("(?,?,?)")
                                     : QStringLiteral(",(?,?,?)");
            if (!query.prepare(QStringLiteral(
                    "INSERT INTO content_objects(sha256,kind,payload) VALUES %1 "
                    "ON CONFLICT(sha256) DO NOTHING").arg(values)))
            {
                setError(error, query.lastError().text());
                return std::nullopt;
            }
            for (qsizetype index = 0; index < count; ++index)
            {
                const QByteArray &hash = ordered.at(start + index);
                const qsizetype base = index * 3;
                query.bindValue(base, hash);
                query.bindValue(base + 1, kindName(kind));
                query.bindValue(base + 2, unique.value(hash));
            }
            if (!query.exec())
            {
                setError(error, query.lastError().text());
                return std::nullopt;
            }
            query.finish();
        }
    }

    phases.mark("sql_insert", ordered.size());
    const std::optional<QHash<QByteArray, QByteArray>> verified =
        getMany(database, kind, ordered, error);
    if (!verified)
        return std::nullopt;
    for (const QByteArray &hash : ordered)
    {
        if (verified->value(hash) != unique.value(hash))
        {
            setError(error, QStringLiteral("Content digest collision or mismatched stored content"));
            return std::nullopt;
        }
    }
    phases.mark("verify_stored_content", ordered.size());
    return results;
}

std::optional<QByteArray> put(const QSqlDatabase &database, Kind kind,
                              const QByteArray &bytes, QString *error)
{
    // NULL and empty payloads are different in SQL. Always bind an actual
    // non-null byte array for empty content, avoiding SQLite/PG driver quirks.
    const QByteArray non_null_bytes = bytes.isNull() ? QByteArray("") : bytes;
    const QByteArray hash = digest(kind, non_null_bytes);
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "INSERT INTO content_objects(sha256,kind,payload) VALUES(?,?,?) "
            "ON CONFLICT(sha256) DO NOTHING")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(hash);
    query.addBindValue(kindName(kind));
    query.addBindValue(non_null_bytes);
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.finish();
    // An existing row with the same key must really contain the same bytes.
    const std::optional<QByteArray> actual = get(database, kind, hash, error);
    if (!actual || *actual != non_null_bytes)
    {
        if (actual)
            setError(error, QStringLiteral("Content digest collision or mismatched stored content"));
        return std::nullopt;
    }
    return hash;
}

} // namespace aowis::db::content_store
