#ifndef AOWIS_DB_CONFIGURATION_H
#define AOWIS_DB_CONFIGURATION_H

#include <optional>

#include <QString>
#include <QtGlobal>

namespace aowis::db
{

// Paths are resolved with Qt, not from a process working directory. Loading
// configuration has no filesystem side effects: it does not create the INI.
struct Configuration
{
    QString ini_file;
    // SQLite is the default. PostgreSQL needs a database name and an existing
    // schema; libpq handles connection credentials and host settings.
    enum class Backend { SQLite, PostgreSQL };
    Backend database_backend = Backend::SQLite;
    QString sqlite_file;
    QString postgres_database;
    QString postgres_schema;
    QString log_file;
    quint16 http_port = 8080;

    static bool serverMode();
    static QString defaultIniFile();
    static QString defaultSqliteFile();
    static QString defaultLogFile();

    // If ini_file is empty, choose the normal Qt AppDataLocation or the
    // compile-time system configuration location. Missing INIs use defaults.
    // Path overrides must be absolute, eliminating current-directory ambiguity.
    static std::optional<Configuration> load(const QString &ini_file = {},
                                              QString *error = nullptr);
};

} // namespace aowis::db

#endif // AOWIS_DB_CONFIGURATION_H
