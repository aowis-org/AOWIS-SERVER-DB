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
using aowis::db::PrincipalKind;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct Reply
{
    int status = 0;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

Reply get(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &token = {})
{
    QNetworkRequest request(url);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", token);
    QNetworkReply *network = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(network, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!network->isFinished())
        loop.exec();
    Reply result{network->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), network->readAll()};
    network->deleteLater();
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database db;
    REQUIRE(db.openSqlite(directory.filePath(QStringLiteral("catalog-http.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> guest = db.createUser(root, QStringLiteral("guest"), &error);
    const std::optional<aowis::db::GroupInfo> group = db.createGroup(root, QStringLiteral("field"), &error);
    const std::optional<aowis::db::RoleInfo> role = db.createRole(root, QStringLiteral("field.viewer"), &error);
    const std::optional<aowis::db::PermissionInfo> permission = db.permission(root, QStringLiteral("project.read"), &error);
    REQUIRE(guest && group && role && permission);
    const std::optional<QUuid> member = db.addGroupMember(root, group->uuid, guest->uuid, &error);
    const std::optional<QUuid> grant = db.grantRolePermission(root, role->uuid, permission->uuid, &error);
    const std::optional<QUuid> assignment = db.assignRole(root, PrincipalKind::Group,
        group->uuid, role->uuid, aowis::db::AccessScope::system(), &error);
    REQUIRE(member && grant && assignment);

    QHttpServer http;
    aowis::db::HttpRoutes routes(db, [admin, guest](const QHttpServerRequest &request)
                                 -> std::optional<OperationContext> {
        const QByteArray token = request.value(QByteArrayLiteral("authorization"));
        if (token == QByteArrayLiteral("Bearer admin-token"))
            return OperationContext{admin->uuid};
        if (token == QByteArrayLiteral("Bearer guest-token"))
            return OperationContext{guest->uuid};
        return std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer *listener = new QTcpServer(&http);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(listener));
    QNetworkAccessManager manager;
    const QUrl base(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener->serverPort()));
    const QByteArray root_token("Bearer admin-token");
    const QByteArray guest_token("Bearer guest-token");

    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/users")))).status == 401);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/users"))), guest_token).status == 403);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/groups"))), guest_token).status == 403);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/users?limit=0"))), root_token).status == 400);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/users?limit=1&limit=2"))), root_token).status == 400);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/users?unknown=1"))), root_token).status == 400);

    const Reply users = get(manager, base.resolved(QUrl(QStringLiteral("admin/users"))), root_token);
    REQUIRE(users.status == 200 && users.json().value(QStringLiteral("users")).toArray().size() == 2);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/groups"))), root_token).json()
        .value(QStringLiteral("groups")).toArray().size() == 1);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/roles"))), root_token).json()
        .value(QStringLiteral("roles")).toArray().size() >= 3);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/permissions"))), root_token).json()
        .value(QStringLiteral("permissions")).toArray().size() >= 9);

    const Reply first_page = get(manager, base.resolved(QUrl(QStringLiteral("admin/users?limit=1"))), root_token);
    REQUIRE(first_page.status == 200);
    const QJsonArray first = first_page.json().value(QStringLiteral("users")).toArray();
    REQUIRE(first.size() == 1);
    const QString after = first.at(0).toObject().value(QStringLiteral("uuid")).toString();
    const Reply next = get(manager, base.resolved(QUrl(QStringLiteral("admin/users?limit=1&after_uuid=%1").arg(after))), root_token);
    REQUIRE(next.status == 200);
    const QJsonArray second = next.json().value(QStringLiteral("users")).toArray();
    REQUIRE(second.size() == 1 && second.at(0).toObject().value(QStringLiteral("uuid")).toString() != after);

    const QString group_id = group->uuid.toString(QUuid::WithoutBraces);
    const QString role_id = role->uuid.toString(QUuid::WithoutBraces);
    const QString group_members = QStringLiteral("admin/groups/%1/members").arg(group_id);
    const QString role_grants = QStringLiteral("admin/roles/%1/permissions").arg(role_id);
    const QString assignments = QStringLiteral("admin/principals/group/%1/assignments").arg(group_id);
    REQUIRE(get(manager, base.resolved(QUrl(group_members)), guest_token).status == 403);
    REQUIRE(get(manager, base.resolved(QUrl(group_members)), root_token).json()
        .value(QStringLiteral("memberships")).toArray().size() == 1);
    REQUIRE(get(manager, base.resolved(QUrl(role_grants)), root_token).json()
        .value(QStringLiteral("grants")).toArray().size() == 1);
    REQUIRE(get(manager, base.resolved(QUrl(assignments)), root_token).json()
        .value(QStringLiteral("assignments")).toArray().size() == 1);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/principals/invalid/%1/assignments").arg(group_id))), root_token).status == 400);
    REQUIRE(get(manager, base.resolved(QUrl(QStringLiteral("admin/groups/invalid/members"))), root_token).status == 400);

    REQUIRE(db.removeGroupMember(root, *member, &error));
    REQUIRE(db.revokeRolePermission(root, *grant, &error));
    REQUIRE(db.revokeRoleAssignment(root, *assignment, &error));
    REQUIRE(get(manager, base.resolved(QUrl(group_members)), root_token).json()
        .value(QStringLiteral("memberships")).toArray().isEmpty());
    const Reply history = get(manager, base.resolved(QUrl(group_members + QStringLiteral("?include_inactive=true"))), root_token);
    REQUIRE(history.status == 200);
    const QJsonArray old_members = history.json().value(QStringLiteral("memberships")).toArray();
    REQUIRE(old_members.size() == 1 && old_members.at(0).toObject().value(QStringLiteral("revoked")).toBool());
    const QJsonArray old_grants = get(manager, base.resolved(QUrl(role_grants + QStringLiteral("?include_inactive=true"))), root_token).json()
        .value(QStringLiteral("grants")).toArray();
    REQUIRE(old_grants.size() == 1 && old_grants.at(0).toObject().value(QStringLiteral("revoked")).toBool());
    REQUIRE(get(manager, base.resolved(QUrl(assignments + QStringLiteral("?include_inactive=true"))), root_token).json()
        .value(QStringLiteral("assignments")).toArray().size() == 1);
    return 0;
}
