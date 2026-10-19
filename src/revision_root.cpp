#include "revision_root.h"

#include "content_store.h"
#include "merkle_tree.h"
#include "storage_backend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <limits>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &text)
{
    if (error != nullptr)
        *error = text;
}

void appendU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(static_cast<char>((value >> shift) & 0xffU));
}

// This is the exact *logical object record* included under each UUID key.
// Unlike raw model bytes, the version and tombstone are part of the snapshot.
// No database-specific UUID or QVariant representation enters the hash.
QByteArray encodeRecord(const QString &kind, qint64 version,
                        bool deleted, const QByteArray &content)
{
    QByteArray result("AOWIS-MODEL-RECORD-V1\0", 22);
    const QByteArray name = kind.toUtf8();
    appendU64(result, static_cast<quint64>(name.size()));
    result.append(name);
    appendU64(result, static_cast<quint64>(version));
    result.append(deleted ? '\x01' : '\x00');
    if (!deleted)
    {
        appendU64(result, static_cast<quint64>(content.size()));
        result.append(content);
    }
    return result;
}

bool readU64(const QByteArray &bytes, qsizetype &offset, quint64 &result)
{
    if (offset < 0 || bytes.size() - offset < 8)
        return false;
    result = 0;
    for (int i = 0; i < 8; ++i)
        result = (result << 8) | static_cast<quint8>(bytes.at(offset++));
    return true;
}

std::optional<MerkleObjectRecord> decodeRecord(const QUuid &key, const QByteArray &bytes,
                                                QString *error)
{
    constexpr qsizetype magic_length = 22;
    if (bytes.size() < magic_length + 8 + 8 + 1 ||
        bytes.left(magic_length) != QByteArray("AOWIS-MODEL-RECORD-V1\0", magic_length))
    {
        setError(error, QStringLiteral("Invalid Merkle model record magic or length"));
        return std::nullopt;
    }
    qsizetype offset = magic_length;
    quint64 kind_length = 0;
    if (!readU64(bytes, offset, kind_length) || kind_length == 0 || kind_length > 128 ||
        kind_length > static_cast<quint64>(bytes.size() - offset))
    {
        setError(error, QStringLiteral("Invalid Merkle model record kind length"));
        return std::nullopt;
    }
    const QByteArray kind_bytes = bytes.mid(offset, static_cast<qsizetype>(kind_length));
    for (const char c : kind_bytes)
    {
        if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '.' && c != '_')
        {
            setError(error, QStringLiteral("Invalid Merkle model record kind"));
            return std::nullopt;
        }
    }
    offset += static_cast<qsizetype>(kind_length);
    quint64 version = 0;
    if (!readU64(bytes, offset, version) || version == 0 ||
        version > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        offset == bytes.size())
    {
        setError(error, QStringLiteral("Invalid Merkle model record version"));
        return std::nullopt;
    }
    const char flag = bytes.at(offset++);
    if (flag != '\x00' && flag != '\x01')
    {
        setError(error, QStringLiteral("Invalid Merkle model record deletion flag"));
        return std::nullopt;
    }
    MerkleObjectRecord record;
    record.uuid = key;
    record.kind = QString::fromLatin1(kind_bytes);
    record.version = static_cast<qint64>(version);
    record.deleted = flag == '\x01';
    if (record.deleted)
    {
        if (offset != bytes.size())
        {
            setError(error, QStringLiteral("Trailing bytes in tombstone Merkle record"));
            return std::nullopt;
        }
        return record;
    }
    quint64 content_length = 0;
    if (!readU64(bytes, offset, content_length) ||
        content_length != static_cast<quint64>(bytes.size() - offset))
    {
        setError(error, QStringLiteral("Invalid Merkle model record content length"));
        return std::nullopt;
    }
    record.content = bytes.mid(offset);
    if (record.content.isNull())
        record.content = QByteArray("");
    return record;
}

std::optional<MerkleObjectRecord> readLeaf(const QSqlDatabase &database,
                                            const QUuid &key, const QByteArray &digest,
                                            QString *error)
{
    const std::optional<QByteArray> bytes = content_store::get(
        database, content_store::Kind::Blob, digest, error);
    if (!bytes)
        return std::nullopt;
    return decodeRecord(key, *bytes, error);
}

