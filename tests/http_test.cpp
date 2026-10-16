#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QJsonArray>
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

using aowis::db::Database;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct HttpResult
{
    int status = 0;
    QByteArray body;
};

HttpResult send(QNetworkAccessManager &manager, const QUrl &url,
                const QByteArray &method, const QByteArray &body = {},
                const QByteArray &authorization = {})
{
    QNetworkRequest request(url);
    if (!authorization.isEmpty())
        request.setRawHeader("Authorization", authorization);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = method == "GET" ? manager.get(request)
                                             : manager.sendCustomRequest(request, method, body);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!reply->isFinished())
        loop.exec();
    const HttpResult result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll()};
    reply->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database database;
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("http.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = database.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const std::optional<aowis::db::UserInfo> guest = database.createUser(
        {admin->uuid}, QStringLiteral("guest"), &error);
    REQUIRE(guest.has_value());

    QHttpServer http;
    aowis::db::HttpRoutes routes(database, [admin, guest](const QHttpServerRequest &request)
                                -> std::optional<aowis::db::OperationContext> {
        const QByteArray authorization = request.value(QByteArrayLiteral("authorization"));
        if (authorization == QByteArrayLiteral("Bearer administrator-test-token"))
            return aowis::db::OperationContext{admin->uuid};
        if (authorization == QByteArrayLiteral("Bearer guest-test-token"))
            return aowis::db::OperationContext{guest->uuid};
        return std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer *listener = new QTcpServer(&http);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(listener));
    QNetworkAccessManager manager;
    const QUrl base(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener->serverPort()));
    const QByteArray authorized("Bearer administrator-test-token");
    const QByteArray guest_authorized("Bearer guest-test-token");

    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("health"))), "GET").status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "GET").status == 401);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "GET", {}, guest_authorized).status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "POST",
                 QByteArrayLiteral("{\"name\":\"Denied\"}"), guest_authorized).status == 403);

    const HttpResult created = send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "POST",
                                    QByteArrayLiteral("{\"name\":\"Test Network\"}"), authorized);
    REQUIRE(created.status == 201);
    const QJsonObject created_json = QJsonDocument::fromJson(created.body).object();
    const QString project_id = created_json.value(QStringLiteral("project_uuid")).toString();
    const QString branch_id = created_json.value(QStringLiteral("main_branch_uuid")).toString();
    const QString revision_id = created_json.value(QStringLiteral("working_revision_uuid")).toString();
    REQUIRE(!project_id.isEmpty() && !branch_id.isEmpty() && !revision_id.isEmpty());

    const HttpResult listed = send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "GET", {}, authorized);
    REQUIRE(listed.status == 200);
    REQUIRE(QJsonDocument::fromJson(listed.body).object().value(QStringLiteral("projects")).toArray().size() == 1);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects/") + project_id)), "GET", {}, authorized).status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects/") + project_id)), "GET", {}, guest_authorized).status == 404);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects/") + project_id + QStringLiteral("/branches"))),
                 "GET", {}, authorized).status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("branches/") + branch_id + QStringLiteral("/revisions"))),
                 "GET", {}, authorized).status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("revisions/") + revision_id)), "GET", {}, authorized).status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("revisions/not-a-uuid"))), "GET", {}, authorized).status == 400);
    // Arbitrary caller-provided user identity cannot substitute for a token.
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "GET", {},
                 QByteArrayLiteral("Bearer forged" )).status == 401);
    const QUuid object_uuid = createUuidV7();
    REQUIRE(database.putObject({admin->uuid}, QUuid(branch_id), object_uuid,
                               QStringLiteral("test.payload"), QByteArrayLiteral("sample bytes"), -1, &error));
    const QString object_path = QStringLiteral("revisions/") + revision_id +
                                QStringLiteral("/objects/") + object_uuid.toString(QUuid::WithoutBraces);
    const HttpResult object_read = send(manager, base.resolved(QUrl(object_path)), "GET", {}, authorized);
    REQUIRE(object_read.status == 200);
    REQUIRE(QByteArray::fromBase64(QJsonDocument::fromJson(object_read.body).object()
                .value(QStringLiteral("content_base64")).toString().toLatin1()) == QByteArrayLiteral("sample bytes"));
    REQUIRE(send(manager, base.resolved(QUrl(object_path)), "GET", {}, guest_authorized).status == 404);

    // Generation is 1 after the object write. Stale finalization must fail;
    // correct generation must succeed and preserve the normal core hash chain.
    const QString finalize_path = QStringLiteral("branches/") + branch_id + QStringLiteral("/finalize");
    REQUIRE(send(manager, base.resolved(QUrl(finalize_path)), "POST",
                 QByteArrayLiteral("{\"expected_generation\":\"0\",\"title\":\"initial\"}"), authorized).status == 409);
    REQUIRE(database.revisions({admin->uuid}, QUuid(branch_id)).size() == 1);
    const HttpResult finalized = send(manager, base.resolved(QUrl(finalize_path)), "POST",
                 QByteArrayLiteral("{\"expected_generation\":\"1\",\"title\":\"initial\"}"), authorized);
    REQUIRE(finalized.status == 200);
    REQUIRE(database.revisions({admin->uuid}, QUuid(branch_id)).size() == 2);
    REQUIRE(database.verifyProject({admin->uuid}, QUuid(project_id)).valid);
    return 0;
}
