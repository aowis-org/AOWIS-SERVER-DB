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

Reply request(QNetworkAccessManager &manager, const QUrl &url, const QJsonObject &body)
{
    QNetworkRequest message(url);
    message.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *network = manager.post(message, QJsonDocument(body).toJson(QJsonDocument::Compact));
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
    const QByteArray payload = network->readAll();
    Reply result{network->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                 QJsonDocument::fromJson(payload).object()};
    if (!result.status)
        qWarning().noquote() << "HTTP network error" << network->errorString();
    network->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    aowis::db::Database database;
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("recovery-http.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = database.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const aowis::db::OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> user = database.createUser(root, QStringLiteral("worker"), &error);
    REQUIRE(user.has_value());
    const QByteArray original("correct initial password");
    const QByteArray replacement("correct replacement password");
    REQUIRE(database.setUserPassword(root, user->uuid, original, &error));
    const std::optional<aowis::db::IssuedRecoveryToken> recovery =
        database.issuePasswordRecovery(root, user->uuid, 900, &error);
    REQUIRE(recovery.has_value());
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
    const QString host = QStringLiteral("http://127.0.0.1:%1").arg(listener.serverPort());
    const QUrl login(host + QStringLiteral("/api/v1/session/login"));
    const QUrl redeem(host + QStringLiteral("/api/v1/session/recover"));
    QNetworkAccessManager manager;
    const QJsonObject body{{QStringLiteral("recovery_token"), QString::fromLatin1(recovery->token)},
                           {QStringLiteral("new_password"), QString::fromUtf8(replacement)}};
    REQUIRE(request(manager, redeem, {{QStringLiteral("recovery_token"), QStringLiteral("bad")},
                                      {QStringLiteral("new_password"), QString::fromUtf8(replacement)}}).status == 401);
    REQUIRE(request(manager, redeem, {{QStringLiteral("recovery_token"), QString::fromLatin1(recovery->token)},
                                      {QStringLiteral("new_password"), QStringLiteral("short")}}).status == 400);
    REQUIRE(request(manager, redeem, {{QStringLiteral("recovery_token"), QString::fromLatin1(recovery->token)},
                                      {QStringLiteral("new_password"), QString::fromUtf8(replacement)},
                                      {QStringLiteral("user_uuid"), user->uuid.toString()}}).status == 400);
    REQUIRE(request(manager, login, {{QStringLiteral("username"), QStringLiteral("worker")},
                                     {QStringLiteral("password"), QString::fromUtf8(original)}}).status == 200);
    const Reply changed = request(manager, redeem, body);
    REQUIRE(changed.status == 200);
    REQUIRE(changed.body.value(QStringLiteral("changed")).toBool());
    REQUIRE(request(manager, redeem, body).status == 401);
    REQUIRE(request(manager, login, {{QStringLiteral("username"), QStringLiteral("worker")},
                                     {QStringLiteral("password"), QString::fromUtf8(original)}}).status == 401);
    REQUIRE(request(manager, login, {{QStringLiteral("username"), QStringLiteral("worker")},
                                     {QStringLiteral("password"), QString::fromUtf8(replacement)}}).status == 200);
    REQUIRE(database.verifySecurityAudit(root).valid);
    return 0;
}