bool prepareObjectScan(QSqlQuery &query, const StorageBackend &backend,
                       const QUuid &revision_uuid, QString *error)
{
    if (!query.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? ORDER BY object_uuid")))
    {
        setError(error, query.lastError().text());
        return false;
    }
    query.addBindValue(backend.bindUuid(revision_uuid));
    if (!query.exec())
    {
        setError(error, query.lastError().text());
        return false;
    }
    return true;
}

} // namespace

std::optional<QByteArray> buildRevisionRoot(const QSqlDatabase &database,
                                             const StorageBackend &backend,
                                             const QUuid &revision_uuid,
                                             const QByteArray &base_root,
                                             QString *error)
{
    QSqlQuery objects(database);
    if (base_root.isEmpty())
    {
        if (!prepareObjectScan(objects, backend, revision_uuid, error))
            return std::nullopt;
    }
    else
    {
        // All supported object mutations have append-only object_events.
        // New working revisions are copied from their first finalized parent.
        // Reusing that parent's Merkle root makes finalization proportional
        // to the changed UUIDs, not the full object count.
        if (!objects.prepare(QStringLiteral(
                "SELECT o.object_uuid,o.kind,o.content,o.version,o.deleted "
                "FROM revision_objects o WHERE o.revision_uuid=? AND o.object_uuid IN ("
                "SELECT DISTINCT object_uuid FROM object_events WHERE revision_uuid=?) "
                "ORDER BY o.object_uuid")))
        {
            setError(error, objects.lastError().text());
            return std::nullopt;
        }
        objects.addBindValue(backend.bindUuid(revision_uuid));
        objects.addBindValue(backend.bindUuid(revision_uuid));
        if (!objects.exec())
        {
            setError(error, objects.lastError().text());
            return std::nullopt;
        }
    }

    QVector<merkle_tree::Change> changes;
    QVector<QUuid> keys;
    QVector<QByteArray> payloads;
    while (objects.next())
    {
        const QUuid uuid = backend.readUuid(objects.value(0));
        if (uuid.isNull())
        {
            setError(error, QStringLiteral("Invalid UUID in revision object mapping"));
            return std::nullopt;
        }
        const QByteArray serialized = encodeRecord(
            objects.value(1).toString(), objects.value(3).toLongLong(),
            objects.value(4).toBool(), objects.value(2).toByteArray());
        keys.append(uuid);
        payloads.append(serialized);
    }
    if (objects.lastError().isValid())
    {
        setError(error, objects.lastError().text());
        return std::nullopt;
    }
    objects.finish();

    const std::optional<QVector<QByteArray>> hashes = content_store::putMany(
        database, content_store::Kind::Blob, payloads, error);
    if (!hashes)
        return std::nullopt;
    changes.reserve(keys.size());
    for (qsizetype i = 0; i < keys.size(); ++i)
        changes.append(merkle_tree::Change{keys.at(i), hashes->at(i)});

    if (!base_root.isEmpty())
        return merkle_tree::apply(database, base_root, changes, error);
    return merkle_tree::build(database, changes, error);
}

std::optional<MerkleObjectRecord> readRevisionObject(const QSqlDatabase &database,
                                                       const QByteArray &root,
                                                       const QUuid &object_uuid,
                                                       QString *error)
{
    const std::optional<QByteArray> digest = merkle_tree::lookup(
        database, root, object_uuid, error);
    if (!digest)
    {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("Object not found");
        return std::nullopt;
    }
    return readLeaf(database, object_uuid, *digest, error);
}

std::optional<QVector<MerkleObjectRecord>> readRevisionObjects(const QSqlDatabase &database,
                                                                 const QByteArray &root,
                                                                 QString *error)
{
    const std::optional<QVector<merkle_tree::Entry>> entries = merkle_tree::entries(
        database, root, error);
    if (!entries)
        return std::nullopt;
    QVector<QByteArray> digests;
    digests.reserve(entries->size());
    for (const merkle_tree::Entry &entry : *entries)
        digests.append(entry.blob_sha256);
    const std::optional<QHash<QByteArray, QByteArray>> payloads =
        content_store::getMany(database, content_store::Kind::Blob, digests, error);
    if (!payloads)
        return std::nullopt;
    QVector<MerkleObjectRecord> result;
    result.reserve(entries->size());
    for (const merkle_tree::Entry &entry : *entries)
    {
        const std::optional<MerkleObjectRecord> record = decodeRecord(
            entry.key, payloads->value(entry.blob_sha256), error);
        if (!record)
            return std::nullopt;
        result.append(*record);
    }
    return result;
}

