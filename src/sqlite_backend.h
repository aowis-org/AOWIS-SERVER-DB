#ifndef AOWIS_DB_SQLITE_BACKEND_H
#define AOWIS_DB_SQLITE_BACKEND_H

#include <optional>

#include <QSqlDatabase>
#include <QString>

namespace aowis::db::sqlite_backend
{
// SQLite-specific schema and connection contract. PostgreSQL must have its
// own adapter and migrations; it must never run these SQLite PRAGMAs/triggers.
inline constexpr int current_schema_version = 13;

bool open(QSqlDatabase &database, const QString &connection_name,
          const QString &file_path, QString *error);
bool configure(const QSqlDatabase &database, QString *error);
bool verifySchema(const QSqlDatabase &database, QString *error);
std::optional<int> schemaVersion(const QSqlDatabase &database, QString *error);
bool setSchemaVersion(const QSqlDatabase &database, int version, QString *error);
bool beginWrite(const QSqlDatabase &database, QString *error);
bool commit(const QSqlDatabase &database, QString *error);
void rollback(const QSqlDatabase &database);
}

#endif // AOWIS_DB_SQLITE_BACKEND_H
