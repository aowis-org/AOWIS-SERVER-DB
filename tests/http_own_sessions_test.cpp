#include <aowis/db/database.h>
#include <aowis/db/http_hardening.h>
#include <aowis/db/http_routes.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct Reply
{
    int status = 0;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

Reply send(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &token = {},
           const QByteArray &method = "GET", const QByteArray &body = {})
{
    QNetworkRequest request(url);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token);
    QNetworkReply *reply = method == QByteArrayLiteral("DELETE")
        ? manager.sendCustomRequest(request, method, body) : manager.get(request);
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
    const Reply result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll()};
    if (result.status == 0)
        qWarning().noquote() << "HTTP transport error:" << reply->errorString();
    reply->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("own-sessions.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> alice = db.createUser(root, QStringLiteral("alice"), &error);
    const std::optional<aowis::db::UserInfo> bob = db.createUser(root, QStringLiteral("bob"), &error);
    REQUIRE(alice.has_value() && bob.has_value());
    const std::optional<aowis::db::IssuedSession> first = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> second = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> third = db.issueSession(root, alice->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> other = db.issueSession(root, bob->uuid, 3600, &error);
    REQUIRE(first && second && third && other);

    // The library API never returns tokens or sessions belonging to Bob.
    const QVector<aowis::db::OwnSessionInfo> list = db.ownSessions(first->bearer_token, {}, 100, &error);
    REQUIRE(error.isEmpty() && list.size() == 3);
    QSet<QUuid> ids;
    int current_count = 0;
    for (const aowis::db::OwnSessionInfo &entry : list)
    {
        ids.insert(entry.uuid);
        REQUIRE(entry.created_at_ms > 0 && entry.expires_at_ms > entry.created_at_ms);
        if (entry.current)
        {
            ++current_count;
            REQUIRE(entry.uuid == first->uuid);
        }
    }
    REQUIRE(current_count == 1 && ids.contains(first->uuid) && ids.contains(second->uuid) &&
            ids.contains(third->uuid) && !ids.contains(other->uuid));
    REQUIRE(db.ownSessions(first->bearer_token, {}, 0, &error).isEmpty());
    REQUIRE(error.contains(QStringLiteral("limit")));
    error.clear();

    QHttpServer http;
    aowis::db::configureHttpHardening(http);
    aowis::db::HttpRoutes routes(db, [&db](const QHttpServerRequest &request)
        -> std::optional<OperationContext> {
        const std::optional<QByteArray> token = aowis::db::parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        return token ? db.authenticateSession(*token) : std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer listener(&http);
    REQUIRE(listener.listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(&listener));
    const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(listener.serverPort());
    const QUrl listing(origin + QStringLiteral("/api/v1/sessions"));
    const auto revokeUrl = [&origin](const QUuid &uuid) {
        return QUrl(origin + QStringLiteral("/api/v1/sessions/") +
                    uuid.toString(QUuid::WithoutBraces));
    };
    QNetworkAccessManager manager;
    REQUIRE(send(manager, listing).status == 401);
    REQUIRE(send(manager, listing, QByteArray(64, 'e')).status == 401);
    REQUIRE(send(manager, QUrl(listing.toString() + QStringLiteral("?limit=0")), first->bearer_token).status == 400);
    REQUIRE(send(manager, QUrl(listing.toString() + QStringLiteral("?after_uuid=bad")), first->bearer_token).status == 400);
    REQUIRE(send(manager, QUrl(listing.toString() + QStringLiteral("?limit=1&limit=2")), first->bearer_token).status == 400);
    REQUIRE(send(manager, QUrl(listing.toString() + QStringLiteral("?hidden=true")), first->bearer_token).status == 400);
    REQUIRE(send(manager, revokeUrl(second->uuid), {}, "DELETE").status == 401);
    REQUIRE(send(manager, QUrl(origin + QStringLiteral("/api/v1/sessions/not-a-uuid")),
                 first->bearer_token, "DELETE").status == 400);
    REQUIRE(send(manager, revokeUrl(second->uuid), first->bearer_token, "DELETE", "{}").status == 400);

    const Reply found = send(manager, listing, first->bearer_token);
    REQUIRE(found.status == 200);
    REQUIRE(found.json().value(QStringLiteral("sessions")).toArray().size() == 3);
    REQUIRE(!found.body.contains(first->bearer_token));
    REQUIRE(!found.body.contains(other->bearer_token));
    // Strict UUID cursor pagination returns exactly the three owned sessions.
    QSet<QString> paginated;
    QString cursor;
    int page_current_count = 0;
    for (int i = 0; i < 4; ++i)
    {
        QUrl url(listing);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        if (!cursor.isEmpty())
            query.addQueryItem(QStringLiteral("after_uuid"), cursor);
        url.setQuery(query);
        const Reply page = send(manager, url, first->bearer_token);
        REQUIRE(page.status == 200);
        const QJsonArray sessions = page.json().value(QStringLiteral("sessions")).toArray();
        if (sessions.isEmpty())
            break;
        REQUIRE(sessions.size() == 1);
        const QJsonObject item = sessions.first().toObject();
        cursor = item.value(QStringLiteral("session_uuid")).toString();
        REQUIRE(!paginated.contains(cursor));
        paginated.insert(cursor);
        if (item.value(QStringLiteral("current")).toBool())
            ++page_current_count;
    }
    REQUIRE(paginated.size() == 3 && page_current_count == 1);
    const Reply bobs = send(manager, listing, other->bearer_token);
    REQUIRE(bobs.status == 200 && bobs.json().value(QStringLiteral("sessions")).toArray().size() == 1);

    // No user can revoke a different user's sessions, including an admin.
    REQUIRE(send(manager, revokeUrl(other->uuid), first->bearer_token, "DELETE").status == 404);
    REQUIRE(db.activeSession(other->bearer_token).has_value());
    REQUIRE(send(manager, revokeUrl(second->uuid), first->bearer_token, "DELETE").status == 200);
    REQUIRE(!db.activeSession(second->bearer_token));
    REQUIRE(db.activeSession(first->bearer_token).has_value());
    REQUIRE(db.activeSession(third->bearer_token).has_value());
    REQUIRE(send(manager, revokeUrl(second->uuid), first->bearer_token, "DELETE").status == 404);

    // Rejecting the audit insert must roll back the session revocation.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("own-sessions-inspect"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_own_revoke BEFORE INSERT ON security_audit_events "
            "WHEN NEW.action='session.revoke.own' "
            "BEGIN SELECT RAISE(ABORT, 'intentional audit failure'); END")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("own-sessions-inspect"));
    REQUIRE(send(manager, revokeUrl(third->uuid), first->bearer_token, "DELETE").status == 500);
    REQUIRE(db.activeSession(third->bearer_token).has_value());
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("own-sessions-inspect-2"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_own_revoke")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("own-sessions-inspect-2"));
    REQUIRE(send(manager, revokeUrl(third->uuid), first->bearer_token, "DELETE").status == 200);
    REQUIRE(!db.activeSession(third->bearer_token));
    REQUIRE(send(manager, revokeUrl(first->uuid), first->bearer_token, "DELETE").status == 200);
    REQUIRE(!db.activeSession(first->bearer_token));
    REQUIRE(send(manager, listing, first->bearer_token).status == 401);
    REQUIRE(db.activeSession(other->bearer_token).has_value());
    const QVector<aowis::db::SecurityAuditEvent> audit = db.securityAudit(root, 0, 100, &error);
    int self_revoke_count = 0;
    int logout_count = 0;
    for (const aowis::db::SecurityAuditEvent &event : audit)
    {
        if (event.action == QStringLiteral("session.revoke.own"))
        {
            ++self_revoke_count;
            REQUIRE(event.actor_uuid == alice->uuid);
        }
        if (event.action == QStringLiteral("session.logout"))
        {
            ++logout_count;
            REQUIRE(event.subject_uuid == first->uuid && event.actor_uuid == alice->uuid);
        }
    }
    REQUIRE(self_revoke_count == 2 && logout_count == 1);
    REQUIRE(db.verifySecurityAudit(root).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(!db.authenticateSession(first->bearer_token));
    REQUIRE(!db.authenticateSession(third->bearer_token));
    REQUIRE(db.authenticateSession(other->bearer_token)->actor_uuid == bob->uuid);
    REQUIRE(db.verifySecurityAudit(root).valid);
    return 0;
}
