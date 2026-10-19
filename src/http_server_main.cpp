#include <aowis/db/configuration.h>
#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>
#include <aowis/db/http_hardening.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTextStream>
#include <QStringList>

#include <cstdio>
#include <optional>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace
{
QMutex log_mutex;
QFile *active_log_file = nullptr;

void serverMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *severity = "INFO";
    if (type == QtDebugMsg)
        severity = "DEBUG";
    else if (type == QtWarningMsg)
        severity = "WARN";
    else if (type == QtCriticalMsg || type == QtFatalMsg)
        severity = "ERROR";

    const QByteArray line = QStringLiteral("%1 [%2] %3\n")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
        .arg(QString::fromLatin1(severity))
        .arg(message).toUtf8();
    QMutexLocker locker(&log_mutex);
    if (active_log_file != nullptr)
    {
        active_log_file->write(line);
        active_log_file->flush();
    }
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
    std::fflush(stderr);
}

class ServerLog final
{
public:
    ~ServerLog()
    {
        if (this->installed_)
        {
            qInstallMessageHandler(this->previous_handler_);
            QMutexLocker locker(&log_mutex);
            active_log_file = nullptr;
        }
    }

    bool open(const QString &file_path, QString *error)
    {
        if (!QDir().mkpath(QFileInfo(file_path).absolutePath()))
        {
            if (error != nullptr)
                *error = QStringLiteral("Cannot create log directory for %1").arg(file_path);
            return false;
        }
        const bool newly_created = !QFileInfo::exists(file_path);
        this->file_.setFileName(file_path);
        if (!this->file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            if (error != nullptr)
                *error = this->file_.errorString();
            return false;
        }
#ifdef Q_OS_UNIX
        // Logs may contain operational diagnostics. Never create a readable-by-
        // everyone log file just because the service has a permissive umask.
        if (newly_created && !this->file_.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        {
            if (error != nullptr)
                *error = QStringLiteral("Cannot restrict permissions on log file: %1").arg(file_path);
            this->file_.close();
            return false;
        }
#endif
        {
            QMutexLocker locker(&log_mutex);
            active_log_file = &this->file_;
            this->previous_handler_ = qInstallMessageHandler(serverMessageHandler);
            this->installed_ = true;
        }
        return true;
    }

private:
    QFile file_;
    QtMessageHandler previous_handler_ = nullptr;
    bool installed_ = false;
};
}

int main(int argc, char **argv)
{
#ifdef Q_OS_UNIX
    // Apply only to this server process, before Qt or SQLite create files.
    // Existing files keep their permissions; embedded library users are unaffected.
    ::umask(0077);
#endif
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("AOWIS"));
    application.setApplicationName(QStringLiteral("AOWIS-SERVER-DB"));
    QTextStream errors(stderr);
    QTextStream output(stdout);
    const QStringList arguments = application.arguments();
    QString ini_file;
    QString sqlite_override;
    const QString usage = QStringLiteral(
        "Usage: aowis-server-db [--config ABSOLUTE_INI_FILE] [SQLITE_FILE]\n"
        "SQLite file argument overrides Database/File for this run only.\n"
        "In PostgreSQL mode, database selection comes exclusively from the INI.\n");
    for (int index = 1; index < arguments.size(); ++index)
    {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--config"))
        {
            if (!ini_file.isEmpty() || index + 1 == arguments.size() ||
                arguments.at(index + 1).isEmpty() ||
                arguments.at(index + 1).startsWith(QStringLiteral("--")))
            {
                errors << usage;
                return 2;
            }
            ini_file = arguments.at(++index);
        }
        else if (argument.startsWith(QStringLiteral("--")) || !sqlite_override.isEmpty())
        {
            errors << usage;
            return 2;
        }
        else
            sqlite_override = argument;
    }

    QString error;
    const std::optional<aowis::db::Configuration> config =
        aowis::db::Configuration::load(ini_file, &error);
    if (!config)
    {
        errors << "Invalid configuration: " << error << "\n";
        return 2;
    }
    const bool use_postgres =
        config->database_backend == aowis::db::Configuration::Backend::PostgreSQL;
    if (use_postgres && !sqlite_override.isEmpty())
    {
        errors << "A SQLite file override cannot be used with Database/Backend=postgres.\n";
        return 2;
    }
    // The INI selects the default database. A positional override is an
    // explicit per-invocation convenience for SQLite, never persisted to INI.
    const QString sqlite_file = sqlite_override.isEmpty() ? config->sqlite_file :
        QDir::cleanPath(QFileInfo(sqlite_override).absoluteFilePath());
    const quint16 port = config->http_port;

    ServerLog log;
    if (!log.open(config->log_file, &error))
    {
        errors << "Could not open log file '" << config->log_file << "': " << error << "\n";
        return 1;
    }
    aowis::db::Database database;
    if (use_postgres)
    {
        // PostgreSQL's host, user and credentials come from libpq's PG*
        // environment, service file or .pgpass, never from this INI.
        if (!database.openPostgres(config->postgres_database, config->postgres_schema, &error))
        {
            qCritical().noquote() << "Could not open PostgreSQL database:" << error;
            return 1;
        }
    }
    else
    {
        if (!QDir().mkpath(QFileInfo(sqlite_file).absolutePath()))
        {
            qCritical().noquote() << "Could not create database directory for" << sqlite_file;
            return 1;
        }
        if (!database.openSqlite(sqlite_file, &error))
        {
            qCritical().noquote() << "Could not open SQLite database:" << error;
            return 1;
        }
    }

    QHttpServer server;
    aowis::db::configureHttpHardening(server);
    aowis::db::HttpRoutes routes(database, [&database](const QHttpServerRequest &request)
        -> std::optional<aowis::db::OperationContext> {
        const QByteArray authorization = request.value(QByteArrayLiteral("authorization"));
        const std::optional<QByteArray> token = aowis::db::parseBearerAuthorization(authorization);
        if (!token)
            return std::nullopt;
        return database.authenticateSession(*token);
    });
    if (!routes.registerOn(server))
    {
        qCritical() << "Could not register HTTP routes.";
        return 1;
    }
    QTcpServer *listener = new QTcpServer(&server);
    if (!listener->listen(QHostAddress::LocalHost, port) || !server.bind(listener))
    {
        qCritical().noquote() << "Could not listen on localhost:" << port << listener->errorString();
        return 1;
    }
    const QString startup = QStringLiteral("AOWIS session-authenticated REST API listening on 127.0.0.1:%1")
                                .arg(listener->serverPort());
    qInfo().noquote() << startup;
    output << startup << "\n";
    output.flush();
    return application.exec();
}