std::optional<QVector<MerkleObjectRecord>> readRevisionObjectEntries(
    const QSqlDatabase &database, const QVector<merkle_tree::Entry> &entries,
    QString *error)
{
    QVector<MerkleObjectRecord> result;
    if (entries.isEmpty())
        return result;
    QVector<QByteArray> digests;
    digests.reserve(entries.size());
    for (const merkle_tree::Entry &entry : entries)
        digests.append(entry.blob_sha256);
    const std::optional<QHash<QByteArray, QByteArray>> payloads =
        content_store::getMany(database, content_store::Kind::Blob, digests, error);
    if (!payloads)
        return std::nullopt;
    result.reserve(entries.size());
    for (const merkle_tree::Entry &entry : entries)
    {
        const std::optional<MerkleObjectRecord> record = decodeRecord(
            entry.key, payloads->value(entry.blob_sha256), error);
        if (!record)
            return std::nullopt;
        result.append(*record);
    }
    return result;
}

std::optional<QVector<MerkleObjectRecord>> readRevisionObjectsSelected(
    const QSqlDatabase &database, const QByteArray &root,
    const QSet<QUuid> &requested, QString *error)
{
    if (error != nullptr)
        error->clear();
    QVector<MerkleObjectRecord> result;
    if (requested.isEmpty())
        return result;

    const std::optional<QVector<merkle_tree::Entry>> entries =
        merkle_tree::entries(database, root, error);
    if (!entries)
        return std::nullopt;

    QVector<merkle_tree::Entry> selected;
    QVector<QByteArray> hashes;
    selected.reserve(requested.size());
    hashes.reserve(requested.size());
    for (const merkle_tree::Entry &entry : *entries)
    {
        if (requested.contains(entry.key))
        {
            selected.append(entry);
            hashes.append(entry.blob_sha256);
        }
    }
    if (selected.size() != requested.size())
    {
        setError(error, QStringLiteral("Referenced model object missing from Merkle snapshot"));
        return std::nullopt;
    }
    const std::optional<QHash<QByteArray, QByteArray>> payloads =
        content_store::getMany(database, content_store::Kind::Blob, hashes, error);
    if (!payloads)
        return std::nullopt;
    result.reserve(selected.size());
    for (const merkle_tree::Entry &entry : selected)
    {
        const std::optional<MerkleObjectRecord> record = decodeRecord(
            entry.key, payloads->value(entry.blob_sha256), error);
        if (!record)
            return std::nullopt;
        result.append(*record);
    }
    return result;
}

