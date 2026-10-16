#include <aowis/db/database.h>
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
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

Reply send(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &token,
           const QByteArray &method, const QJsonObject &object = {}, bool with_body = true)
{
    QNetworkRequest message(url);
    if (!token.isEmpty())
        message.setRawHeader("Authorization", QByteArray("Bearer ") + token);
    message.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray bytes = with_body ? QJsonDocument(object).toJson(QJsonDocument::Compact) : QByteArray();
    QNetworkReply *network = method == "POST" ? manager.post(message, bytes) :
        method == "DELETE" ? manager.sendCustomRequest(message, "DELETE") : manager.get(message);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(network, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!network->isFinished())
        loop.exec();
    if (!network->isFinished())
        network->abort();
    Reply result{network->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), network->readAll()};
    if (result.status == 0)
        qWarning().noquote() << network->errorString();
    network->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    Database db;
    REQUIRE(db.openSqlite(temp.filePath(QStringLiteral("security-http.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> guest = db.createUser(root, QStringLiteral("guest"), &error);
    REQUIRE(guest);
    const std::optional<aowis::db::IssuedSession> admin_session = db.issueSession(root, admin->uuid, 3600, &error);
    const std::optional<aowis::db::IssuedSession> guest_session = db.issueSession(root, guest->uuid, 3600, &error);
    REQUIRE(admin_session && guest_session);

    QHttpServer server;
    aowis::db::HttpRoutes routes(db, [&db](const QHttpServerRequest &request)
        -> std::optional<OperationContext> {
        const QByteArray authorization = request.value(QByteArrayLiteral("authorization"));
        if (!authorization.startsWith("Bearer "))
            return std::nullopt;
        return db.authenticateSession(authorization.mid(7));
    });
    REQUIRE(routes.registerOn(server));
    QTcpServer *listener = new QTcpServer(&server);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(server.bind(listener));
    QNetworkAccessManager manager;
    const QUrl base(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener->serverPort()));
    const QByteArray admin_token = admin_session->bearer_token;
    const QByteArray guest_token = guest_session->bearer_token;
    const QJsonObject new_user{{QStringLiteral("username"), QStringLiteral("engineer")}};
    const QString users_path = QStringLiteral("admin/users");
    const QUrl users_url = base.resolved(QUrl(users_path));
    REQUIRE(send(manager, users_url, {}, "POST", new_user).status == 401);
    REQUIRE(send(manager, users_url, guest_token, "POST", new_user).status == 403);
    REQUIRE(send(manager, users_url, admin_token, "POST", QJsonObject{
        {QStringLiteral("username"), QStringLiteral("spoof")},
        {QStringLiteral("actor_uuid"), admin->uuid.toString()}}).status == 400);
    REQUIRE(send(manager, users_url, admin_token, "POST", QJsonObject{
        {QStringLiteral("username"), 42}}).status == 400);
    const Reply created_user = send(manager, users_url, admin_token, "POST", new_user);
    REQUIRE(created_user.status == 201);
    const QString user_id = created_user.json().value(QStringLiteral("uuid")).toString();
    REQUIRE(!QUuid(user_id).isNull());
    const std::optional<aowis::db::IssuedSession> engineer_session =
        db.issueSession(root, QUuid(user_id), 3600, &error);
    REQUIRE(engineer_session);
    REQUIRE(send(manager, users_url, admin_token, "POST", new_user).status == 409);

    const Reply created_group = send(manager, base.resolved(QUrl(QStringLiteral("admin/groups"))), admin_token,
        "POST", {{QStringLiteral("name"), QStringLiteral("designers")}});
    REQUIRE(created_group.status == 201);
    const QString group_id = created_group.json().value(QStringLiteral("uuid")).toString();
    const Reply created_role = send(manager, base.resolved(QUrl(QStringLiteral("admin/roles"))), admin_token,
        "POST", {{QStringLiteral("key"), QStringLiteral("field.viewer")}});
    REQUIRE(created_role.status == 201);
    const QString role_id = created_role.json().value(QStringLiteral("uuid")).toString();
    const Reply custom_permission = send(manager, base.resolved(QUrl(QStringLiteral("admin/permissions"))), admin_token,
        "POST", {{QStringLiteral("key"), QStringLiteral("field.export")}});
    REQUIRE(custom_permission.status == 201);
    const QString permission_id = custom_permission.json().value(QStringLiteral("uuid")).toString();

    const QString group_members_path = QStringLiteral("admin/groups/%1/members").arg(group_id);
    const QUrl group_members_url = base.resolved(QUrl(group_members_path));
    REQUIRE(send(manager, group_members_url, admin_token, "POST",
        {{QStringLiteral("user_uuid"), QStringLiteral("not-uuid")}}).status == 400);
    const Reply member = send(manager, group_members_url, admin_token, "POST",
        {{QStringLiteral("user_uuid"), user_id}});
    REQUIRE(member.status == 201);
    const QString member_id = member.json().value(QStringLiteral("uuid")).toString();
    REQUIRE(send(manager, group_members_url, admin_token, "POST",
        {{QStringLiteral("user_uuid"), user_id}}).status == 409);

    const QUrl grants_url = base.resolved(QUrl(QStringLiteral("admin/roles/%1/permissions").arg(role_id)));
    const Reply grant = send(manager, grants_url, admin_token, "POST",
        {{QStringLiteral("permission_uuid"), permission_id}});
    REQUIRE(grant.status == 201);
    const QString grant_id = grant.json().value(QStringLiteral("uuid")).toString();
    const QUrl assignments_url = base.resolved(QUrl(
        QStringLiteral("admin/principals/group/%1/assignments").arg(group_id)));
    const QJsonObject scope{{QStringLiteral("role_uuid"), role_id},
                            {QStringLiteral("scope_kind"), QStringLiteral("system")},
                            {QStringLiteral("scope_uuid"), QJsonValue(QJsonValue::Null)}};
    REQUIRE(send(manager, assignments_url, admin_token, "POST", QJsonObject{
        {QStringLiteral("role_uuid"), role_id}, {QStringLiteral("scope_kind"), QStringLiteral("project")},
        {QStringLiteral("scope_uuid"), QJsonValue(QJsonValue::Null)}}).status == 400);
    const Reply assignment = send(manager, assignments_url, admin_token, "POST", scope);
    REQUIRE(assignment.status == 201);
    const QString assignment_id = assignment.json().value(QStringLiteral("uuid")).toString();
    const OperationContext engineer{QUuid(user_id)};
    REQUIRE(db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));

    const QUrl members_del = base.resolved(QUrl(QStringLiteral("admin/memberships/%1").arg(member_id)));
    REQUIRE(send(manager, members_del, guest_token, "DELETE", {}, false).status == 403);
    REQUIRE(send(manager, members_del, admin_token, "DELETE", {}, false).status == 200);
    REQUIRE(!db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));
    REQUIRE(send(manager, members_del, admin_token, "DELETE", {}, false).status == 409);
    const Reply regrant = send(manager, group_members_url, admin_token, "POST",
        {{QStringLiteral("user_uuid"), user_id}});
    REQUIRE(regrant.status == 201);
    REQUIRE(regrant.json().value(QStringLiteral("uuid")).toString() != member_id);
    REQUIRE(db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));

    const QUrl grants_del = base.resolved(QUrl(QStringLiteral("admin/permission-grants/%1").arg(grant_id)));
    REQUIRE(send(manager, grants_del, admin_token, "DELETE", {}, false).status == 200);
    REQUIRE(!db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));
    REQUIRE(send(manager, grants_url, admin_token, "POST",
        {{QStringLiteral("permission_uuid"), permission_id}}).status == 201);
    REQUIRE(db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));

    const QUrl assignment_del = base.resolved(QUrl(QStringLiteral("admin/role-assignments/%1").arg(assignment_id)));
    REQUIRE(send(manager, assignment_del, admin_token, "DELETE", {}, false).status == 200);
    REQUIRE(!db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));
    const std::optional<aowis::db::ProjectCreation> project_a = db.createProject(root, QStringLiteral("A"), &error);
    const std::optional<aowis::db::ProjectCreation> project_b = db.createProject(root, QStringLiteral("B"), &error);
    REQUIRE(project_a && project_b);
    const QJsonObject project_scope{{QStringLiteral("role_uuid"), role_id},
                                    {QStringLiteral("scope_kind"), QStringLiteral("project")},
                                    {QStringLiteral("scope_uuid"), project_a->project_uuid.toString(QUuid::WithoutBraces)}};
    const Reply project_grant = send(manager, base.resolved(QUrl(
        QStringLiteral("admin/principals/user/%1/assignments").arg(user_id))),
        admin_token, "POST", project_scope);
    REQUIRE(project_grant.status == 201);
    REQUIRE(db.hasPermission(engineer, QStringLiteral("field.export"),
        aowis::db::AccessScope::project(project_a->project_uuid)));
    REQUIRE(!db.hasPermission(engineer, QStringLiteral("field.export"),
        aowis::db::AccessScope::project(project_b->project_uuid)));
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("admin/role-assignments/%1")
        .arg(project_grant.json().value(QStringLiteral("uuid")).toString()))),
        admin_token, "DELETE", {}, false).status == 200);
    REQUIRE(send(manager, assignments_url, admin_token, "POST", scope).status == 201);

    const QUrl disable = base.resolved(QUrl(QStringLiteral("admin/users/%1/disable").arg(user_id)));
    const QUrl enable = base.resolved(QUrl(QStringLiteral("admin/users/%1/enable").arg(user_id)));
    REQUIRE(send(manager, disable, guest_token, "POST", {}, false).status == 403);
    REQUIRE(send(manager, disable, admin_token, "POST", {}, false).status == 200);
    REQUIRE(!db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));
    REQUIRE(send(manager, disable, admin_token, "POST", {}, false).status == 409);
    REQUIRE(send(manager, users_url, engineer_session->bearer_token, "GET", {}, false).status == 401);
    REQUIRE(send(manager, enable, admin_token, "POST", {}, false).status == 200);
    REQUIRE(send(manager, users_url, engineer_session->bearer_token, "GET", {}, false).status == 401);
    REQUIRE(db.hasPermission(engineer, QStringLiteral("field.export"), aowis::db::AccessScope::system()));

    // Disabled accounts have their sessions permanently revoked on reactivation.
    REQUIRE(send(manager, users_url, guest_token, "POST", new_user).status == 403);
    const aowis::db::SecurityAuditReport report = db.verifySecurityAudit(root);
    REQUIRE(report.valid);
    REQUIRE(db.securityAudit(root, 0, 200, &error).size() >= 18);
    REQUIRE(send(manager, base.resolved(QUrl(group_members_path + QStringLiteral("?include_inactive=true"))),
        admin_token, "GET", {}, false).json().value(QStringLiteral("memberships")).toArray().size() == 2);
    return 0;
}
