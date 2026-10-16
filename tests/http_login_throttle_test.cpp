#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>
#include <aowis/db/login_throttle.h>

#include <QCoreApplication>
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
#include <QDebug>

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

namespace
{
int post(QNetworkAccessManager &manager, const QUrl &url, const QJsonObject &body)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!reply->isFinished())
        loop.exec();
    if (!reply->isFinished())
        reply->abort();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!status)
        qWarning().noquote() << "Network failure:" << reply->errorString();
    reply->deleteLater();
    return status;
}

QJsonObject login(const QString &username)
{
    // Empty password is invalid before PBKDF2, but a syntactically valid HTTP
    // login attempt must still consume throttle capacity.
    return {{QStringLiteral("username"), username}, {QStringLiteral("password"), QStringLiteral("")}};
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    aowis::db::LoginThrottle limiter;

    // Deterministic sliding-window tests: no sleeps and no expensive hashing.
    for (int i = 0; i < aowis::db::LoginThrottle::per_username_max; ++i)
        REQUIRE(limiter.allowAttempt(QStringLiteral("engineer"), 0));
    REQUIRE(!limiter.allowAttempt(QStringLiteral("ENGINEER"), 0));
    REQUIRE(limiter.allowAttempt(QStringLiteral("different"), 0));
    REQUIRE(!limiter.allowAttempt(QStringLiteral("engineer"), 59999));
    REQUIRE(limiter.allowAttempt(QStringLiteral("engineer"), 60000));
    REQUIRE(limiter.allowAttempt(QString::fromUtf8("Å"), 60000));
    // Different Unicode forms must share one bucket.
    aowis::db::LoginThrottle unicode;
    for (int i = 0; i < aowis::db::LoginThrottle::per_username_max; ++i)
        REQUIRE(unicode.allowAttempt(QString::fromUtf8("Å"), 0));
    REQUIRE(!unicode.allowAttempt(QString::fromUtf8("A\xcc\x8a"), 0));

    aowis::db::LoginThrottle global;
    for (int i = 0; i < aowis::db::LoginThrottle::global_max; ++i)
        REQUIRE(global.allowAttempt(QStringLiteral("unknown-%1").arg(i), 0));
    REQUIRE(!global.allowAttempt(QStringLiteral("another"), 0));
    REQUIRE(global.allowAttempt(QStringLiteral("another"), 60000));

    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    aowis::db::Database database;
    REQUIRE(database.openSqlite(temp.filePath(QStringLiteral("throttle.sqlite")), &error));
    QHttpServer http;
    aowis::db::HttpRoutes routes(database,
        [](const QHttpServerRequest &) -> std::optional<aowis::db::OperationContext> {
            return std::nullopt;
        });
    REQUIRE(routes.registerOn(http));
    QTcpServer listener;
    REQUIRE(listener.listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(&listener));
    const QUrl address(QStringLiteral("http://127.0.0.1:%1/api/v1/session/login")
        .arg(listener.serverPort()));
    QNetworkAccessManager manager;
    REQUIRE(post(manager, address, {{QStringLiteral("username"), QStringLiteral("x")}}) == 400);
    for (int i = 0; i < aowis::db::LoginThrottle::per_username_max; ++i)
        REQUIRE(post(manager, address, login(QStringLiteral("engineer"))) == 401);
    REQUIRE(post(manager, address, login(QStringLiteral("ENGINEER"))) == 429);
    REQUIRE(post(manager, address, login(QStringLiteral("another"))) == 401);
    for (int i = 0; i < aowis::db::LoginThrottle::global_max
                      - aowis::db::LoginThrottle::per_username_max - 1; ++i)
        REQUIRE(post(manager, address, login(QStringLiteral("extra-%1").arg(i))) == 401);
    REQUIRE(post(manager, address, login(QStringLiteral("still-another"))) == 429);
    return 0;
}
