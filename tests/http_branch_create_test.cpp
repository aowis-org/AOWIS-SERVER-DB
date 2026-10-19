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

using aowis::db::AccessScope;
using aowis::db::Database;
using aowis::db::OperationContext;
using aowis::db::PrincipalKind;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

namespace
{
struct Reply
{
    int status = 0;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

Reply send(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &method,
           const QByteArray &payload = {}, const QByteArray &token = QByteArrayLiteral("Bearer admin-test"))
{
    QNetworkRequest request(url);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", token);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = method == "GET" ? manager.get(request) : manager.post(request, payload);
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
    reply->deleteLater();
    return result;
}

QByteArray jsonBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString uuidText(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database database;
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("http-branch-create.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = database.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> engineer = database.createUser(root, QStringLiteral("engineer"), &error);
    const std::optional<aowis::db::UserInfo> outsider = database.createUser(root, QStringLiteral("outsider"), &error);
    REQUIRE(engineer && outsider);
    const OperationContext worker{engineer->uuid};

    const std::optional<aowis::db::ProjectCreation> project = database.createProject(
        root, QStringLiteral("Primary water network"), &error);
    const std::optional<aowis::db::ProjectCreation> other = database.createProject(
        root, QStringLiteral("Another project"), &error);
    REQUIRE(project && other);
    const QUuid object_uuid = createUuidV7();
    REQUIRE(database.putObject(root, project->main_branch_uuid, object_uuid,
                               QStringLiteral("test.payload"), QByteArrayLiteral("base model"), -1, &error));
    const std::optional<aowis::db::Finalization> base_revision = database.finalize(
        root, project->main_branch_uuid, 1, QStringLiteral("Design baseline"), {}, &error);
    REQUIRE(base_revision);
    // An independent private source branch must not be clonable without read rights.
    const std::optional<aowis::db::BranchInfo> secret = database.createBranch(
        root, project->project_uuid, QStringLiteral("private-alternative"),
        base_revision->finalized_revision_uuid, &error);
    REQUIRE(secret);
    const std::optional<aowis::db::Finalization> secret_revision = database.finalize(
        root, secret->uuid, 0, QStringLiteral("Private design"), {}, &error);
    REQUIRE(secret_revision);
    REQUIRE(database.putObject(root, other->main_branch_uuid, createUuidV7(),
                               QStringLiteral("test.payload"), QByteArrayLiteral("different project"), -1, &error));
    const std::optional<aowis::db::Finalization> other_revision = database.finalize(
        root, other->main_branch_uuid, 1, QStringLiteral("Other baseline"), {}, &error);
    REQUIRE(other_revision);

    const std::optional<aowis::db::RoleInfo> role = database.createRole(
        root, QStringLiteral("test.branch.reader"), &error);
    const std::optional<aowis::db::PermissionInfo> read_permission = database.permission(
        root, QStringLiteral("project.read"), &error);
    const std::optional<aowis::db::PermissionInfo> create_permission = database.permission(
        root, QStringLiteral("project.branch.create"), &error);
    REQUIRE(role && read_permission && create_permission);
    REQUIRE(database.grantRolePermission(root, role->uuid, read_permission->uuid, &error));
    // Read only main: other branches are not implicitly readable.
    REQUIRE(database.assignRole(root, PrincipalKind::User, engineer->uuid,
        role->uuid, AccessScope::branch(project->main_branch_uuid), &error));
    REQUIRE(database.hasPermission(worker, QStringLiteral("project.read"),
                                   AccessScope::branch(project->main_branch_uuid)));
    REQUIRE(!database.hasPermission(worker, QStringLiteral("project.read"),
                                    AccessScope::branch(secret->uuid)));

    QHttpServer http;
    aowis::db::HttpRoutes routes(database, [admin, engineer, outsider](const QHttpServerRequest &request)
                                -> std::optional<OperationContext> {
        const QByteArray authorization = request.value(QByteArrayLiteral("authorization"));
        if (authorization == QByteArrayLiteral("Bearer admin-test"))
            return OperationContext{admin->uuid};
        if (authorization == QByteArrayLiteral("Bearer engineer-test"))
            return OperationContext{engineer->uuid};
        if (authorization == QByteArrayLiteral("Bearer outsider-test"))
            return OperationContext{outsider->uuid};
        return std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer listener;
    REQUIRE(listener.listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(&listener));
    QNetworkAccessManager manager;
    const QUrl base(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener.serverPort()));
    const QUrl url = base.resolved(QUrl(QStringLiteral("projects/%1/branches").arg(uuidText(project->project_uuid))));
    const QByteArray engineer_token("Bearer engineer-test");
    const QByteArray outsider_token("Bearer outsider-test");
    const QJsonObject valid{{QStringLiteral("name"), QStringLiteral("new-design")},
                            {QStringLiteral("base_finalized_revision_uuid"), uuidText(base_revision->finalized_revision_uuid)}};
    const QByteArray data = jsonBytes(valid);

    REQUIRE(send(manager, url, "POST", data, {}).status == 401);
    REQUIRE(send(manager, url, "POST", data, outsider_token).status == 403);
    REQUIRE(send(manager, url, "POST", data, engineer_token).status == 403);
    // Project-level create rights and main-branch read rights are independent.
    // The actor must not be able to copy a private sibling's snapshot.
    const std::optional<aowis::db::RoleInfo> create_role = database.createRole(
        root, QStringLiteral("test.branch.create-only"), &error);
    REQUIRE(create_role);
    REQUIRE(database.grantRolePermission(root, create_role->uuid, create_permission->uuid, &error));
    REQUIRE(database.assignRole(root, PrincipalKind::User, engineer->uuid, create_role->uuid,
                                AccessScope::project(project->project_uuid), &error));
    REQUIRE(database.assignRole(root, PrincipalKind::User, outsider->uuid, create_role->uuid,
                                AccessScope::project(project->project_uuid), &error));
    REQUIRE(!database.createBranch(OperationContext{outsider->uuid}, project->project_uuid,
                                   QStringLiteral("unreadable"), base_revision->finalized_revision_uuid, &error));
    REQUIRE(send(manager, url, "POST", data, outsider_token).status == 409);

    const Reply created = send(manager, url, "POST", data, engineer_token);
    REQUIRE(created.status == 201);
    const QJsonObject record = created.json();
    REQUIRE(QUuid(record.value(QStringLiteral("uuid")).toString()) != QUuid());
    REQUIRE(record.value(QStringLiteral("project_uuid")).toString() == uuidText(project->project_uuid));
    REQUIRE(record.value(QStringLiteral("name")).toString() == QStringLiteral("new-design"));
    REQUIRE(record.value(QStringLiteral("created_by_uuid")).toString() == uuidText(engineer->uuid));
    const QUuid head(record.value(QStringLiteral("head_revision_uuid")).toString());
    REQUIRE(!head.isNull());
    const std::optional<aowis::db::RevisionInfo> revision = database.revision(root, head, &error);
    REQUIRE(revision && revision->number == 1 && !revision->finalized);
    REQUIRE(revision->parent_revision_uuids == QVector<QUuid>{base_revision->finalized_revision_uuid});
    const std::optional<aowis::db::ModelObject> inherited = database.object(root, head, object_uuid, &error);
    REQUIRE(inherited && inherited->content == QByteArrayLiteral("base model"));
    REQUIRE(database.branches(root, project->project_uuid, &error).size() == 3);
    REQUIRE(database.verifyProject(root, project->project_uuid).valid);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects/%1/branches").arg(uuidText(project->project_uuid)))),
                 "GET", {}, QByteArrayLiteral("Bearer admin-test")).json().value(QStringLiteral("branches")).toArray().size() == 3);

    REQUIRE(send(manager, url, "POST", data, engineer_token).status == 409); // duplicate name
    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("not-finalized")},
        {QStringLiteral("base_finalized_revision_uuid"), uuidText(head)}}), engineer_token).status == 409);
    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("wrong-project")},
        {QStringLiteral("base_finalized_revision_uuid"), uuidText(other_revision->finalized_revision_uuid)}})).status == 409);
    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("private-copy")},
        {QStringLiteral("base_finalized_revision_uuid"), uuidText(secret_revision->finalized_revision_uuid)}}),
        engineer_token).status == 409); // private source is unreadable
    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("unknown")},
        {QStringLiteral("base_finalized_revision_uuid"), uuidText(createUuidV7())}})).status == 409);

    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("spoof")},
        {QStringLiteral("base_finalized_revision_uuid"), uuidText(base_revision->finalized_revision_uuid)},
        {QStringLiteral("created_by_uuid"), uuidText(admin->uuid)}})).status == 400);
    REQUIRE(send(manager, QUrl(url.toString() + QStringLiteral("?unused=true")), "POST", data).status == 400);
    REQUIRE(send(manager, url, "POST", QByteArrayLiteral("{")).status == 400);
    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("")},
        {QStringLiteral("base_finalized_revision_uuid"), uuidText(base_revision->finalized_revision_uuid)}})).status == 400);
    REQUIRE(send(manager, url, "POST", jsonBytes(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("bad-uuid")},
        {QStringLiteral("base_finalized_revision_uuid"), QStringLiteral("nope")}})).status == 400);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("projects/not-a-uuid/branches"))), "POST", data).status == 400);
    REQUIRE(database.branches(root, project->project_uuid, &error).size() == 3);
    REQUIRE(database.verifyProject(root, project->project_uuid).valid);
    return 0;
}
