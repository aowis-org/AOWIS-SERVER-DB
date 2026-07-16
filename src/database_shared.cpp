#include "database_shared.h"

#include "database_helpers.h"

DatabaseShared::DatabaseShared(const QSqlDatabase &database, QObject *parent)
    : QObject{parent},
    database(database)
{
    
}

bool DatabaseShared::initialize()
{
    if (!DatabaseHelpers::execute(
            this->database,
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS configs ("
                "config_key TEXT PRIMARY KEY,"
                "config_value TEXT NOT NULL"
                ")"
                ))) {
        return false;
    }
    
    if (!DatabaseHelpers::execute(
            this->database,
            QStringLiteral(
                "CREATE TABLE IF NOT EXISTS projects ("
                "project_id TEXT NOT NULL PRIMARY KEY,"
                "name TEXT NOT NULL,"
                "description TEXT,"
                "created_at BIGINT NOT NULL,"
                "modified_at BIGINT NOT NULL,"
                "archived_at BIGINT"
                ")"
                ))) {
        return false;
    }
    
    return true;
}

QString DatabaseShared::createProject(const QString &name, const QString &description)
{
    const QString projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    
    const bool success = DatabaseHelpers::execute(
        this->database,
        QStringLiteral(
            "INSERT INTO projects ("
            "project_id,"
            "name,"
            "description,"
            "created_at,"
            "modified_at,"
            "archived_at"
            ") VALUES ("
            ":project_id,"
            ":name,"
            ":description,"
            ":created_at,"
            ":modified_at,"
            "NULL"
            ")"
            ),
        {
            {QStringLiteral(":project_id"), projectId},
            {QStringLiteral(":name"), name},
            {QStringLiteral(":description"), description},
            {QStringLiteral(":created_at"), timestamp},
            {QStringLiteral(":modified_at"), timestamp}
        }
        );
    
    if (!success)
        return {};
    
    return projectId;
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
