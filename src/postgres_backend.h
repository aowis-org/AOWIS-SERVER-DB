#ifndef AOWIS_DB_POSTGRES_BACKEND_H
#define AOWIS_DB_POSTGRES_BACKEND_H

#include "storage_backend.h"

namespace aowis::db
{
// PostgreSQL backend with the current fresh schema and integrity guards.
// The selected schema is explicit so callers (and tests) never accidentally
// initialize a different schema via a surprising PostgreSQL search_path.
class PostgresBackend final : public StorageBackend
{
public:
    explicit PostgresBackend(const QString &schema_name = QStringLiteral("public"));

    QVariant bindUuid(const QUuid &uuid) const override;
    QUuid readUuid(const QVariant &value) const override;
    bool open(QSqlDatabase &database, const QString &connection_name,
              const QString &database_name, QString *error) const override;
    bool configure(const QSqlDatabase &database, QString *error) const override;
    bool verifySchema(const QSqlDatabase &database, QString *error) const override;
    bool createSchema(const QSqlDatabase &database, QString *error) const override;
    std::optional<bool> hasSchema(const QSqlDatabase &database,
                                  QString *error) const override;
    bool beginWrite(const QSqlDatabase &database, QString *error) const override;
    bool commit(const QSqlDatabase &database, QString *error) const override;
    void rollback(const QSqlDatabase &database) const override;
    std::optional<bool> hasColumn(const QSqlDatabase &database,
                                  const QString &table, const QString &column,
                                  QString *error) const override;

private:
    QString schema_name_;
};

} // namespace aowis::db

#endif // AOWIS_DB_POSTGRES_BACKEND_H
