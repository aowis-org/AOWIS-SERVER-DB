#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>
#include <aowis/db/http_hardening.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct Reply
{
    int status = 0;
    QJsonObject body;
};

Reply send(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &method,
           const QJsonObject &body = {}, const QByteArray &token = {})
{
    QNetworkRequest request(url);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token);
    QNetworkReply *network = nullptr;
    if (method == "POST")
    {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        network = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    else
        network = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(network, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(15000);
    if (!network->isFinished())
        loop.exec();
    if (!network->isFinished())
        network->abort();
    const QByteArray bytes = network->readAll();
    const Reply result{network->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                       QJsonDocument::fromJson(bytes).object()};
    if (!result.status)
        qWarning().noquote() << "Network error" << network->errorString();
    network->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    aowis::db::Database db;
    REQUIRE(db.openSqlite(temp.filePath(QStringLiteral("login.sqlite")), &error));
    const auto admin = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const auto user = db.createUser({admin->uuid}, QStringLiteral("engineer"), &error);
    REQUIRE(user.has_value());
    const QByteArray pass("long-password-with-a-secret");
    REQUIRE(db.setUserPassword({admin->uuid}, user->uuid, pass, &error));
    QHttpServer http;
    aowis::db::configureHttpHardening(http);
    aowis::db::HttpRoutes routes(db, [&db](const QHttpServerRequest &request)
        -> std::optional<aowis::db::OperationContext> {
        const auto token = aowis::db::parseBearerAuthorization(request.value(QByteArrayLiteral("authorization")));
        return token ? db.authenticateSession(*token) : std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer *listener = new QTcpServer(&http);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(listener));
    QNetworkAccessManager manager;
    const QUrl login(QStringLiteral("http://127.0.0.1:%1/api/v1/session/login").arg(listener->serverPort()));
    const QUrl current(QStringLiteral("http://127.0.0.1:%1/api/v1/session/current").arg(listener->serverPort()));
    REQUIRE(send(manager, login, "GET").status != 200);
    REQUIRE(send(manager, login, "POST", {{QStringLiteral("username"), QStringLiteral("unknown")},
                                         {QStringLiteral("password"), QString::fromLatin1(pass)}}).status == 401);
    REQUIRE(send(manager, login, "POST", {{QStringLiteral("username"), QStringLiteral("engineer")},
                                         {QStringLiteral("password"), QStringLiteral("incorrect")}}).status == 401);
    REQUIRE(send(manager, login, "POST", {{QStringLiteral("username"), QStringLiteral("engineer")},
                                         {QStringLiteral("password"), QString::fromLatin1(pass)},
                                         {QStringLiteral("actor_uuid"), user->uuid.toString()}}).status == 400);
    const Reply logged = send(manager, login, "POST", {{QStringLiteral("username"), QStringLiteral("engineer")},
                                                      {QStringLiteral("password"), QString::fromLatin1(pass)}});
    REQUIRE(logged.status == 200);
    const QByteArray token = logged.body.value(QStringLiteral("bearer_token")).toString().toLatin1();
    REQUIRE(token.size() == 64);
    REQUIRE(logged.body.value(QStringLiteral("user_uuid")).toString() == user->uuid.toString(QUuid::WithoutBraces));
    REQUIRE(send(manager, current, "GET", {}, token).status == 200);
    REQUIRE(db.setUserPassword({admin->uuid}, user->uuid, QByteArrayLiteral("new long credential secret"), &error));
    REQUIRE(send(manager, current, "GET", {}, token).status == 401);
    REQUIRE(send(manager, login, "POST", {{QStringLiteral("username"), QStringLiteral("engineer")},
                                         {QStringLiteral("password"), QString::fromLatin1(pass)}}).status == 401);
    REQUIRE(send(manager, login, "POST", {{QStringLiteral("username"), QStringLiteral("engineer")},
                                         {QStringLiteral("password"), QStringLiteral("new long credential secret")}}).status == 200);
    REQUIRE(db.verifySecurityAudit({admin->uuid}).valid);
    return 0;
}
