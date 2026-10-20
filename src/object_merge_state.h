#ifndef AOWIS_DB_OBJECT_MERGE_STATE_H
#define AOWIS_DB_OBJECT_MERGE_STATE_H

#include "storage_backend.h"

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>

#include <optional>

namespace aowis::db
{
// A working revision is fingerprinted from every physical SQL object row,
// including tombstones and versions, for preview-to-commit CAS checks.
// For a finalized revision (nonempty sealed_root), return a domain-separated
// fingerprint of the verified immutable Merkle root instead. The latter is
// transient preview metadata, not a whole-tree corruption audit; finalized
// revisions can never be merge-commit working targets.
std::optional<QByteArray> mergePhysicalObjectDigest(const QSqlDatabase &database, const StorageBackend &backend,
    const QUuid &revision_uuid, const QByteArray &sealed_root, QString *error);
}

#endif // AOWIS_DB_OBJECT_MERGE_STATE_H
