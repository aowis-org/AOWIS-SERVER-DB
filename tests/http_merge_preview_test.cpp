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
#include <QUrlQuery>
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
struct HttpResult
{
    int status = 0;
    QByteArray body;

    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

QString id(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

HttpResult get(QNetworkAccessManager &manager, const QUrl &url,
               const QByteArray &authorization = {})
{
    QNetworkRequest request(url);
    if (!authorization.isEmpty())
        request.setRawHeader("Authorization", authorization);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!reply->isFinished())
        loop.exec();
    const HttpResult result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                            reply->readAll()};
    reply->deleteLater();
    return result;
}

QUrl requestUrl(const QUrl &base, const QString &endpoint, const QUuid &left,
                const QUuid &right)
{
    QUrl url = base.resolved(QUrl(endpoint));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("left_revision_uuid"), id(left));
    query.addQueryItem(QStringLiteral("right_revision_uuid"), id(right));
    url.setQuery(query);
    return url;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database database;
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("http-merge-preview.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = database.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> guest = database.createUser(root, QStringLiteral("guest"), &error);
    REQUIRE(guest.has_value());
    const std::optional<aowis::db::ProjectCreation> project = database.createProject(
        root, QStringLiteral("HTTP object comparison"), &error);
    REQUIRE(project.has_value());
    const QUuid shared = createUuidV7();
    const QUuid left_only = createUuidV7();
    const QUuid right_only = createUuidV7();
    const QString kind = QStringLiteral("hydraulic.label");
    REQUIRE(database.putObject(root, project->main_branch_uuid, shared, kind,
                               QByteArrayLiteral("base"), -1, &error));
    const std::optional<aowis::db::Finalization> base_revision = database.finalize(
        root, project->main_branch_uuid, 1, QStringLiteral("Base"), {}, &error);
    REQUIRE(base_revision.has_value());
    const std::optional<aowis::db::BranchInfo> left = database.createBranch(
        root, project->project_uuid, QStringLiteral("left"), base_revision->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = database.createBranch(
        root, project->project_uuid, QStringLiteral("right"), base_revision->finalized_revision_uuid, &error);
    REQUIRE(left && right);
    REQUIRE(database.putObject(root, left->uuid, shared, kind, QByteArrayLiteral("L"), 1, &error));
    REQUIRE(database.putObject(root, right->uuid, shared, kind, QByteArrayLiteral("R"), 1, &error));
    REQUIRE(database.putObject(root, left->uuid, left_only, kind, QByteArrayLiteral("L"), -1, &error));
    REQUIRE(database.putObject(root, right->uuid, right_only, kind, QByteArrayLiteral("R"), -1, &error));
    const qint64 left_generation = database.revision(root, left->head_revision_uuid, &error)->generation;
    const qint64 right_generation = database.revision(root, right->head_revision_uuid, &error)->generation;

    QHttpServer http;
    aowis::db::HttpRoutes routes(database, [admin, guest](const QHttpServerRequest &request)
            -> std::optional<OperationContext> {
        const QByteArray authorization = request.value(QByteArrayLiteral("authorization"));
        if (authorization == QByteArrayLiteral("Bearer admin-test"))
            return OperationContext{admin->uuid};
        if (authorization == QByteArrayLiteral("Bearer guest-test"))
            return OperationContext{guest->uuid};
        return std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer listener;
    REQUIRE(listener.listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(&listener));
    QNetworkAccessManager manager;
    const QUrl root_url(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener.serverPort()));
    const QUrl comparison = requestUrl(root_url, QStringLiteral("merge/compare"),
                                       left->head_revision_uuid, right->head_revision_uuid);
    const QUrl preview = requestUrl(root_url, QStringLiteral("merge/preview"),
                                    left->head_revision_uuid, right->head_revision_uuid);
    const QByteArray admin_token("Bearer admin-test");
    const QByteArray guest_token("Bearer guest-test");

    REQUIRE(get(manager, comparison).status == 401);
    REQUIRE(get(manager, preview).status == 401);
    REQUIRE(get(manager, comparison, guest_token).status == 404);
    REQUIRE(get(manager, preview, guest_token).status == 404);
    REQUIRE(get(manager, comparison, admin_token).status == 200);
    const QJsonObject comparison_json = get(manager, comparison, admin_token).json();
    REQUIRE(comparison_json.value(QStringLiteral("project_uuid")).toString() == id(project->project_uuid));
    REQUIRE(!comparison_json.value(QStringLiteral("left_is_ancestor_of_right")).toBool());
    REQUIRE(!comparison_json.value(QStringLiteral("right_is_ancestor_of_left")).toBool());
    REQUIRE(comparison_json.value(QStringLiteral("merge_base_revision_uuids")).toArray() ==
        QJsonArray{id(base_revision->finalized_revision_uuid)});

    const HttpResult response = get(manager, preview, admin_token);
    REQUIRE(response.status == 200);
    const QJsonObject preview_json = response.json();
    REQUIRE(preview_json.value(QStringLiteral("base_revision_uuid")).toString() ==
            id(base_revision->finalized_revision_uuid));
    REQUIRE(preview_json.value(QStringLiteral("left_generation")).toString() == QString::number(left_generation));
    REQUIRE(preview_json.value(QStringLiteral("right_generation")).toString() == QString::number(right_generation));
    REQUIRE(preview_json.value(QStringLiteral("conflict_count")).toString() == QStringLiteral("1"));
    REQUIRE(preview_json.value(QStringLiteral("changes")).toArray().size() == 3);
    REQUIRE(!response.body.contains("base64")); // summary only, no object payloads
    bool saw_conflict = false;
    bool saw_left = false;
    bool saw_right = false;
    for (const QJsonValue &value : preview_json.value(QStringLiteral("changes")).toArray())
    {
        const QJsonObject change = value.toObject();
        if (change.value(QStringLiteral("object_uuid")).toString() == id(shared))
        {
            REQUIRE(change.value(QStringLiteral("classification")).toString() == QStringLiteral("conflict"));
            REQUIRE(change.value(QStringLiteral("base_present")).toBool());
            REQUIRE(change.value(QStringLiteral("left_present")).toBool());
            REQUIRE(change.value(QStringLiteral("right_present")).toBool());
            saw_conflict = true;
        }
        if (change.value(QStringLiteral("object_uuid")).toString() == id(left_only))
        {
            REQUIRE(change.value(QStringLiteral("classification")).toString() == QStringLiteral("left_only"));
            saw_left = true;
        }
        if (change.value(QStringLiteral("object_uuid")).toString() == id(right_only))
        {
            REQUIRE(change.value(QStringLiteral("classification")).toString() == QStringLiteral("right_only"));
            saw_right = true;
        }
    }
    REQUIRE(saw_conflict && saw_left && saw_right);

    // Invalid, duplicate and unknown parameters may never be silently ignored.
    REQUIRE(get(manager, root_url.resolved(QUrl(QStringLiteral("merge/compare"))), admin_token).status == 400);
    REQUIRE(get(manager, root_url.resolved(QUrl(QStringLiteral("merge/preview?left_revision_uuid=garbage&right_revision_uuid=garbage"))),
                admin_token).status == 400);
    QUrl duplicate = comparison;
    duplicate.setQuery(duplicate.query() + QStringLiteral("&left_revision_uuid=") + id(left->head_revision_uuid));
    REQUIRE(get(manager, duplicate, admin_token).status == 400);
    QUrl unexpected = preview;
    unexpected.setQuery(unexpected.query() + QStringLiteral("&verbose=true"));
    REQUIRE(get(manager, unexpected, admin_token).status == 400);
    REQUIRE(get(manager, requestUrl(root_url, QStringLiteral("merge/compare"),
            left->head_revision_uuid, createUuidV7()), admin_token).status == 404);
    REQUIRE(get(manager, requestUrl(root_url, QStringLiteral("merge/preview"),
            left->head_revision_uuid, createUuidV7()), admin_token).status == 404);

    // Cross-branch graph comparison is permitted with tip read rights, but a
    // three-way object diff additionally requires read access to the base.
    const std::optional<aowis::db::RoleInfo> read_role = database.createRole(root, QStringLiteral("merge.read"), &error);
    REQUIRE(read_role);
    const std::optional<aowis::db::PermissionInfo> permission = database.permission(root,
        QStringLiteral("project.read"), &error);
    REQUIRE(permission);
    REQUIRE(database.grantRolePermission(root, read_role->uuid, permission->uuid, &error));
    REQUIRE(database.assignRole(root, PrincipalKind::User, guest->uuid, read_role->uuid,
                                AccessScope::branch(left->uuid), &error));
    REQUIRE(database.assignRole(root, PrincipalKind::User, guest->uuid, read_role->uuid,
                                AccessScope::branch(right->uuid), &error));
    REQUIRE(get(manager, comparison, guest_token).status == 200);
    REQUIRE(get(manager, preview, guest_token).status == 404);
    REQUIRE(database.assignRole(root, PrincipalKind::User, guest->uuid, read_role->uuid,
                                AccessScope::branch(project->main_branch_uuid), &error));
    REQUIRE(get(manager, preview, guest_token).status == 200);

    // GET has no mutation side effects on either editable working head.
    REQUIRE(database.revision(root, left->head_revision_uuid, &error)->generation == left_generation);
    REQUIRE(database.revision(root, right->head_revision_uuid, &error)->generation == right_generation);
    REQUIRE(database.verifyProject(root, project->project_uuid).valid);

    // 1,025 new objects exceed the HTTP change cap without needing a large
    // serialized JSON reply. The preview must reject the whole result, not
    // truncate it or inadvertently modify either revision.
    for (int index = 0; index < 1025; ++index)
    {
        REQUIRE(database.putObject(root, left->uuid, createUuidV7(), kind,
                                   QByteArrayLiteral("added"), -1, &error));
    }
    const qint64 limited_generation = database.revision(
        root, left->head_revision_uuid, &error)->generation;
    const HttpResult oversized = get(manager, preview, admin_token);
    REQUIRE(oversized.status == 409);
    REQUIRE(oversized.json().value(QStringLiteral("error")).toString() ==
            QStringLiteral("Merge preview exceeds 1024 changed entities"));
    REQUIRE(get(manager, comparison, admin_token).status == 200);
    REQUIRE(database.revision(root, left->head_revision_uuid, &error)->generation == limited_generation);
    REQUIRE(database.verifyProject(root, project->project_uuid).valid);
    return 0;
}
