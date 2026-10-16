#include <aowis/db/database.h>
#include <aowis/db/http_hardening.h>
#include <aowis/db/http_routes.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#include <QHttpServerConfiguration>
#endif
#include <QHttpServerRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct HttpResult
{
    int status = 0;
    QByteArray cache_control;
    QByteArray pragma;
    QByteArray content_options;
    QByteArray referrer_policy;
    QByteArray allow_origin;
};

HttpResult get(QNetworkAccessManager &manager, const QUrl &url, bool spoof_proxy)
{
    QNetworkRequest request(url);
    if (spoof_proxy)
    {
        request.setRawHeader("X-Forwarded-User", "administrator");
        request.setRawHeader("X-Forwarded-Proto", "https");
        request.setRawHeader("Origin", "https://untrusted.example");
    }
    QNetworkReply *reply = manager.get(request);
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
    const HttpResult result{
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        reply->rawHeader("Cache-Control"),
        reply->rawHeader("Pragma"),
        reply->rawHeader("X-Content-Type-Options"),
        reply->rawHeader("Referrer-Policy"),
        reply->rawHeader("Access-Control-Allow-Origin")
    };
    reply->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;

    const QByteArray valid_token(64, 'a');
    REQUIRE(aowis::db::parseBearerAuthorization(QByteArrayLiteral("Bearer ") + valid_token) == valid_token);
    REQUIRE(!aowis::db::parseBearerAuthorization(QByteArrayLiteral("bearer ") + valid_token));
    REQUIRE(!aowis::db::parseBearerAuthorization(QByteArrayLiteral("Bearer ") + valid_token + QByteArrayLiteral(" ")));
    REQUIRE(!aowis::db::parseBearerAuthorization(QByteArrayLiteral("Bearer ") + QByteArray(64, 'G')));
    REQUIRE(!aowis::db::parseBearerAuthorization(QByteArrayLiteral("Bearer ") + QByteArray(63, 'a')));
    REQUIRE(!aowis::db::parseBearerAuthorization(QByteArrayLiteral("Bearer ") + QByteArray(5000, 'a')));
    REQUIRE(!aowis::db::parseBearerAuthorization(QByteArrayLiteral("Basic abc")));

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database db;
    REQUIRE(db.openSqlite(directory.filePath(QStringLiteral("hardening.sqlite")), &error));
    QHttpServer server;
    aowis::db::configureHttpHardening(server);

#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    REQUIRE(server.configuration().rateLimitPerSecond() == 120);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    REQUIRE(server.configuration().maximumBodySize() == 8 * 1024 * 1024);
    REQUIRE(server.configuration().maximumUrlSize() == 8192);
    REQUIRE(server.configuration().maximumTotalHeaderSize() == 16384);
    REQUIRE(server.configuration().maximumHeaderFieldSize() == 4096);
    REQUIRE(server.configuration().maximumHeaderFieldCount() == 64);
#endif

    aowis::db::HttpRoutes routes(db, [](const QHttpServerRequest &)
        -> std::optional<OperationContext> { return std::nullopt; });
    REQUIRE(routes.registerOn(server));
    QTcpServer *listener = new QTcpServer(&server);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(server.bind(listener));
    QNetworkAccessManager manager;
    const QUrl base(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener->serverPort()));
    const HttpResult health = get(manager, base.resolved(QUrl(QStringLiteral("health"))), false);
    REQUIRE(health.status == 200);
    const HttpResult private_result = get(manager, base.resolved(QUrl(QStringLiteral("projects"))), true);
    REQUIRE(private_result.status == 401);
    REQUIRE(private_result.allow_origin.isEmpty());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    REQUIRE(health.cache_control == QByteArrayLiteral("no-store"));
    REQUIRE(health.pragma == QByteArrayLiteral("no-cache"));
    REQUIRE(health.content_options == QByteArrayLiteral("nosniff"));
    REQUIRE(health.referrer_policy == QByteArrayLiteral("no-referrer"));
    REQUIRE(private_result.cache_control == QByteArrayLiteral("no-store"));
#endif
    return 0;
}
