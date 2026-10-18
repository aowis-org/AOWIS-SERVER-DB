#include <aowis/db/database.h>
#include <aowis/db/http_routes.h>
#include <aowis/db/model_codec.h>
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
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QDebug>

using aowis::db::Database;
using aowis::db::ModelCodec;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

namespace
{
QString id(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

struct HttpResult
{
    int status = 0;
    QJsonObject json;
};

HttpResult post(QNetworkAccessManager &manager, const QUrl &url, const QJsonObject &body,
                const QByteArray &authorization = QByteArrayLiteral("Bearer admin-test"))
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!authorization.isEmpty())
        request.setRawHeader("Authorization", authorization);
    QNetworkReply *reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    if (!reply->isFinished())
        loop.exec();
    const HttpResult result{
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        QJsonDocument::fromJson(reply->readAll()).object()};
    reply->deleteLater();
    return result;
}

QJsonObject fieldChoice(const QUuid &uuid, const QString &name, const QString &side)
{
    return {{QStringLiteral("object_uuid"), id(uuid)},
            {QStringLiteral("field_resolutions"), QJsonArray{QJsonObject{
                {QStringLiteral("field_name"), name}, {QStringLiteral("side"), side}}}}};
}

QJsonObject entityChoice(const QUuid &uuid, const QString &side)
{
    return {{QStringLiteral("object_uuid"), id(uuid)}, {QStringLiteral("side"), side}};
}

