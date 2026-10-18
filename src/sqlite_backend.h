#ifndef AOWIS_DB_SQLITE_BACKEND_H
#define AOWIS_DB_SQLITE_BACKEND_H

#include "storage_backend.h"

namespace aowis::db
{
class SqliteBackend final : public StorageBackend
{
public:
    QVariant bindUuid(const QUuid &uuid) const override;
    QUuid readUuid(const QVariant &value) const override;
    bool open(QSqlDatabase &database, const QString &connection_name,
              const QString &file_path, QString *error) const override;
    bool configure(const QSqlDatabase &database, QString *error) const override;
    bool verifySchema(const QSqlDatabase &database, QString *error) const override;
    bool createSchema(const QSqlDatabase &database, QString *error) const override;
    std::optional<bool> hasSchema(const QSqlDatabase &database, QString *error) const override;
    bool beginWrite(const QSqlDatabase &database, QString *error) const override;
    bool commit(const QSqlDatabase &database, QString *error) const override;
    void rollback(const QSqlDatabase &database) const override;
    std::optional<bool> hasColumn(const QSqlDatabase &database, const QString &table,
                                   const QString &column, QString *error) const override;
};
}

#endif // AOWIS_DB_SQLITE_BACKEND_H
