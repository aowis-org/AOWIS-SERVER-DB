#include <aowis/db/revision_manifest.h>

#include <limits>

#include <QCryptographicHash>
#include <QSet>

namespace aowis::db
{
namespace
{
constexpr int digest_length = 32;
constexpr qsizetype max_frame = 16 * 1024 * 1024;
constexpr quint64 max_text_length = 8 * 1024 * 1024;
constexpr quint64 max_parents = 64;
constexpr char manifest_magic[] = "AOWIS-REVISION-MANIFEST-V1";

void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

void appendU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(QByteArray &out, const QByteArray &data)
{
    appendU64(out, static_cast<quint64>(data.size()));
    out.append(data);
}

bool readBytes(const QByteArray &bytes, qsizetype &offset, qsizetype count,
               QByteArray &result)
{
    if (count < 0 || offset < 0 || offset > bytes.size() || count > bytes.size() - offset)
        return false;
    result = bytes.mid(offset, count);
    offset += count;
    return true;
}

bool readU64(const QByteArray &bytes, qsizetype &offset, quint64 &result)
{
    QByteArray raw;
    if (!readBytes(bytes, offset, 8, raw))
        return false;
    result = 0;
    for (const char value : raw)
        result = (result << 8) | static_cast<quint8>(value);
    return true;
}

bool readUuid(const QByteArray &bytes, qsizetype &offset, QUuid &result)
{
    QByteArray raw;
    if (!readBytes(bytes, offset, 16, raw))
        return false;
    result = QUuid::fromRfc4122(raw);
    return true;
}

bool readString(const QByteArray &bytes, qsizetype &offset, QString &result)
{
    quint64 length = 0;
    if (!readU64(bytes, offset, length) || length > max_text_length ||
        length > static_cast<quint64>(bytes.size() - offset))
        return false;
    QByteArray raw;
    if (!readBytes(bytes, offset, static_cast<qsizetype>(length), raw))
        return false;
    result = QString::fromUtf8(raw);
    return result.toUtf8() == raw; // Strict UTF-8, no replacement characters.
}

bool valid(const RevisionManifest &manifest, QString *error)
{
    if (manifest.project_uuid.isNull() || manifest.branch_uuid.isNull() ||
        manifest.revision_uuid.isNull() || manifest.number < 1 ||
        manifest.created_at_ms < 0 || manifest.finalized_at_ms < 0 ||
        manifest.snapshot_sha256.size() != digest_length ||
        manifest.operation_history_sha256.size() != digest_length ||
        manifest.chain_sha256.size() != digest_length ||
        manifest.parents.size() > static_cast<qsizetype>(max_parents))
    {
        setError(error, QStringLiteral("Invalid finalized revision manifest metadata"));
        return false;
    }
    if ((manifest.object_mode && manifest.object_root_sha256.size() != digest_length) ||
        (!manifest.object_mode && !manifest.object_root_sha256.isEmpty()))
    {
        setError(error, QStringLiteral("Invalid manifest object-mode root"));
        return false;
    }
    const QByteArray title = manifest.title.toUtf8();
    const QByteArray message = manifest.message.toUtf8();
    if (title.size() > static_cast<qsizetype>(max_text_length) ||
        message.size() > static_cast<qsizetype>(max_text_length) ||
        QString::fromUtf8(title) != manifest.title ||
        QString::fromUtf8(message) != manifest.message)
    {
        setError(error, QStringLiteral("Invalid manifest UTF-8 text"));
        return false;
    }
    QSet<QUuid> seen;
    for (const QPair<QUuid, QByteArray> &parent : manifest.parents)
    {
        if (parent.first.isNull() || parent.first == manifest.revision_uuid ||
            parent.second.size() != digest_length || seen.contains(parent.first))
        {
            setError(error, QStringLiteral("Invalid or repeated manifest parent"));
            return false;
        }
        seen.insert(parent.first);
    }
    const QByteArray object_root = manifest.object_mode
        ? manifest.object_root_sha256 : QByteArray(digest_length, '\0');
    if (revisionManifestChainHash(manifest.project_uuid, manifest.branch_uuid,
            manifest.revision_uuid, manifest.parents, manifest.number,
            manifest.created_at_ms, manifest.finalized_at_ms, manifest.title,
            manifest.message, manifest.snapshot_sha256, manifest.created_by_uuid,
            manifest.finalized_by_uuid, manifest.operation_history_sha256,
            object_root) != manifest.chain_sha256)
    {
        setError(error, QStringLiteral("Revision manifest chain SHA-256 mismatch"));
        return false;
    }
    return true;
}
} // namespace

QByteArray revisionManifestChainHash(const QUuid &project_uuid,
    const QUuid &branch_uuid, const QUuid &revision_uuid,
    const QVector<QPair<QUuid, QByteArray>> &parents, qint64 number,
    qint64 created_at_ms, qint64 finalized_at_ms, const QString &title,
    const QString &message, const QByteArray &snapshot_hash,
    const QUuid &creator_uuid, const QUuid &finalizer_uuid,
    const QByteArray &history_hash, const QByteArray &object_root)
{
    if (snapshot_hash.size() != digest_length || history_hash.size() != digest_length ||
        object_root.size() != digest_length)
        return {};
    QByteArray bytes("AOWIS-REVISION-DAG-V1\0", 22);
    bytes.append(project_uuid.toRfc4122());
    bytes.append(branch_uuid.toRfc4122());
    bytes.append(revision_uuid.toRfc4122());
    bytes.append(creator_uuid.toRfc4122());
    bytes.append(finalizer_uuid.toRfc4122());
    appendU64(bytes, static_cast<quint64>(created_at_ms));
    appendU64(bytes, static_cast<quint64>(number));
    appendU64(bytes, static_cast<quint64>(finalized_at_ms));
    appendField(bytes, title.toUtf8());
    appendField(bytes, message.toUtf8());
    bytes.append(snapshot_hash);
    bytes.append(history_hash);
    bytes.append(object_root);
    appendU64(bytes, static_cast<quint64>(parents.size()));
    for (const QPair<QUuid, QByteArray> &parent : parents)
    {
        bytes.append(parent.first.toRfc4122());
        bytes.append(parent.second);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

std::optional<QByteArray> encodeRevisionManifest(const RevisionManifest &manifest,
                                                  QString *error)
{
    if (!valid(manifest, error))
        return std::nullopt;
    QByteArray bytes(manifest_magic, sizeof(manifest_magic)); // Include NUL delimiter.
    bytes.append(manifest.object_mode ? '\x01' : '\x00');
    bytes.append(manifest.project_uuid.toRfc4122());
    bytes.append(manifest.branch_uuid.toRfc4122());
    bytes.append(manifest.revision_uuid.toRfc4122());
    bytes.append(manifest.created_by_uuid.toRfc4122());
    bytes.append(manifest.finalized_by_uuid.toRfc4122());
    appendU64(bytes, static_cast<quint64>(manifest.created_at_ms));
    appendU64(bytes, static_cast<quint64>(manifest.number));
    appendU64(bytes, static_cast<quint64>(manifest.finalized_at_ms));
    appendField(bytes, manifest.title.toUtf8());
    appendField(bytes, manifest.message.toUtf8());
    bytes.append(manifest.snapshot_sha256);
    bytes.append(manifest.operation_history_sha256);
    bytes.append(manifest.object_mode ? manifest.object_root_sha256 : QByteArray(digest_length, '\0'));
    bytes.append(manifest.chain_sha256);
    appendU64(bytes, static_cast<quint64>(manifest.parents.size()));
    for (const QPair<QUuid, QByteArray> &parent : manifest.parents)
    {
        bytes.append(parent.first.toRfc4122());
        bytes.append(parent.second);
    }
    if (bytes.size() > max_frame)
    {
        setError(error, QStringLiteral("Revision manifest exceeds frame size limit"));
        return std::nullopt;
    }
    return bytes;
}

std::optional<RevisionManifest> decodeRevisionManifest(const QByteArray &bytes,
                                                        QString *error)
{
    const qsizetype magic_length = static_cast<qsizetype>(sizeof(manifest_magic));
    if (bytes.size() < magic_length + 1 || bytes.size() > max_frame ||
        bytes.left(magic_length) != QByteArray(manifest_magic, sizeof(manifest_magic)))
    {
        setError(error, QStringLiteral("Invalid revision manifest framing"));
        return std::nullopt;
    }
    qsizetype offset = magic_length;
    const char mode = bytes.at(offset++);
    if (mode != '\x00' && mode != '\x01')
    {
        setError(error, QStringLiteral("Invalid revision manifest mode"));
        return std::nullopt;
    }
    RevisionManifest manifest;
    manifest.object_mode = mode == '\x01';
    quint64 created_at = 0;
    quint64 number = 0;
    quint64 finalized_at = 0;
    QByteArray object_root;
    if (!readUuid(bytes, offset, manifest.project_uuid) ||
        !readUuid(bytes, offset, manifest.branch_uuid) ||
        !readUuid(bytes, offset, manifest.revision_uuid) ||
        !readUuid(bytes, offset, manifest.created_by_uuid) ||
        !readUuid(bytes, offset, manifest.finalized_by_uuid) ||
        !readU64(bytes, offset, created_at) ||
        !readU64(bytes, offset, number) ||
        !readU64(bytes, offset, finalized_at) ||
        created_at > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        number > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        finalized_at > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        !readString(bytes, offset, manifest.title) ||
        !readString(bytes, offset, manifest.message) ||
        !readBytes(bytes, offset, digest_length, manifest.snapshot_sha256) ||
        !readBytes(bytes, offset, digest_length, manifest.operation_history_sha256) ||
        !readBytes(bytes, offset, digest_length, object_root) ||
        !readBytes(bytes, offset, digest_length, manifest.chain_sha256))
    {
        setError(error, QStringLiteral("Truncated or malformed revision manifest"));
        return std::nullopt;
    }
    manifest.created_at_ms = static_cast<qint64>(created_at);
    manifest.number = static_cast<qint64>(number);
    manifest.finalized_at_ms = static_cast<qint64>(finalized_at);
    if (manifest.object_mode)
        manifest.object_root_sha256 = object_root;
    else if (object_root != QByteArray(digest_length, '\0'))
    {
        setError(error, QStringLiteral("Noncanonical opaque manifest root"));
        return std::nullopt;
    }
    quint64 count = 0;
    if (!readU64(bytes, offset, count) || count > max_parents ||
        count > static_cast<quint64>((bytes.size() - offset) / (16 + digest_length)))
    {
        setError(error, QStringLiteral("Invalid revision manifest parent count"));
        return std::nullopt;
    }
    for (quint64 index = 0; index < count; ++index)
    {
        QUuid uuid;
        QByteArray hash;
        if (!readUuid(bytes, offset, uuid) || !readBytes(bytes, offset, digest_length, hash))
        {
            setError(error, QStringLiteral("Truncated revision manifest parent"));
            return std::nullopt;
        }
        manifest.parents.append(qMakePair(uuid, hash));
    }
    if (offset != bytes.size())
    {
        setError(error, QStringLiteral("Trailing bytes in revision manifest"));
        return std::nullopt;
    }
    if (!valid(manifest, error))
        return std::nullopt;
    return manifest;
}
// The history digest has ALWAYS hashed these exact bytes, including the
// 20-byte magic, count, event ordering and nullable snapshot flags. Validate
// that preimage without changing the historical hash or loading SQL data.
bool verifyRevisionOperationJournal(const QByteArray &frame,
    const QUuid &revision_uuid, const QByteArray &expected_sha256,
    QString *error)
{
    constexpr char journal_magic[] = "AOWIS-OPERATIONS-V1";
    constexpr qsizetype max_journal_frame = 256 * 1024 * 1024;
    if (revision_uuid.isNull() || expected_sha256.size() != digest_length ||
        frame.size() < static_cast<qsizetype>(sizeof(journal_magic)) + 8 ||
        frame.size() > max_journal_frame ||
        frame.left(sizeof(journal_magic)) != QByteArray(journal_magic, sizeof(journal_magic)))
    {
        setError(error, QStringLiteral("Invalid portable operation journal framing"));
        return false;
    }
    if (QCryptographicHash::hash(frame, QCryptographicHash::Sha256) != expected_sha256)
    {
        setError(error, QStringLiteral("Portable operation journal history SHA-256 mismatch"));
        return false;
    }
    qsizetype offset = sizeof(journal_magic);
    quint64 count = 0;
    if (!readU64(frame, offset, count) ||
        count > static_cast<quint64>((frame.size() - offset) / (16 * 4 + 8 * 3 + 1 * 2)))
    {
        setError(error, QStringLiteral("Invalid portable operation count"));
        return false;
    }
    QSet<QUuid> event_uuids;
    for (quint64 index = 0; index < count; ++index)
    {
        QUuid event_uuid;
        QUuid actor_uuid;
        QUuid event_revision_uuid;
        QUuid target_uuid;
        quint64 sequence = 0;
        quint64 occurred_at = 0;
        QString kind;
        QByteArray flag;
        QByteArray payload;
        if (!readUuid(frame, offset, event_uuid) ||
            !readUuid(frame, offset, actor_uuid) ||
            !readUuid(frame, offset, event_revision_uuid) ||
            !readU64(frame, offset, sequence) ||
            !readU64(frame, offset, occurred_at) ||
            !readString(frame, offset, kind) ||
            !readUuid(frame, offset, target_uuid) ||
            !readBytes(frame, offset, 1, flag))
        {
            setError(error, QStringLiteral("Truncated portable operation journal entry"));
            return false;
        }
        if (event_uuid.isNull() || actor_uuid.isNull() ||
            event_revision_uuid != revision_uuid || event_uuids.contains(event_uuid) ||
            sequence != index + 1 ||
            occurred_at > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
            (kind != QStringLiteral("replace") && kind != QStringLiteral("undo") &&
             kind != QStringLiteral("redo")) ||
            (kind == QStringLiteral("replace") && !target_uuid.isNull()) ||
            (kind != QStringLiteral("replace") && target_uuid.isNull()) ||
            (flag.at(0) != '\x00' && flag.at(0) != '\x01'))
        {
            setError(error, QStringLiteral("Invalid portable operation journal event metadata"));
            return false;
        }
        event_uuids.insert(event_uuid);
        if (flag.at(0) == '\x01')
        {
            quint64 length = 0;
            if (!readU64(frame, offset, length) ||
                length > static_cast<quint64>(frame.size() - offset) ||
                !readBytes(frame, offset, static_cast<qsizetype>(length), payload))
            {
                setError(error, QStringLiteral("Invalid before-snapshot in operation journal"));
                return false;
            }
        }
        if (!readBytes(frame, offset, 1, flag) ||
            (flag.at(0) != '\x00' && flag.at(0) != '\x01'))
        {
            setError(error, QStringLiteral("Invalid after-snapshot flag in operation journal"));
            return false;
        }
        if (flag.at(0) == '\x01')
        {
            quint64 length = 0;
            if (!readU64(frame, offset, length) ||
                length > static_cast<quint64>(frame.size() - offset) ||
                !readBytes(frame, offset, static_cast<qsizetype>(length), payload))
            {
                setError(error, QStringLiteral("Invalid after-snapshot in operation journal"));
                return false;
            }
        }
    }
    if (offset != frame.size())
    {
        setError(error, QStringLiteral("Trailing bytes in portable operation journal"));
        return false;
    }
    return true;
}

} // namespace aowis::db
