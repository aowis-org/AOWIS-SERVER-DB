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

Reply request(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &method,
              const QJsonObject &body = {}, const QByteArray &token = {})
{
    QNetworkRequest message(url);
    if (!token.isEmpty())
        message.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token);
    QNetworkReply *network = nullptr;
    if (method == "POST")
    {
        message.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        network = manager.post(message, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    else
        network = manager.get(message);
    QEventLoop event_loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(network, &QNetworkReply::finished, &event_loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &event_loop, &QEventLoop::quit);
    timeout.start(15000);
    if (!network->isFinished())
        event_loop.exec();
    if (!network->isFinished())
        network->abort();
    const QByteArray response = network->readAll();
    Reply result{network->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                 QJsonDocument::fromJson(response).object()};
    if (!result.status)
        qWarning().noquote() << "HTTP network error:" << network->errorString();
    network->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QString error;
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    aowis::db::Database database;
    REQUIRE(database.openSqlite(temporary.filePath(QStringLiteral("password-self-service.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> administrator =
        database.bootstrapAdministrator(QStringLiteral("administrator"), &error);
    REQUIRE(administrator.has_value());
    const aowis::db::OperationContext admin{administrator->uuid};
    const std::optional<aowis::db::UserInfo> user = database.createUser(admin, QStringLiteral("engineer"), &error);
    REQUIRE(user.has_value());
    const QByteArray initial = QByteArrayLiteral("this-is-my-initial-password");
    const QByteArray replacement = QByteArrayLiteral("this-is-my-new-password-2026");
    REQUIRE(database.setUserPassword(admin, user->uuid, initial, &error));
    QHttpServer http;
    aowis::db::configureHttpHardening(http);
    aowis::db::HttpRoutes routes(database, [&database](const QHttpServerRequest &message)
        -> std::optional<aowis::db::OperationContext> {
        const std::optional<QByteArray> token = aowis::db::parseBearerAuthorization(
            message.value(QByteArrayLiteral("authorization")));
        return token ? database.authenticateSession(*token) : std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer listener(&http);
    REQUIRE(listener.listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(&listener));
    const QString address = QStringLiteral("http://127.0.0.1:%1").arg(listener.serverPort());
    const QUrl login(address + QStringLiteral("/api/v1/session/login"));
    const QUrl change(address + QStringLiteral("/api/v1/session/password"));
    const QUrl current(address + QStringLiteral("/api/v1/session/current"));
    QNetworkAccessManager manager;
    const QJsonObject change_body{{QStringLiteral("current_password"), QString::fromUtf8(initial)},
                                  {QStringLiteral("new_password"), QString::fromUtf8(replacement)}};
    REQUIRE(request(manager, change, "POST", change_body).status == 401);
    const Reply logged = request(manager, login, "POST",
        {{QStringLiteral("username"), QStringLiteral("engineer")},
         {QStringLiteral("password"), QString::fromUtf8(initial)}});
    REQUIRE(logged.status == 200);
    const QByteArray token = logged.body.value(QStringLiteral("bearer_token")).toString().toLatin1();
    REQUIRE(token.size() == 64);
    REQUIRE(request(manager, change, "GET", {}, token).status != 200);
    REQUIRE(request(manager, change, "POST",
        {{QStringLiteral("current_password"), QStringLiteral("bad-password")},
         {QStringLiteral("new_password"), QString::fromUtf8(replacement)}}, token).status == 403);
    REQUIRE(request(manager, current, "GET", {}, token).status == 200);
    // A valid session must not turn enormous current-password strings into
    // expensive PBKDF2 work or break subsequent authenticated requests.
    REQUIRE(request(manager, change, "POST",
        {{QStringLiteral("current_password"), QString(1025, QLatin1Char('x'))},
         {QStringLiteral("new_password"), QString::fromUtf8(replacement)}}, token).status == 403);
    REQUIRE(request(manager, current, "GET", {}, token).status == 200);
    REQUIRE(request(manager, change, "POST",
        {{QStringLiteral("current_password"), QString::fromUtf8(initial)},
         {QStringLiteral("new_password"), QStringLiteral("short")}}, token).status == 400);
    REQUIRE(request(manager, change, "POST",
        {{QStringLiteral("current_password"), QString::fromUtf8(initial)},
         {QStringLiteral("new_password"), QString::fromUtf8(replacement)},
         {QStringLiteral("user_uuid"), user->uuid.toString()}}, token).status == 400);
    const Reply changed = request(manager, change, "POST", change_body, token);
    REQUIRE(changed.status == 200);
    REQUIRE(changed.body.value(QStringLiteral("changed")).toBool());
    REQUIRE(request(manager, current, "GET", {}, token).status == 401);
    REQUIRE(request(manager, change, "POST", change_body, token).status == 401);
    REQUIRE(request(manager, login, "POST",
        {{QStringLiteral("username"), QStringLiteral("engineer")},
         {QStringLiteral("password"), QString::fromUtf8(initial)}}).status == 401);
    REQUIRE(request(manager, login, "POST",
        {{QStringLiteral("username"), QStringLiteral("engineer")},
         {QStringLiteral("password"), QString::fromUtf8(replacement)}}).status == 200);
    REQUIRE(database.verifySecurityAudit(admin).valid);
    return 0;
}
