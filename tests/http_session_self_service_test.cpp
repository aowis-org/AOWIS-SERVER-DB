#include <aowis/db/database.h>
#include <aowis/db/http_hardening.h>
#include <aowis/db/http_routes.h>

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
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct Reply
{
    int status = 0;
    QByteArray body;
    QByteArray cache_control;
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

Reply send(QNetworkAccessManager &manager, const QUrl &url,
           const QByteArray &method, const QByteArray &token = {}, const QByteArray &body = {})
{
    QNetworkRequest request(url);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token);
    if (!body.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *network = nullptr;
    if (method == QByteArrayLiteral("DELETE"))
        network = manager.sendCustomRequest(request, method, body);
    else
        network = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(network, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(5000);
    if (!network->isFinished())
        loop.exec();
    if (!network->isFinished())
        network->abort();
    const Reply result{network->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                       network->readAll(), network->rawHeader("Cache-Control")};
    if (result.status == 0)
        qWarning().noquote() << "HTTP transport error:" << network->errorString();
    network->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("self-service.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(
        QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> user = db.createUser(
        root, QStringLiteral("field-engineer"), &error);
    REQUIRE(user.has_value());
    const std::optional<aowis::db::IssuedSession> one = db.issueSession(root, user->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> two = db.issueSession(root, user->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> administrator = db.issueSession(
        root, admin->uuid, 3600, &error);
    REQUIRE(one && two && administrator);
    REQUIRE(!db.activeSession(QByteArrayLiteral("invalid"), &error));
    REQUIRE(!db.logoutSession(QByteArrayLiteral("invalid"), &error));
    REQUIRE(db.activeSession(one->bearer_token, &error)->uuid == one->uuid);

    QHttpServer http;
    aowis::db::configureHttpHardening(http);
    aowis::db::HttpRoutes routes(db, [&db](const QHttpServerRequest &request)
        -> std::optional<OperationContext> {
        const std::optional<QByteArray> token = aowis::db::parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        return token ? db.authenticateSession(*token) : std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer *listener = new QTcpServer(&http);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(listener));
    QNetworkAccessManager manager;
    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/v1/session/current")
        .arg(listener->serverPort()));
    const QUrl extra(QStringLiteral("http://127.0.0.1:%1/api/v1/session/current?session_uuid=other")
        .arg(listener->serverPort()));

    REQUIRE(send(manager, endpoint, "GET").status == 401);
    REQUIRE(send(manager, endpoint, "DELETE").status == 401);
    REQUIRE(send(manager, endpoint, "GET", QByteArray(64, 'f')).status == 401);
    REQUIRE(send(manager, extra, "GET", one->bearer_token).status == 400);
    REQUIRE(send(manager, extra, "DELETE", one->bearer_token).status == 400);
    REQUIRE(send(manager, endpoint, "DELETE", one->bearer_token, QByteArrayLiteral("{}")).status == 400);
    REQUIRE(db.activeSession(one->bearer_token, &error).has_value());

    const Reply profile = send(manager, endpoint, "GET", one->bearer_token);
    REQUIRE(profile.status == 200);
    REQUIRE(profile.json().value(QStringLiteral("session_uuid")).toString() ==
        one->uuid.toString(QUuid::WithoutBraces));
    REQUIRE(profile.json().value(QStringLiteral("user_uuid")).toString() ==
        user->uuid.toString(QUuid::WithoutBraces));
    REQUIRE(profile.json().value(QStringLiteral("expires_at_ms")).toString() ==
        QString::number(one->expires_at_ms));
    REQUIRE(!profile.body.contains(one->bearer_token));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    REQUIRE(profile.cache_control == QByteArrayLiteral("no-store"));
#endif

    REQUIRE(send(manager, endpoint, "DELETE", one->bearer_token).status == 200);
    REQUIRE(!db.authenticateSession(one->bearer_token, &error));
    REQUIRE(!db.activeSession(one->bearer_token, &error));
    REQUIRE(!db.logoutSession(one->bearer_token, &error));
    REQUIRE(send(manager, endpoint, "GET", one->bearer_token).status == 401);
    REQUIRE(send(manager, endpoint, "DELETE", one->bearer_token).status == 401);
    REQUIRE(send(manager, endpoint, "GET", two->bearer_token).status == 200);
    REQUIRE(send(manager, endpoint, "GET", administrator->bearer_token).status == 200);

    const QVector<aowis::db::SecurityAuditEvent> events = db.securityAudit(root, 0, 200, &error);
    int logout_events = 0;
    for (const aowis::db::SecurityAuditEvent &event : events)
    {
        if (event.action != QStringLiteral("session.logout"))
            continue;
        ++logout_events;
        REQUIRE(event.actor_uuid == user->uuid);
        REQUIRE(event.subject_uuid == one->uuid);
    }
    REQUIRE(logout_events == 1);
    REQUIRE(db.verifySecurityAudit(root).valid);

    // Even a privileged SQL trigger rejecting the audit insert must not
    // leave the token revoked without a corresponding audit event.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("self-service-inspect"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_logout BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='session.logout' "
            "BEGIN SELECT RAISE(ABORT, 'intentional audit failure'); END")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("self-service-inspect"));
    REQUIRE(send(manager, endpoint, "DELETE", two->bearer_token).status == 500);
    REQUIRE(!db.logoutSession(two->bearer_token, &error));
    REQUIRE(db.authenticateSession(two->bearer_token)->actor_uuid == user->uuid);
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("self-service-inspect-2"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_logout")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("self-service-inspect-2"));
    REQUIRE(db.logoutSession(two->bearer_token, &error));
    REQUIRE(!db.authenticateSession(two->bearer_token));
    REQUIRE(db.verifySecurityAudit(root).valid);

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(!db.authenticateSession(one->bearer_token, &error));
    REQUIRE(!db.authenticateSession(two->bearer_token));
    REQUIRE(db.authenticateSession(administrator->bearer_token)->actor_uuid == admin->uuid);
    REQUIRE(db.verifySecurityAudit(root).valid);
    return 0;
}
