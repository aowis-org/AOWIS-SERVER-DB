#include "database_shared.h"

#include "database_helpers.h"

DatabaseShared::DatabaseShared(const QSqlDatabase &database, QObject *parent)
    : QObject{parent},
    database(database)
{
    initTables();
}

bool DatabaseShared::initTables()
{
    return DatabaseHelpers::execute(
        this->database,
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS configs ("
            "config_key TEXT PRIMARY KEY,"
            "config_value TEXT NOT NULL"
            ")"
            )
        );
}

std::optional<QString> DatabaseShared::configValue(const QString &key) const
{
    return DatabaseHelpers::stringValue(
        this->database,
        QStringLiteral(
            "SELECT config_value "
            "FROM configs "
            "WHERE config_key = :config_key"
            ),
        {
            {QStringLiteral(":config_key"), key}
        }
        );
}

bool DatabaseShared::setConfigValue(const QString &key, const QString &value)
{
    return DatabaseHelpers::execute(
        this->database,
        QStringLiteral(
            "INSERT INTO configs (config_key, config_value) "
            "VALUES (:config_key, :config_value) "
            "ON CONFLICT (config_key) DO UPDATE SET "
            "config_value = excluded.config_value"
            ),
        {
            {QStringLiteral(":config_key"), key},
            {QStringLiteral(":config_value"), value}
        }
        );
}
