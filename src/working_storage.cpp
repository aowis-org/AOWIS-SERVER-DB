#include "working_storage.h"

#include "storage_backend.h"

#include <QSqlError>
#include <QSqlQuery>

namespace aowis::db
{
std::optional<QUuid> resolveWorkingStorageUuid(const QSqlDatabase &database,
    const StorageBackend &backend, const QUuid &working_revision_uuid,
    QString *error)
{
    if (working_revision_uuid.isNull())
    {
        if (error != nullptr)
            *error = QStringLiteral("Invalid working revision UUID");
        return std::nullopt;
    }

    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT b.working_storage_uuid FROM revisions r "
            "JOIN branches b ON b.uuid=r.branch_uuid "
            "AND b.project_uuid=r.project_uuid "
            "AND b.head_revision_uuid=r.uuid "
            "WHERE r.uuid=? AND r.state='working'")))
    {
        if (error != nullptr)
            *error = query.lastError().text();
        return std::nullopt;
    }
    query.addBindValue(backend.bindUuid(working_revision_uuid));
    if (!query.exec())
    {
        if (error != nullptr)
            *error = query.lastError().text();
        return std::nullopt;
    }
    if (!query.next())
    {
        if (error != nullptr)
            *error = QStringLiteral("Revision is not the current working branch head");
        return std::nullopt;
    }
    const QUuid storage_uuid = backend.readUuid(query.value(0));
    if (storage_uuid.isNull())
    {
        if (error != nullptr)
            *error = QStringLiteral("Invalid branch working-storage UUID");
        return std::nullopt;
    }
    return storage_uuid;
}
}
