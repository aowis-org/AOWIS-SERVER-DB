#ifndef AOWIS_DB_WORKING_STORAGE_H
#define AOWIS_DB_WORKING_STORAGE_H

#include <optional>

#include <QSqlDatabase>
#include <QString>
#include <QUuid>

namespace aowis::db
{
class StorageBackend;

// Map the CURRENT branch-head revision to its permanent mutable storage key.
// No historical revision can resolve to a working storage identity, even if
// it belongs to the same branch. Call under the caller's read/write transaction;
// do not cache this mapping across transactions or branch-head changes.
std::optional<QUuid> resolveWorkingStorageUuid(const QSqlDatabase &database,
    const StorageBackend &backend, const QUuid &working_revision_uuid,
    QString *error = nullptr);
}

#endif // AOWIS_DB_WORKING_STORAGE_H
