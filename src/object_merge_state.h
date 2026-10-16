#ifndef AOWIS_DB_OBJECT_MERGE_STATE_H
#define AOWIS_DB_OBJECT_MERGE_STATE_H

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>

#include <optional>

namespace aowis::db
{
// Physical state includes tombstones and version counters, not just live
// semantic content. Used only for transient preview-to-commit CAS checks.
std::optional<QByteArray> mergePhysicalObjectDigest(const QSqlDatabase &database,
    const QUuid &revision_uuid, QString *error);
}

#endif // AOWIS_DB_OBJECT_MERGE_STATE_H
