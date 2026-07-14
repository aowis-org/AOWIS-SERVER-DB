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

    // SQLite
    QString sqlite_database_path;

    // PostgreSQL
    QString postgresql_host;
    int postgresql_port = 5432;
    QString postgresql_database;
    QString postgresql_user;
    QString postgresql_password;
};

#endif // DATABASE_CONFIGURATION_H
