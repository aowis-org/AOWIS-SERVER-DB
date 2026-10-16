#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>
#include <aowis/db/http_hardening.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QTcpServer>
#include <QTextStream>
#include <QStringList>
#include <optional>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTextStream errors(stderr);
    QTextStream output(stdout);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 2 || arguments.size() > 3)
    {
        errors << "Usage: aowis-db-server SQLITE_FILE [PORT]\n"
               << "Requires locally provisioned, persistent bearer sessions. Loopback only.\n";
        return 2;
    }
    quint16 port = 8080;
    if (arguments.size() == 3)
    {
        bool ok = false;
        const uint requested = arguments.at(2).toUInt(&ok);
        if (!ok || requested == 0 || requested > 65535)
        {
            errors << "PORT must be between 1 and 65535.\n";
            return 2;
        }
        port = static_cast<quint16>(requested);
    }

    aowis::db::Database database;
    QString error;
    if (!database.openSqlite(arguments.at(1), &error))
    {
        errors << "Could not open database: " << error << "\n";
        return 1;
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
        errors << "Could not register HTTP routes.\n";
        return 1;
    }
    QTcpServer *listener = new QTcpServer(&server);
    if (!listener->listen(QHostAddress::LocalHost, port) || !server.bind(listener))
    {
        errors << "Could not listen on localhost:" << port << ": " << listener->errorString() << "\n";
        return 1;
    }
    output << "AOWIS session-authenticated REST API listening on 127.0.0.1:" << listener->serverPort() << "\n";
    output.flush();
    return application.exec();
}
