#ifndef DATABASE_CONFIGURATION_H
#define DATABASE_CONFIGURATION_H

#include <QString>

enum class DatabaseBackend
{
    SQLite,
    PostgreSQL
};

struct DatabaseConfiguration
{
    DatabaseBackend backend = DatabaseBackend::SQLite;
    
    // SQLite. An empty path selects an application-specific default path.
    QString sqlite_database_path;
    QString sqlite_database_filename = QStringLiteral("aowis-server.sqlite3");
    int sqlite_busy_timeout_ms = 5000;
    
    // PostgreSQL.
    QString postgresql_host;
    int postgresql_port = 5432;
    QString postgresql_database;
    QString postgresql_user;
    QString postgresql_password;
};

#endif // DATABASE_CONFIGURATION_H
