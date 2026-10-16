#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int request(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &token,
            const QByteArray &method = "GET", const QByteArray &body = {})
{
    QNetworkRequest message(url);
    if (!token.isEmpty())
        message.setRawHeader("Authorization", QByteArray("Bearer ") + token);
    message.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = method == "POST" ? manager.post(message, body) : manager.get(message);
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
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0)
        qWarning().noquote() << reply->errorString();
    reply->deleteLater();
    return status;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database db;
    REQUIRE(db.openSqlite(directory.filePath(QStringLiteral("http_sessions.sqlite")), &error));
    const auto admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const auto guest = db.createUser(actor, QStringLiteral("guest"), &error);
    REQUIRE(guest.has_value());
    const auto admin_token = db.issueSession(actor, admin->uuid, 3600, &error);
    const auto guest_token = db.issueSession(actor, guest->uuid, 3600, &error);
    REQUIRE(admin_token.has_value() && guest_token.has_value());

    QHttpServer server;
    aowis::db::HttpRoutes routes(db, [&db](const QHttpServerRequest &input)
        -> std::optional<OperationContext> {
        const QByteArray auth = input.value(QByteArrayLiteral("authorization"));
        if (!auth.startsWith("Bearer "))
            return std::nullopt;
        return db.authenticateSession(auth.mid(7));
    });
    REQUIRE(routes.registerOn(server));
    QTcpServer *listener = new QTcpServer(&server);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(server.bind(listener));
    QNetworkAccessManager manager;
    const QUrl projects(QStringLiteral("http://127.0.0.1:%1/api/v1/projects").arg(listener->serverPort()));
    REQUIRE(request(manager, projects, {}) == 401);
    REQUIRE(request(manager, projects, admin_token->bearer_token) == 200);
    REQUIRE(request(manager, projects, guest_token->bearer_token) == 200);
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("name"), QStringLiteral("Network")}})
        .toJson(QJsonDocument::Compact);
    REQUIRE(request(manager, projects, guest_token->bearer_token, "POST", payload) == 403);
    REQUIRE(request(manager, projects, admin_token->bearer_token, "POST", payload) == 201);
    REQUIRE(db.revokeSession(actor, admin_token->uuid, &error));
    REQUIRE(request(manager, projects, admin_token->bearer_token) == 401);
    REQUIRE(request(manager, projects, guest_token->bearer_token) == 200);
    REQUIRE(db.disableUser(actor, guest->uuid, &error));
    REQUIRE(request(manager, projects, guest_token->bearer_token) == 401);
    return 0;
}
