#include <aowis/db/configuration.h>

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

QString appDataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString localDataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

bool readFilePath(QSettings &settings, const QString &key, QString &path, QString *error)
{
    if (!settings.contains(key))
        return true;
    const QString value = QDir::fromNativeSeparators(settings.value(key).toString().trimmed());
    if (value.isEmpty() || !QFileInfo(value).isAbsolute())
    {
        setError(error, QStringLiteral("%1 must be an absolute, nonempty file path").arg(key));
        return false;
    }
    path = QDir::cleanPath(value);
    return true;
}
}

bool Configuration::serverMode()
{
#ifdef AOWIS_DB_SERVER_MODE
    return true;
#else
    return false;
#endif
}

QString Configuration::defaultIniFile()
{
#if defined(AOWIS_DB_SERVER_MODE) && defined(Q_OS_LINUX)
    return QStringLiteral("/etc/aowis-server-db/config.ini");
#else
    return QDir(appDataDirectory()).filePath(QStringLiteral("config.ini"));
#endif
}

QString Configuration::defaultSqliteFile()
{
#if defined(AOWIS_DB_SERVER_MODE) && defined(Q_OS_LINUX)
    return QStringLiteral("/var/lib/aowis-server-db/aowis.sqlite");
#else
    return QDir(appDataDirectory()).filePath(QStringLiteral("aowis.sqlite"));
#endif
}

QString Configuration::defaultLogFile()
{
#if defined(AOWIS_DB_SERVER_MODE) && defined(Q_OS_LINUX)
    return QStringLiteral("/var/log/aowis-server-db/server.log");
#else
    return QDir(localDataDirectory()).filePath(QStringLiteral("logs/server.log"));
#endif
}

std::optional<Configuration> Configuration::load(const QString &ini_file, QString *error)
{
    Configuration result;
    result.ini_file = ini_file.isEmpty() ? defaultIniFile() : ini_file;
    result.sqlite_file = defaultSqliteFile();
    result.log_file = defaultLogFile();

    if (!QFileInfo(result.ini_file).isAbsolute())
    {
        setError(error, QStringLiteral("INI file path must be absolute"));
        return std::nullopt;
    }
    result.ini_file = QDir::cleanPath(result.ini_file);
    const QFileInfo source(result.ini_file);
    if (!source.exists())
        return result;
    if (!source.isFile() || !source.isReadable())
    {
        setError(error, QStringLiteral("INI file is not a readable file: %1").arg(result.ini_file));
        return std::nullopt;
    }

    QSettings settings(result.ini_file, QSettings::IniFormat);
    const QString backend = settings.value(QStringLiteral("Database/Backend"),
                                            QStringLiteral("sqlite")).toString().trimmed();
    if (backend == QStringLiteral("sqlite"))
    {
        result.database_backend = Configuration::Backend::SQLite;
        if (settings.contains(QStringLiteral("Database/Name")) ||
            settings.contains(QStringLiteral("Database/Schema")))
        {
            setError(error, QStringLiteral("Database/Name and Database/Schema require Database/Backend=postgres"));
            return std::nullopt;
        }
        if (!readFilePath(settings, QStringLiteral("Database/File"), result.sqlite_file, error))
            return std::nullopt;
    }
    else if (backend == QStringLiteral("postgres"))
    {
        result.database_backend = Configuration::Backend::PostgreSQL;
        if (settings.contains(QStringLiteral("Database/File")))
        {
            setError(error, QStringLiteral("Database/File is only valid with Database/Backend=sqlite"));
            return std::nullopt;
        }
        result.postgres_database = settings.value(QStringLiteral("Database/Name")).toString().trimmed();
        result.postgres_schema = settings.value(QStringLiteral("Database/Schema")).toString().trimmed();
        if (result.postgres_database.isEmpty() || result.postgres_schema.isEmpty())
        {
            setError(error, QStringLiteral("PostgreSQL requires Database/Name and Database/Schema"));
            return std::nullopt;
        }
        // The backend inserts the schema identifier into a SET search_path
        // statement. Reject invalid identifiers before starting the server.
        if (result.postgres_schema.size() > 63 ||
            result.postgres_schema.front().isDigit())
        {
            setError(error, QStringLiteral("Invalid PostgreSQL Database/Schema identifier"));
            return std::nullopt;
        }
        for (const QChar ch : result.postgres_schema)
        {
            const ushort c = ch.unicode();
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            {
                setError(error, QStringLiteral("Invalid PostgreSQL Database/Schema identifier"));
                return std::nullopt;
            }
        }
    }
    else
    {
        setError(error, QStringLiteral("Database/Backend must be sqlite or postgres"));
        return std::nullopt;
    }

    if (!readFilePath(settings, QStringLiteral("Logging/File"), result.log_file, error))
        return std::nullopt;

    if (settings.contains(QStringLiteral("HTTP/Port")))
    {
        bool ok = false;
        const uint port = settings.value(QStringLiteral("HTTP/Port")).toString().toUInt(&ok);
        if (!ok || port == 0 || port > 65535)
        {
            setError(error, QStringLiteral("HTTP/Port must be between 1 and 65535"));
            return std::nullopt;
        }
        result.http_port = static_cast<quint16>(port);
    }
    if (settings.status() != QSettings::NoError)
    {
        setError(error, QStringLiteral("Could not parse INI file: %1").arg(result.ini_file));
        return std::nullopt;
    }
    return result;
}

} // namespace aowis::db
