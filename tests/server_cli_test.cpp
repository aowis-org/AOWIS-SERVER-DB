#include <aowis/db/database.h>

#include <QByteArray>
#include <QCoreApplication>

#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <optional>

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition; return 1; } \
} while (false)

struct HttpResponse
{
    int status = 0;
    QByteArray body;
    QString error;
};

static HttpResponse sendHttp(QNetworkAccessManager &manager, quint16 port,
                             const QString &path, const QByteArray &method,
                             const QByteArray &token = {}, const QByteArray &body = {})
{
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/v1/%2").arg(port).arg(path));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!token.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token);
    QNetworkReply *reply = method == QByteArrayLiteral("POST")
        ? manager.post(request, body) : manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(5000);
    if (!reply->isFinished())
        loop.exec();
    if (!reply->isFinished())
        reply->abort();
    const HttpResponse result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                              reply->readAll(), reply->errorString()};
    reply->deleteLater();
    return result;
}

struct ServerResult
{
    bool started = false;
    bool listening = false;
    int health_status = 0;
    int anonymous_status = 0;
    int authorized_status = 0;
    int create_status = 0;
    QByteArray projects_body;
    QByteArray diagnostics;
};

static ServerResult tryServer(const QString &executable, const QStringList &arguments,
                              quint16 port, const QByteArray &token = {})
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();

    ServerResult result;
    result.started = process.waitForStarted(5000);
    if (result.started)
    {
        QByteArray output;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 8000 && process.state() != QProcess::NotRunning)
        {
            process.waitForReadyRead(500);
            output += process.readAllStandardOutput();
            if (output.contains("listening on 127.0.0.1:"))
            {
                result.listening = true;
                break;
            }
        }
        if (result.listening)
        {
            QNetworkAccessManager manager;
            const HttpResponse health = sendHttp(manager, port, QStringLiteral("health"), "GET");
            const HttpResponse anonymous = sendHttp(manager, port, QStringLiteral("projects"), "GET");
            result.health_status = health.status;
            result.anonymous_status = anonymous.status;
            if (!token.isEmpty())
            {
                const HttpResponse created = sendHttp(manager, port, QStringLiteral("projects"), "POST",
                    token, QByteArrayLiteral("{\"name\":\"PostgreSQL HTTP project\"}"));
                const HttpResponse listed = sendHttp(manager, port, QStringLiteral("projects"), "GET", token);
                result.create_status = created.status;
                result.authorized_status = listed.status;
                result.projects_body = listed.body;
            }
        }
        output += process.readAllStandardOutput();
        result.diagnostics = output + process.readAllStandardError();
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(5000);
        }
        result.diagnostics += process.readAllStandardError();
    }
    else
        result.diagnostics = process.errorString().toUtf8();
    if (!result.listening || result.health_status != 200 || result.anonymous_status != 401 ||
        (!token.isEmpty() && (result.create_status != 201 || result.authorized_status != 200)))
        qCritical().noquote() << "Server integration failed:" << result.diagnostics
            << "health:" << result.health_status << "anonymous:" << result.anonymous_status
            << "created:" << result.create_status << "authorized:" << result.authorized_status
            << "projects:" << result.projects_body;
    return result;
}

// The live PostgreSQL test uses its own schema. Always remove it, including
// when a subprocess or an HTTP assertion fails; never touch existing schemas.
static bool postgresSchemaCommand(const QString &database_name, const QString &sql)
{
    const QString connection_name = QStringLiteral("aowis-server-db-cli-pg-schema");
    bool success = false;
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connection_name);
        connection.setDatabaseName(database_name);
        if (connection.open())
        {
            QSqlQuery query(connection);
            success = query.exec(sql);
            if (!success)
                qCritical().noquote() << "PostgreSQL schema operation failed:" << query.lastError().text();
            query.finish();
            connection.close();
        }
        else
            qCritical().noquote() << "PostgreSQL connection failed:" << connection.lastError().text();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return success;
}

