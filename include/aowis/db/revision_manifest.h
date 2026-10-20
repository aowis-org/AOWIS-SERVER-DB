#ifndef AOWIS_DB_REVISION_MANIFEST_H
#define AOWIS_DB_REVISION_MANIFEST_H

#include <optional>

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QUuid>
#include <QVector>

namespace aowis::db
{
// Backend-independent description of ONE sealed revision. This is metadata,
// not a clone bundle: the snapshot, journal and Merkle content must be
// transferred and verified separately before an imported head is published.
// Parent hashes are in their original DAG order, including merge parents.
struct RevisionManifest
{
    QUuid project_uuid;
    QUuid branch_uuid;
    QUuid revision_uuid;
    QUuid created_by_uuid;
    QUuid finalized_by_uuid;
    qint64 created_at_ms = 0;
    qint64 number = 0;
    qint64 finalized_at_ms = 0;
    QString title;
    QString message;
    QByteArray snapshot_sha256;
    QByteArray operation_history_sha256;
    // Object-mode only; absent for opaque snapshots. The canonical chain
    // uses 32 zero bytes for the absent root in opaque mode.
    QByteArray object_root_sha256;
    QByteArray chain_sha256;
    bool object_mode = false;
    QVector<QPair<QUuid, QByteArray>> parents;
};

// EXACT historical AOWIS-REVISION-DAG-V1 hash contract, reused by the local
// finalizer and portable-manifest verification. Never hash the transport frame
// instead of this digest or change the meaning of existing revision hashes.
QByteArray revisionManifestChainHash(const QUuid &project_uuid,
    const QUuid &branch_uuid, const QUuid &revision_uuid,
    const QVector<QPair<QUuid, QByteArray>> &parents, qint64 number,
    qint64 created_at_ms, qint64 finalized_at_ms, const QString &title,
    const QString &message, const QByteArray &snapshot_hash,
    const QUuid &creator_uuid, const QUuid &finalizer_uuid,
    const QByteArray &history_hash, const QByteArray &object_root);

// Canonical V1 binary frame. Rejects malformed, non-canonical, tampered,
// truncated and oversized data; verifies the existing revision-chain hash.
// This does NOT verify that separately transferred content exists or that the
// listed parent revisions are actually present in a receiving repository.
std::optional<QByteArray> encodeRevisionManifest(const RevisionManifest &manifest,
                                                  QString *error = nullptr);
std::optional<RevisionManifest> decodeRevisionManifest(const QByteArray &bytes,
                                                        QString *error = nullptr);
// Verify the EXACT historical AOWIS-OPERATIONS-V1 preimage against a
// finalized manifest's revision UUID and operation-history SHA-256. This
// validates the frame without inserting or trusting any remote SQL rows.
// The operation journal is distinct from the object-event/group journal.
bool verifyRevisionOperationJournal(const QByteArray &frame,
    const QUuid &revision_uuid, const QByteArray &expected_sha256,
    QString *error = nullptr);

} // namespace aowis::db

#endif // AOWIS_DB_REVISION_MANIFEST_H
