#include <aowis/db/configuration.h>
#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("AOWIS-SERVER-DB"));
    QStandardPaths::setTestModeEnabled(true);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());

    const QString absent_ini = temp.filePath(QStringLiteral("does-not-exist.ini"));
    REQUIRE(!QFileInfo::exists(absent_ini));
    const std::optional<aowis::db::Configuration> defaults =
        aowis::db::Configuration::load(absent_ini, &error);
    REQUIRE(defaults.has_value());
    REQUIRE(defaults->ini_file == absent_ini);
    REQUIRE(defaults->database_backend == aowis::db::Configuration::Backend::SQLite);
    REQUIRE(defaults->postgres_database.isEmpty());
    REQUIRE(defaults->postgres_schema.isEmpty());
    REQUIRE(defaults->sqlite_file == aowis::db::Configuration::defaultSqliteFile());
    REQUIRE(defaults->log_file == aowis::db::Configuration::defaultLogFile());
    REQUIRE(defaults->http_port == 8080);
    REQUIRE(!QFileInfo::exists(absent_ini)); // Reading never creates an INI.

#if defined(AOWIS_DB_SERVER_MODE) && defined(Q_OS_LINUX)
    REQUIRE(aowis::db::Configuration::serverMode());
    REQUIRE(aowis::db::Configuration::defaultIniFile() == QStringLiteral("/etc/aowis-server-db/config.ini"));
    REQUIRE(aowis::db::Configuration::defaultSqliteFile() == QStringLiteral("/var/lib/aowis-server-db/aowis.sqlite"));
    REQUIRE(aowis::db::Configuration::defaultLogFile() == QStringLiteral("/var/log/aowis-server-db/server.log"));
#else
    REQUIRE(aowis::db::Configuration::defaultIniFile() ==
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("config.ini")));
    REQUIRE(aowis::db::Configuration::defaultSqliteFile() ==
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("aowis.sqlite")));
#endif

    const QString ini = temp.filePath(QStringLiteral("user config.ini"));
    const QString database_file = temp.filePath(QStringLiteral("data/network database.sqlite"));
    const QString log_file = temp.filePath(QStringLiteral("logs/aowis server.log"));
    {
        QSettings settings(ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/File"), database_file);
        settings.setValue(QStringLiteral("Logging/File"), log_file);
        settings.setValue(QStringLiteral("HTTP/Port"), 9234);
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    const std::optional<aowis::db::Configuration> custom =
        aowis::db::Configuration::load(ini, &error);
    REQUIRE(custom.has_value());
    REQUIRE(custom->database_backend == aowis::db::Configuration::Backend::SQLite);
    REQUIRE(custom->sqlite_file == database_file);
    REQUIRE(custom->log_file == log_file);
    REQUIRE(custom->http_port == 9234);

    // The selected database filename is directly usable by the SQLite core.
    REQUIRE(QDir().mkpath(QFileInfo(custom->sqlite_file).absolutePath()));
    aowis::db::Database database;
    REQUIRE(database.openSqlite(custom->sqlite_file, &error));
    database.close();
    REQUIRE(QFileInfo::exists(custom->sqlite_file));

    {
        QSettings settings(ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/File"), QStringLiteral("relative.sqlite"));
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(ini, &error));
    REQUIRE(error.contains(QStringLiteral("Database/File")));

    {
        QSettings settings(ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/File"), database_file);
        settings.setValue(QStringLiteral("Logging/File"), QStringLiteral(" "));
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(ini, &error));
    REQUIRE(error.contains(QStringLiteral("Logging/File")));

    {
        QSettings settings(ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Logging/File"), log_file);
        settings.setValue(QStringLiteral("HTTP/Port"), 65536);
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(ini, &error));
    REQUIRE(error.contains(QStringLiteral("HTTP/Port")));

    {
        QSettings settings(ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("HTTP/Port"), 0);
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(ini, &error));
    REQUIRE(error.contains(QStringLiteral("HTTP/Port")));

    // PostgreSQL selection requires an explicit existing schema, but must
    // not require or inspect a SQLite file. Connection secrets belong to libpq.
    const QString postgres_ini = temp.filePath(QStringLiteral("postgres.ini"));
    {
        QSettings settings(postgres_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgres"));
        settings.setValue(QStringLiteral("Database/Name"), QStringLiteral("aowis_test"));
        settings.setValue(QStringLiteral("Database/Schema"), QStringLiteral("aowis_projects"));
        settings.setValue(QStringLiteral("Logging/File"), log_file);
        settings.setValue(QStringLiteral("HTTP/Port"), 9321);
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    error.clear();
    const std::optional<aowis::db::Configuration> pg_config =
        aowis::db::Configuration::load(postgres_ini, &error);
    REQUIRE(pg_config.has_value());
    REQUIRE(pg_config->database_backend == aowis::db::Configuration::Backend::PostgreSQL);
    REQUIRE(pg_config->postgres_database == QStringLiteral("aowis_test"));
    REQUIRE(pg_config->postgres_schema == QStringLiteral("aowis_projects"));
    REQUIRE(pg_config->log_file == log_file);
    REQUIRE(pg_config->http_port == 9321);

    // Reject misspelled backends, ambiguous mixed configurations, and unsafe
    // schema identifiers instead of silently falling back to SQLite.
    {
        QSettings settings(postgres_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgre"));
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(postgres_ini, &error));
    REQUIRE(error.contains(QStringLiteral("Database/Backend")));
    {
        QSettings settings(postgres_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgres"));
        settings.setValue(QStringLiteral("Database/File"), database_file);
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(postgres_ini, &error));
    REQUIRE(error.contains(QStringLiteral("Database/File")));
    {
        QSettings settings(postgres_ini, QSettings::IniFormat);
        settings.remove(QStringLiteral("Database/File"));
        settings.setValue(QStringLiteral("Database/Schema"), QStringLiteral("aowis;DROP TABLE users"));
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(postgres_ini, &error));
    REQUIRE(error.contains(QStringLiteral("Database/Schema")));
    {
        QSettings settings(postgres_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Schema"), QStringLiteral("aowis_projects"));
        settings.remove(QStringLiteral("Database/Name"));
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(postgres_ini, &error));
    REQUIRE(error.contains(QStringLiteral("Database/Name")));
    {
        QSettings settings(postgres_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("sqlite"));
        settings.setValue(QStringLiteral("Database/Name"), QStringLiteral("aowis_test"));
        settings.sync();
    }
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(postgres_ini, &error));
    REQUIRE(error.contains(QStringLiteral("Database/Name")));

    error.clear();
    REQUIRE(!aowis::db::Configuration::load(QStringLiteral("relative.ini"), &error));
    REQUIRE(error.contains(QStringLiteral("absolute")));
    error.clear();
    REQUIRE(!aowis::db::Configuration::load(temp.path(), &error));
    REQUIRE(error.contains(QStringLiteral("readable file")));
    return 0;
}