bool materializeWorkingObjectsFromRoot(const QSqlDatabase &database,
                                       const StorageBackend &backend,
                                       const QUuid &working_revision_uuid,
                                       const QByteArray &finalized_root,
                                       QString *error)
{
    if (working_revision_uuid.isNull() || finalized_root.size() != 32)
    {
        setError(error, QStringLiteral("Invalid working revision or finalized Merkle root"));
        return false;
    }
    // Reject accidental misuse: the only supported target is a brand-new
    // working revision. A caller must not append to an existing materialization.
    QSqlQuery destination(database);
    if (!destination.prepare(QStringLiteral(
            "SELECT state FROM revisions WHERE uuid=?")))
    {
        setError(error, destination.lastError().text());
        return false;
    }
    destination.addBindValue(backend.bindUuid(working_revision_uuid));
    if (!destination.exec() || !destination.next() ||
        destination.value(0).toString() != QStringLiteral("working"))
    {
        setError(error, QStringLiteral("Merkle materialization target must be a working revision"));
        return false;
    }
    destination.finish();
    if (!destination.prepare(QStringLiteral(
            "SELECT 1 FROM revision_objects WHERE revision_uuid=?")))
    {
        setError(error, destination.lastError().text());
        return false;
    }
    destination.addBindValue(backend.bindUuid(working_revision_uuid));
    if (!destination.exec())
    {
        setError(error, destination.lastError().text());
        return false;
    }
    if (destination.next())
    {
        setError(error, QStringLiteral("Merkle materialization target is not empty"));
        return false;
    }
    destination.finish();
    // A fresh working head inherits exact object versions and tombstones, but
    // no historical SQL object rows are required. All leaf hashes are checked.
    const std::optional<QVector<merkle_tree::Entry>> entries =
        merkle_tree::entries(database, finalized_root, error);
    if (!entries)
        return false;

    const bool postgres = database.driverName() == QStringLiteral("QPSQL");
    QSqlQuery insert(database);
    if (postgres)
    {
        if (!insert.prepare(QStringLiteral(
                "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
                "SELECT CAST(? AS uuid), CAST(src.object_uuid AS uuid), src.kind, "
                "CASE WHEN src.deleted=1 THEN NULL ELSE decode(src.content_b64,'base64') END, "
                "src.version, src.deleted FROM jsonb_to_recordset(CAST(? AS jsonb)) "
                "AS src(object_uuid text,kind text,content_b64 text,version bigint,deleted bigint)")))
        {
            setError(error, insert.lastError().text());
            return false;
        }
    }
    // SQLite uses multi-row INSERTs as well. Avoid one prepared-statement
    // execution per object and keep below SQLite's old 999-bind limit.
    constexpr qsizetype batch_size = 256;
    constexpr qsizetype sqlite_insert_batch = 64; // 64 * 6 = 384 bind parameters

    for (qsizetype start = 0; start < entries->size(); start += batch_size)
    {
        const QVector<merkle_tree::Entry> portion =
            entries->mid(start, qMin(batch_size, entries->size() - start));
        const std::optional<QVector<MerkleObjectRecord>> records =
            readRevisionObjectEntries(database, portion, error);
        if (!records)
            return false;
        if (postgres)
        {
            QJsonArray rows;
            for (const MerkleObjectRecord &record : *records)
            {
                QJsonObject row;
                row.insert(QStringLiteral("object_uuid"),
                           record.uuid.toString(QUuid::WithoutBraces));
                row.insert(QStringLiteral("kind"), record.kind);
                row.insert(QStringLiteral("content_b64"),
                           QString::fromLatin1(record.content.toBase64()));
                // JSON numbers can lose precision above 2^53; transmit the
                // version as an exact decimal string for the bigint column.
                row.insert(QStringLiteral("version"), QString::number(record.version));
                row.insert(QStringLiteral("deleted"), record.deleted ? 1 : 0);
                rows.append(row);
            }
            insert.bindValue(0, backend.bindUuid(working_revision_uuid));
            insert.bindValue(1, QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));
            if (!insert.exec() || insert.numRowsAffected() != records->size())
            {
                setError(error, insert.lastError().text().isEmpty()
                    ? QStringLiteral("Merkle working-object batch insert count mismatch")
                    : insert.lastError().text());
                return false;
            }
            insert.finish();
        }
        else
        {
            for (qsizetype offset = 0; offset < records->size(); offset += sqlite_insert_batch)
            {
                const qsizetype count = qMin(sqlite_insert_batch, records->size() - offset);
                QString placeholders;
                for (qsizetype item = 0; item < count; ++item)
                    placeholders += item == 0 ? QStringLiteral("(?,?,?,?,?,?)")
                                              : QStringLiteral(",(?,?,?,?,?,?)");
                if (!insert.prepare(QStringLiteral(
                        "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
                        "VALUES %1").arg(placeholders)))
                {
                    setError(error, insert.lastError().text());
                    return false;
                }
                for (qsizetype item = 0; item < count; ++item)
                {
                    const MerkleObjectRecord &record = records->at(offset + item);
                    const qsizetype base = item * 6;
                    insert.bindValue(base, backend.bindUuid(working_revision_uuid));
                    insert.bindValue(base + 1, backend.bindUuid(record.uuid));
                    insert.bindValue(base + 2, record.kind);
                    insert.bindValue(base + 3, record.deleted ? QVariant() : QVariant(record.content));
                    insert.bindValue(base + 4, record.version);
                    insert.bindValue(base + 5, record.deleted ? 1 : 0);
                }
                if (!insert.exec() || insert.numRowsAffected() != count)
                {
                    setError(error, insert.lastError().text().isEmpty()
                        ? QStringLiteral("Merkle SQLite working-object batch insert count mismatch")
                        : insert.lastError().text());
                    return false;
                }
                insert.finish();
            }
        }
    }
    return true;
}