static bool testPostgresServer(const QString &executable, const QTemporaryDir &temporary, quint16 port)
{
    const QString postgres_database = qEnvironmentVariable("AOWIS_DB_TEST_PG_DB");
    const QString schema = QStringLiteral("aowis_http_") +
        QUuid::createUuid().toString(QUuid::Id128).toLower();
    if (!postgresSchemaCommand(postgres_database,
            QStringLiteral("CREATE SCHEMA %1").arg(schema)))
        return false;

    bool success = false;
    QString error;
    do
    {
        QByteArray token;
        QUuid administrator_uuid;
        {
            aowis::db::Database db;
            if (!db.openPostgres(postgres_database, schema, &error))
                break;
            const std::optional<aowis::db::UserInfo> administrator =
                db.bootstrapAdministrator(QStringLiteral("postgres-http-admin"), &error);
            if (!administrator)
                break;
            administrator_uuid = administrator->uuid;
            const std::optional<aowis::db::IssuedSession> session =
                db.issueSession({administrator_uuid}, administrator_uuid, 3600, &error);
            if (!session)
                break;
            token = session->bearer_token;
        }
        const QString ini_file = temporary.filePath(QStringLiteral("server-postgres.ini"));
        {
            QSettings settings(ini_file, QSettings::IniFormat);
            settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgres"));
            settings.setValue(QStringLiteral("Database/Name"), postgres_database);
            settings.setValue(QStringLiteral("Database/Schema"), schema);
            settings.setValue(QStringLiteral("Logging/File"),
                              temporary.filePath(QStringLiteral("server-postgres.log")));
            settings.setValue(QStringLiteral("HTTP/Port"), port);
            settings.sync();
            if (settings.status() != QSettings::NoError)
            {
                error = QStringLiteral("Could not write PostgreSQL test INI");
                break;
            }
        }
        const ServerResult server = tryServer(executable,
            QStringList{QStringLiteral("--config"), ini_file}, port, token);
        if (!server.started || !server.listening || server.health_status != 200 ||
            server.anonymous_status != 401 || server.authorized_status != 200 ||
            server.create_status != 201)
        {
            error = QStringLiteral("PostgreSQL HTTP startup or request failed");
            break;
        }
        const QJsonArray projects = QJsonDocument::fromJson(server.projects_body)
                                        .object().value(QStringLiteral("projects")).toArray();
        if (projects.size() != 1 ||
            projects.first().toObject().value(QStringLiteral("name")).toString() !=
                QStringLiteral("PostgreSQL HTTP project"))
        {
            error = QStringLiteral("PostgreSQL HTTP project listing differs from persisted model");
            break;
        }
        {
            aowis::db::Database reopened;
            if (!reopened.openPostgres(postgres_database, schema, &error))
                break;
            const QVector<aowis::db::ProjectInfo> items =
                reopened.projects({administrator_uuid}, &error);
            if (!error.isEmpty() || items.size() != 1 ||
                items.first().name != QStringLiteral("PostgreSQL HTTP project"))
            {
                error = QStringLiteral("PostgreSQL HTTP project did not persist");
                break;
            }
        }
        success = true;
    } while (false);
    if (!success)
        qCritical().noquote() << "PostgreSQL server integration failed:" << error;
    const bool cleaned = postgresSchemaCommand(postgres_database,
        QStringLiteral("DROP SCHEMA %1 CASCADE").arg(schema));
    return success && cleaned;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    REQUIRE(app.arguments().size() == 2);
    const QString executable = app.arguments().at(1);
    REQUIRE(QFileInfo::exists(executable));

    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QString ini_file = temporary.filePath(QStringLiteral("server.ini"));
    const QString configured_db = temporary.filePath(QStringLiteral("configured.sqlite"));
    const QString override_db = temporary.filePath(QStringLiteral("overridden.sqlite"));
    const QString log_file = temporary.filePath(QStringLiteral("log.txt"));

    QTcpServer port_probe;
    REQUIRE(port_probe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = port_probe.serverPort();
    port_probe.close();

    {
        QSettings settings(ini_file, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("sqlite"));
        settings.setValue(QStringLiteral("Database/File"), configured_db);
        settings.setValue(QStringLiteral("Logging/File"), log_file);
        settings.setValue(QStringLiteral("HTTP/Port"), port);
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }

    const QStringList configuration{QStringLiteral("--config"), ini_file};
    const ServerResult default_start = tryServer(executable, configuration, port);
    REQUIRE(default_start.started && default_start.listening);
    REQUIRE(default_start.health_status == 200 && default_start.anonymous_status == 401);
    REQUIRE(QFileInfo::exists(configured_db));
    REQUIRE(!QFileInfo::exists(override_db));

    const ServerResult override_start = tryServer(executable,
        configuration + QStringList{override_db}, port);
    REQUIRE(override_start.started && override_start.listening);
    REQUIRE(override_start.health_status == 200 && override_start.anonymous_status == 401);
    REQUIRE(QFileInfo::exists(override_db));

    // A CLI override must not modify the INI file itself.
    QSettings loaded(ini_file, QSettings::IniFormat);
    REQUIRE(loaded.value(QStringLiteral("Database/File")).toString() == configured_db);

    const QString pg_ini = temporary.filePath(QStringLiteral("pg.ini"));
    {
        QSettings settings(pg_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgres"));
        settings.setValue(QStringLiteral("Database/Name"), QStringLiteral("unused"));
        settings.setValue(QStringLiteral("Database/Schema"), QStringLiteral("unused"));
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    QProcess invalid;
    invalid.start(executable, QStringList{QStringLiteral("--config"), pg_ini, override_db});
    REQUIRE(invalid.waitForStarted(5000));
    REQUIRE(invalid.waitForFinished(5000));
    REQUIRE(invalid.exitCode() == 2);
    REQUIRE(invalid.readAllStandardError().contains("Backend=postgres"));

    if (!qEnvironmentVariable("AOWIS_DB_TEST_PG_DB").isEmpty())
        REQUIRE(testPostgresServer(executable, temporary, port));
    else if (qEnvironmentVariable("AOWIS_DB_TEST_PG_REQUIRED") == QStringLiteral("1"))
    {
        qCritical() << "Live PostgreSQL was required but no test database is configured";
        return 1;
    }
    return 0;
}
