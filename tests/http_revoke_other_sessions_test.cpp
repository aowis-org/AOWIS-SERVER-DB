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

struct HttpReply
{
    int status = 0;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

HttpReply post(QNetworkAccessManager &manager, const QUrl &url,
               const QByteArray &bearer = {}, const QByteArray &body = {})
{
    QNetworkRequest request(url);
    if (!bearer.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearer);
    QNetworkReply *reply = manager.post(request, body);
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
    HttpReply output{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll()};
    if (output.status == 0)
        qWarning().noquote() << "HTTP transport error:" << reply->errorString();
    reply->deleteLater();
    return output;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("other-sessions.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> alice = db.createUser(root, QStringLiteral("alice"), &error);
    const std::optional<aowis::db::UserInfo> bob = db.createUser(root, QStringLiteral("bob"), &error);
    REQUIRE(alice && bob);
    const std::optional<aowis::db::IssuedSession> keeper = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> extra1 = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> extra2 = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> other_user = db.issueSession(root, bob->uuid, 3600, &error);
    REQUIRE(keeper && extra1 && extra2 && other_user);

    QHttpServer http;
    aowis::db::configureHttpHardening(http);
    aowis::db::HttpRoutes routes(db, [&db](const QHttpServerRequest &request)
        -> std::optional<OperationContext> {
        const std::optional<QByteArray> token = aowis::db::parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        return token ? db.authenticateSession(*token) : std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer listener;
    REQUIRE(listener.listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(&listener));
    QNetworkAccessManager manager;
    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/api/v1/session/revoke-others")
        .arg(listener.serverPort()));
    QUrl unexpected(endpoint);
    unexpected.setQuery(QStringLiteral("user_uuid=other"));
    REQUIRE(post(manager, endpoint).status == 401);
    REQUIRE(post(manager, endpoint, QByteArray(64, 'f')).status == 401);
    REQUIRE(post(manager, unexpected, keeper->bearer_token).status == 400);
    REQUIRE(post(manager, endpoint, keeper->bearer_token, QByteArrayLiteral("{}" )).status == 400);

    // Force an audit failure and ensure no session revocation escaped the
    // transaction. A successful response must never precede its audit event.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("revoke-others-inspector"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_revoke_others BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='session.revoke_others' "
            "BEGIN SELECT RAISE(ABORT, 'intentional audit failure'); END")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("revoke-others-inspector"));
    REQUIRE(post(manager, endpoint, keeper->bearer_token).status == 500);
    REQUIRE(db.activeSession(keeper->bearer_token).has_value());
    REQUIRE(db.activeSession(extra1->bearer_token).has_value());
    REQUIRE(db.activeSession(extra2->bearer_token).has_value());
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("revoke-others-inspector-2"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_revoke_others")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("revoke-others-inspector-2"));

    const HttpReply revoked = post(manager, endpoint, keeper->bearer_token);
    REQUIRE(revoked.status == 200);
    REQUIRE(revoked.json().value(QStringLiteral("revoked_count")).toString() == QStringLiteral("2"));
    REQUIRE(revoked.json().value(QStringLiteral("current_session_uuid")).toString() ==
            keeper->uuid.toString(QUuid::WithoutBraces));
    REQUIRE(db.activeSession(keeper->bearer_token).has_value());
    REQUIRE(!db.activeSession(extra1->bearer_token));
    REQUIRE(!db.activeSession(extra2->bearer_token));
    REQUIRE(db.activeSession(other_user->bearer_token).has_value());
    REQUIRE(post(manager, endpoint, extra1->bearer_token).status == 401);

    // Idempotent on the remaining active session; no other user is affected.
    const HttpReply again = post(manager, endpoint, keeper->bearer_token);
    REQUIRE(again.status == 200);
    REQUIRE(again.json().value(QStringLiteral("revoked_count")).toString() == QStringLiteral("0"));
    REQUIRE(db.activeSession(keeper->bearer_token).has_value());
    REQUIRE(db.activeSession(other_user->bearer_token).has_value());

    const QVector<aowis::db::SecurityAuditEvent> audit = db.securityAudit(root, 0, 200, &error);
    int revocations = 0;
    for (const aowis::db::SecurityAuditEvent &event : audit)
    {
        if (event.action != QStringLiteral("session.revoke_others"))
            continue;
        ++revocations;
        REQUIRE(event.actor_uuid == alice->uuid);
        REQUIRE(event.subject_uuid == keeper->uuid);
    }
    REQUIRE(revocations == 2);
    REQUIRE(db.verifySecurityAudit(root).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.activeSession(keeper->bearer_token).has_value());
    REQUIRE(!db.activeSession(extra1->bearer_token));
    REQUIRE(db.activeSession(other_user->bearer_token).has_value());
    REQUIRE(db.verifySecurityAudit(root).valid);
    return 0;
}