HydraulicNodeJunction junction(const QUuid &uuid, const QString &label, double elevation)
{
    HydraulicNodeJunction node;
    node.uuid = uuid;
    node.id = label;
    node.elevation_m = elevation;
    return node;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("http-merge-commit.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> guest = db.createUser(root, QStringLiteral("guest"), &error);
    REQUIRE(guest);
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        root, QStringLiteral("HTTP merge commit"), &error);
    REQUIRE(project);
    const QUuid typed = createUuidV7();
    const QUuid opaque = createUuidV7();
    const QUuid remote_only = createUuidV7();
    const QUuid local_only = createUuidV7();
    const QString typed_kind = QStringLiteral("hydraulic.junction");
    const QString label_kind = QStringLiteral("hydraulic.label");
    const HydraulicNodeJunction common = junction(typed, QStringLiteral("J1"), 5.0);
    REQUIRE(db.putObject(root, project->main_branch_uuid, typed, typed_kind,
                         ModelCodec<HydraulicNodeJunction>::encode(common), -1, &error));
    REQUIRE(db.putObject(root, project->main_branch_uuid, opaque, label_kind,
                         QByteArrayLiteral("base"), -1, &error));
    const std::optional<aowis::db::Finalization> base = db.finalize(
        root, project->main_branch_uuid, 2, QStringLiteral("base"), {}, &error);
    REQUIRE(base);
    const std::optional<aowis::db::BranchInfo> left = db.createBranch(
        root, project->project_uuid, QStringLiteral("left"), base->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(
        root, project->project_uuid, QStringLiteral("right"), base->finalized_revision_uuid, &error);
    REQUIRE(left && right);
    HydraulicNodeJunction local = common;
    local.elevation_m = 10.0;
    local.metadata.description = QStringLiteral("local description");
    HydraulicNodeJunction remote = common;
    remote.elevation_m = 12.0;
    remote.metadata.comment = QStringLiteral("remote comment");
    REQUIRE(db.putObject(root, left->uuid, typed, typed_kind,
                         ModelCodec<HydraulicNodeJunction>::encode(local), 1, &error));
    REQUIRE(db.putObject(root, right->uuid, typed, typed_kind,
                         ModelCodec<HydraulicNodeJunction>::encode(remote), 1, &error));
    REQUIRE(db.putObject(root, left->uuid, opaque, label_kind, QByteArrayLiteral("left"), 1, &error));
    REQUIRE(db.putObject(root, right->uuid, opaque, label_kind, QByteArrayLiteral("right"), 1, &error));
    REQUIRE(db.putObject(root, right->uuid, remote_only, label_kind, QByteArrayLiteral("remote only"), -1, &error));
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        root, left->uuid, 2, QStringLiteral("left"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        root, right->uuid, 3, QStringLiteral("right"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid head = left_final->new_working_revision_uuid;
    REQUIRE(db.putObject(root, left->uuid, local_only, label_kind,
                         QByteArrayLiteral("keep local"), -1, &error));
    REQUIRE(db.revision(root, head, &error)->generation == 1);
    REQUIRE(db.objectGroupsForRevision(root, head, &error).size() == 1);

    QHttpServer http;
    aowis::db::HttpRoutes routes(db, [admin, guest](const QHttpServerRequest &request)
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
    const QUrl base_url(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener.serverPort()));
    const QUrl endpoint = base_url.resolved(QUrl(QStringLiteral("branches/") + id(left->uuid) +
                                                    QStringLiteral("/merge")));
    const QJsonArray choices{fieldChoice(typed, QStringLiteral("elevation_m"), QStringLiteral("right")),
                             entityChoice(opaque, QStringLiteral("left"))};
    const QJsonObject valid{
        {QStringLiteral("expected_working_revision_uuid"), id(head)},
        {QStringLiteral("expected_generation"), QStringLiteral("1")},
        {QStringLiteral("right_finalized_revision_uuid"), id(right_final->finalized_revision_uuid)},
        {QStringLiteral("resolutions"), choices},
        {QStringLiteral("title"), QStringLiteral("REST merged")},
        {QStringLiteral("message"), QStringLiteral("preserve journal and merge fields")}};

    REQUIRE(post(manager, endpoint, valid, {}).status == 401);
    REQUIRE(post(manager, endpoint, valid, QByteArrayLiteral("Bearer guest-test")).status == 409);
    REQUIRE(post(manager, QUrl(endpoint.toString() + QStringLiteral("?debug=true")), valid).status == 400);
    REQUIRE(post(manager, endpoint, QJsonObject{}).status == 400);
    QJsonObject malformed = valid;
    malformed.insert(QStringLiteral("unknown"), true);
    REQUIRE(post(manager, endpoint, malformed).status == 400);
    malformed = valid;
    malformed.insert(QStringLiteral("expected_generation"), 1); // never a lossy JSON number
    REQUIRE(post(manager, endpoint, malformed).status == 400);
    malformed = valid;
    malformed.insert(QStringLiteral("expected_generation"), QStringLiteral("01"));
    REQUIRE(post(manager, endpoint, malformed).status == 400);
    malformed = valid;
    malformed.insert(QStringLiteral("expected_working_revision_uuid"), id(createUuidV7()));
    REQUIRE(post(manager, endpoint, malformed).status == 409);
    malformed = valid;
    malformed.insert(QStringLiteral("expected_generation"), QStringLiteral("0"));
    REQUIRE(post(manager, endpoint, malformed).status == 409);
    malformed = valid;
    malformed.insert(QStringLiteral("resolutions"), QJsonArray{});
    REQUIRE(post(manager, endpoint, malformed).status == 409);
    malformed = valid;
    malformed.insert(QStringLiteral("resolutions"), QJsonArray{choices.at(0), choices.at(0), choices.at(1)});
    REQUIRE(post(manager, endpoint, malformed).status == 400);
    malformed = valid;
    malformed.insert(QStringLiteral("resolutions"), QJsonArray{fieldChoice(typed, QStringLiteral("id"),
                                      QStringLiteral("right")), choices.at(1)});
    REQUIRE(post(manager, endpoint, malformed).status == 409);
    malformed = valid;
    malformed.insert(QStringLiteral("resolutions"), QJsonArray{fieldChoice(typed, QStringLiteral("elevation_m"),
                                      QStringLiteral("invalid")), choices.at(1)});
    REQUIRE(post(manager, endpoint, malformed).status == 400);
    malformed = valid;
    malformed.insert(QStringLiteral("resolutions"), QJsonArray{QJsonObject{
        {QStringLiteral("object_uuid"), id(opaque)}, {QStringLiteral("side"), QStringLiteral("left")},
        {QStringLiteral("field_resolutions"), QJsonArray{}}}, choices.at(0)});
    REQUIRE(post(manager, endpoint, malformed).status == 400);
    malformed = valid;
    malformed.insert(QStringLiteral("resolutions"), QJsonArray{QJsonObject{
        {QStringLiteral("object_uuid"), id(typed)}, {QStringLiteral("field_resolutions"), QJsonArray{
            QJsonObject{{QStringLiteral("field_name"), QStringLiteral("elevation_m")},
                        {QStringLiteral("side"), QStringLiteral("left")}},
            QJsonObject{{QStringLiteral("field_name"), QStringLiteral("elevation_m")},
                        {QStringLiteral("side"), QStringLiteral("right")}}}}}, choices.at(1)});
    REQUIRE(post(manager, endpoint, malformed).status == 400);

    // A late failure must roll back merged fields, created objects, new journal
    // groups, and parent insertion without discarding existing local work.
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("http-merge-inject"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_rest_merge BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'injected failure'); END")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("http-merge-inject"));
    REQUIRE(post(manager, endpoint, valid).status == 409);
    REQUIRE(db.revision(root, head, &error)->generation == 1);
    REQUIRE(db.revision(root, head, &error)->parent_revision_uuids.size() == 1);
    REQUIRE(db.object(root, head, typed, &error)->content == ModelCodec<HydraulicNodeJunction>::encode(local));
    REQUIRE(db.object(root, head, local_only, &error)->content == QByteArrayLiteral("keep local"));
    REQUIRE(!db.object(root, head, remote_only, &error));
    REQUIRE(db.objectGroupsForRevision(root, head, &error).size() == 1);
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("http-merge-inject"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_rest_merge")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("http-merge-inject"));

    const HttpResult result = post(manager, endpoint, valid);
    REQUIRE(result.status == 201);
    REQUIRE(result.json.value(QStringLiteral("finalized_revision_uuid")).toString() == id(head));
    REQUIRE(result.json.value(QStringLiteral("new_working_revision_uuid")).toString() != id(head));
    REQUIRE(result.json.value(QStringLiteral("snapshot_sha256")).toString().size() == 64);
    REQUIRE(result.json.value(QStringLiteral("chain_sha256")).toString().size() == 64);
    REQUIRE(post(manager, endpoint, valid).status == 409); // no replay after head advanced
    const std::optional<aowis::db::RevisionInfo> finalized = db.revision(root, head, &error);
    REQUIRE(finalized && finalized->finalized && finalized->parent_revision_uuids.size() == 2);
    REQUIRE(finalized->parent_revision_uuids.at(0) == left_final->finalized_revision_uuid);
    REQUIRE(finalized->parent_revision_uuids.at(1) == right_final->finalized_revision_uuid);
    const std::optional<aowis::db::ModelObject> merged_object = db.object(root, head, typed, &error);
    REQUIRE(merged_object);
    const std::optional<HydraulicNodeJunction> merged = ModelCodec<HydraulicNodeJunction>::decode(
        merged_object->content, &error);
    REQUIRE(merged);
    REQUIRE(merged->elevation_m == 12.0);
    REQUIRE(merged->metadata.description == QStringLiteral("local description"));
    REQUIRE(merged->metadata.comment == QStringLiteral("remote comment"));
    REQUIRE(db.object(root, head, opaque, &error)->content == QByteArrayLiteral("left"));
    REQUIRE(db.object(root, head, remote_only, &error)->content == QByteArrayLiteral("remote only"));
    REQUIRE(db.object(root, head, local_only, &error)->content == QByteArrayLiteral("keep local"));
    REQUIRE(db.objectGroupsForRevision(root, head, &error).size() == 2);
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    return 0;
}