bool verifyWorkingObjectsAgainstRoot(const QSqlDatabase &database,
                        const StorageBackend &backend,
                        const QUuid &revision_uuid,
                        const QByteArray &root,
                        QString *error)
{
    const std::optional<QVector<merkle_tree::Entry>> entries =
        merkle_tree::entries(database, root, error);
    if (!entries)
        return false;
    QSqlQuery objects(database);
    if (!prepareObjectScan(objects, backend, revision_uuid, error))
        return false;

    qsizetype index = 0;
    while (objects.next())
    {
        const QUuid uuid = backend.readUuid(objects.value(0));
        if (index >= entries->size() || entries->at(index).key != uuid)
        {
            setError(error, QStringLiteral("Revision Merkle object membership differs from SQL state"));
            return false;
        }
        const QByteArray record = encodeRecord(
            objects.value(1).toString(), objects.value(3).toLongLong(),
            objects.value(4).toBool(), objects.value(2).toByteArray());
        const QByteArray expected_hash = content_store::digest(content_store::Kind::Blob, record);
        if (entries->at(index).blob_sha256 != expected_hash)
        {
            setError(error, QStringLiteral("Revision Merkle object contents differ from SQL state"));
            return false;
        }
        const std::optional<QByteArray> sealed = content_store::get(
            database, content_store::Kind::Blob, expected_hash, error);
        if (!sealed || *sealed != record)
        {
            if (sealed)
                setError(error, QStringLiteral("Revision Merkle object payload differs from SQL state"));
            return false;
        }
        ++index;
    }
    if (objects.lastError().isValid())
    {
        setError(error, objects.lastError().text());
        return false;
    }
    if (index != entries->size())
    {
        setError(error, QStringLiteral("Revision Merkle root contains extra objects"));
        return false;
    }
    return true;
}

bool verifyRevisionRootCached(const QSqlDatabase &database,
    const QByteArray &root, QHash<QByteArray, QByteArray> &verified_nodes,
    QSet<QByteArray> &verified_blobs, QString *error)
{
    // The tree is still traversed separately for every root, preserving
    // per-revision path and shape checks. Shared node payloads and model blobs
    // are loaded and SHA-256 checked just once per verification request.
    const std::optional<QVector<merkle_tree::Entry>> entries =
        merkle_tree::entriesCached(database, root, verified_nodes, error);
    if (!entries)
        return false;
    constexpr qsizetype batch_size = 256;
    QVector<merkle_tree::Entry> new_entries;
    new_entries.reserve(batch_size);
    for (const merkle_tree::Entry &entry : *entries)
    {
        if (verified_blobs.contains(entry.blob_sha256))
            continue;
        new_entries.append(entry);
        if (new_entries.size() == batch_size)
        {
            if (!readRevisionObjectEntries(database, new_entries, error))
                return false;
            for (const merkle_tree::Entry &verified : new_entries)
                verified_blobs.insert(verified.blob_sha256);
            new_entries.clear();
        }
    }
    if (!new_entries.isEmpty())
    {
        if (!readRevisionObjectEntries(database, new_entries, error))
            return false;
        for (const merkle_tree::Entry &verified : new_entries)
            verified_blobs.insert(verified.blob_sha256);
    }
    return true;
}

bool verifyRevisionRoot(const QSqlDatabase &database,
                        const QByteArray &root, QString *error)
{
    QHash<QByteArray, QByteArray> verified_nodes;
    QSet<QByteArray> verified_blobs;
    return verifyRevisionRootCached(database, root, verified_nodes,
                                    verified_blobs, error);
}

} // namespace aowis::db
