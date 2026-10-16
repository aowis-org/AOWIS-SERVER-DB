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
    QNetworkReply::NetworkError network_error = QNetworkReply::NoError;
    QString error_message;

    QJsonObject json() const { return QJsonDocument::fromJson(this->body).object(); }
};

HttpResult send(QNetworkAccessManager &manager, const QUrl &url, const QByteArray &method,
                const QJsonObject &json = {}, const QByteArray &token = QByteArrayLiteral("Bearer root-token"))
{
    QNetworkRequest request(url);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", token);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Compact);
    // Use Qt's dedicated HTTP operations for standard verbs. In particular,
    // PUT must take the same transport path as real network clients.
    QNetworkReply *reply = nullptr;
    if (method == "GET")
        reply = manager.get(request);
    else if (method == "PUT")
        reply = manager.put(request, data);
    else if (method == "POST")
        reply = manager.post(request, data);
    else
        reply = manager.sendCustomRequest(request, method, data);
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
    const HttpResult result{
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        reply->readAll(),
        reply->error(),
        reply->errorString()
    };
    reply->deleteLater();
    return result;
}

QString id(const QUuid &uuid) { return uuid.toString(QUuid::WithoutBraces); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database database;
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("http_mutations.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> root = database.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(root.has_value());
    const std::optional<aowis::db::UserInfo> guest = database.createUser({root->uuid}, QStringLiteral("guest"), &error);
    REQUIRE(guest.has_value());

    QHttpServer http;
    aowis::db::HttpRoutes routes(database, [root, guest](const QHttpServerRequest &request)
                                -> std::optional<aowis::db::OperationContext> {
        const QByteArray token = request.value(QByteArrayLiteral("authorization"));
        if (token == QByteArrayLiteral("Bearer root-token"))
            return aowis::db::OperationContext{root->uuid};
        if (token == QByteArrayLiteral("Bearer guest-token"))
            return aowis::db::OperationContext{guest->uuid};
        return std::nullopt;
    });
    REQUIRE(routes.registerOn(http));
    QTcpServer *listener = new QTcpServer(&http);
    REQUIRE(listener->listen(QHostAddress::LocalHost));
    REQUIRE(http.bind(listener));
    QNetworkAccessManager manager;
    const QUrl base(QStringLiteral("http://127.0.0.1:%1/api/v1/").arg(listener->serverPort()));
    const QByteArray guest_token("Bearer guest-token");

    const HttpResult created = send(manager, base.resolved(QUrl(QStringLiteral("projects"))), "POST",
                                    {{QStringLiteral("name"), QStringLiteral("Hydraulic test")}});
    REQUIRE(created.status == 201);
    const QString branch_id = created.json().value(QStringLiteral("main_branch_uuid")).toString();
    const QString revision_id = created.json().value(QStringLiteral("working_revision_uuid")).toString();
    const QString project_id = created.json().value(QStringLiteral("project_uuid")).toString();
    REQUIRE(!branch_id.isEmpty() && !revision_id.isEmpty() && !project_id.isEmpty());

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    network.id = QStringLiteral("network");
    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    junction.id = QStringLiteral("J1");
    junction.elevation_m = 123.4567890123;
    network.nodes_junctions.append(junction);
    const QByteArray bytes = aowis::db::ModelCodec<NetworkHydraulic>::encode(network);
    REQUIRE(!bytes.isEmpty());
    const QString net_path = QStringLiteral("branches/") + branch_id + QStringLiteral("/networks/") + id(network.uuid);
    const QString net_get_path = QStringLiteral("revisions/") + revision_id + QStringLiteral("/networks/") + id(network.uuid);
    const QJsonObject save_body{{QStringLiteral("codec"), QStringLiteral("aowis.model.network_hydraulic.v1")},
                                {QStringLiteral("expected_generation"), QStringLiteral("0")},
                                {QStringLiteral("content_base64"), QString::fromLatin1(bytes.toBase64())}};
    // Keep the anonymous connection isolated from the authenticated one, so
    // the test cannot accidentally depend on a client's credential cache.
    QNetworkAccessManager anonymous_manager;
    const HttpResult anonymous_get = send(anonymous_manager, base.resolved(QUrl(net_get_path)),
                                          "GET", {}, {});
    if (anonymous_get.status != 401)
        qCritical().noquote() << "Anonymous GET unexpected response:"
            << anonymous_get.status << static_cast<int>(anonymous_get.network_error)
            << anonymous_get.error_message << anonymous_get.body;
    REQUIRE(anonymous_get.status == 401);
    const HttpResult anonymous_put = send(anonymous_manager, base.resolved(QUrl(net_path)),
                                          "PUT", save_body, {});
    if (anonymous_put.status != 401)
        qCritical().noquote() << "Anonymous PUT unexpected response:"
            << anonymous_put.status << static_cast<int>(anonymous_put.network_error)
            << anonymous_put.error_message << anonymous_put.body;
    REQUIRE(anonymous_put.status == 401);
    // Also verify that sending an explicit anonymous request using the SAME
    // client that previously sent an authenticated request never inherits its
    // Authorization header or access rights.
    const HttpResult shared_client_anonymous_put = send(manager, base.resolved(QUrl(net_path)),
                                                        "PUT", save_body, {});
    if (shared_client_anonymous_put.status != 401)
        qCritical().noquote() << "Anonymous PUT on reused client unexpected response:"
            << shared_client_anonymous_put.status
            << static_cast<int>(shared_client_anonymous_put.network_error)
            << shared_client_anonymous_put.error_message << shared_client_anonymous_put.body;
    REQUIRE(shared_client_anonymous_put.status == 401);
    REQUIRE(send(manager, base.resolved(QUrl(net_path)), "PUT", save_body, guest_token).status == 409);
    REQUIRE(send(manager, base.resolved(QUrl(net_get_path)), "GET", {}, guest_token).status == 404);
    REQUIRE(send(manager, base.resolved(QUrl(net_path)), "PUT", QJsonObject{
        {QStringLiteral("codec"), QStringLiteral("bad")},
        {QStringLiteral("expected_generation"), QStringLiteral("0")},
        {QStringLiteral("content_base64"), QString::fromLatin1(bytes.toBase64())}}).status == 400);
    REQUIRE(send(manager, base.resolved(QUrl(net_path)), "PUT", QJsonObject{
        {QStringLiteral("codec"), QStringLiteral("aowis.model.network_hydraulic.v1")},
        {QStringLiteral("expected_generation"), QStringLiteral("0")},
        {QStringLiteral("content_base64"), QStringLiteral("%%%%")}}).status == 400);
    REQUIRE(send(manager, base.resolved(QUrl(QStringLiteral("branches/") + branch_id +
                                               QStringLiteral("/networks/") + id(createUuidV7()))),
                 "PUT", save_body).status == 400);
    const HttpResult saved = send(manager, base.resolved(QUrl(net_path)), "PUT", save_body);
    REQUIRE(saved.status == 200);
    REQUIRE(saved.json().value(QStringLiteral("generation")).toString() == QStringLiteral("2"));
    REQUIRE(send(manager, base.resolved(QUrl(net_path)), "PUT", save_body).status == 409);
    const HttpResult loaded = send(manager, base.resolved(QUrl(net_get_path)), "GET");
    REQUIRE(loaded.status == 200);
    REQUIRE(QByteArray::fromBase64(loaded.json().value(QStringLiteral("content_base64")).toString().toLatin1()) == bytes);

    const QString groups_path = QStringLiteral("branches/") + branch_id + QStringLiteral("/object-groups");
    const QString groups_read_path = QStringLiteral("revisions/") + revision_id + QStringLiteral("/object-groups");
    const QUuid custom = createUuidV7();
    const QString custom_path = QStringLiteral("revisions/") + revision_id + QStringLiteral("/objects/") + id(custom);
    const QJsonObject edit{{QStringLiteral("uuid"), id(custom)},
                           {QStringLiteral("kind"), QStringLiteral("test.payload")},
                           {QStringLiteral("expected_version"), QStringLiteral("-1")},
                           {QStringLiteral("content_base64"), QStringLiteral("aGVsbG8=")}};
    QJsonObject group_body{{QStringLiteral("expected_generation"), QStringLiteral("2")},
                           {QStringLiteral("edits"), QJsonArray{edit}}};
    REQUIRE(send(manager, base.resolved(QUrl(groups_path)), "POST", group_body, guest_token).status == 404);
    REQUIRE(send(manager, base.resolved(QUrl(groups_read_path)), "GET", {}, guest_token).status == 404);
    REQUIRE(send(manager, base.resolved(QUrl(groups_path)), "POST", QJsonObject{
        {QStringLiteral("expected_generation"), QStringLiteral("2")},
        {QStringLiteral("edits"), QJsonArray{edit, edit}}}).status == 400);
    REQUIRE(send(manager, base.resolved(QUrl(groups_path)), "POST", QJsonObject{
        {QStringLiteral("expected_generation"), QStringLiteral("2")},
        {QStringLiteral("edits"), QJsonArray{QJsonObject{
            {QStringLiteral("uuid"), id(createUuidV7())},
            {QStringLiteral("kind"), QStringLiteral("hydraulic.pipe")},
            {QStringLiteral("expected_version"), QStringLiteral("-1")},
            {QStringLiteral("content_base64"), QStringLiteral("aGVsbG8=")}}}}}).status == 400);
    const HttpResult first = send(manager, base.resolved(QUrl(groups_path)), "POST", group_body);
    REQUIRE(first.status == 201);
    const QString first_id = first.json().value(QStringLiteral("group_uuid")).toString();
    REQUIRE(!first_id.isEmpty());
    REQUIRE(first.json().value(QStringLiteral("generation")).toString() == QStringLiteral("3"));
    REQUIRE(send(manager, base.resolved(QUrl(custom_path)), "GET").json()
            .value(QStringLiteral("content_base64")).toString() == QStringLiteral("aGVsbG8="));

    QJsonObject second_edit = edit;
    second_edit.insert(QStringLiteral("expected_version"), QStringLiteral("1"));
    second_edit.insert(QStringLiteral("content_base64"), QStringLiteral("d29ybGQ="));
    group_body.insert(QStringLiteral("expected_generation"), QStringLiteral("3"));
    group_body.insert(QStringLiteral("edits"), QJsonArray{second_edit});
    REQUIRE(send(manager, base.resolved(QUrl(groups_path)), "POST", group_body).status == 201);
    const HttpResult failed = send(manager, base.resolved(QUrl(groups_path)), "POST", QJsonObject{
        {QStringLiteral("expected_generation"), QStringLiteral("4")},
        {QStringLiteral("edits"), QJsonArray{
            QJsonObject{{QStringLiteral("uuid"), id(custom)},
                        {QStringLiteral("kind"), QStringLiteral("test.payload")},
                        {QStringLiteral("expected_version"), QStringLiteral("2")},
                        {QStringLiteral("content_base64"), QStringLiteral("eA==")}},
            QJsonObject{{QStringLiteral("uuid"), id(createUuidV7())},
                        {QStringLiteral("kind"), QStringLiteral("test.payload")},
                        {QStringLiteral("expected_version"), QStringLiteral("3")},
                        {QStringLiteral("content_base64"), QStringLiteral("eQ==")}}}}});
    REQUIRE(failed.status == 409);
    REQUIRE(database.revision({root->uuid}, QUuid(revision_id), &error)->generation == 4);
    REQUIRE(send(manager, base.resolved(QUrl(custom_path)), "GET").json()
            .value(QStringLiteral("content_base64")).toString() == QStringLiteral("d29ybGQ="));
    const QString junction_delete = QStringLiteral("branches/") + branch_id + QStringLiteral("/object-groups");
    REQUIRE(send(manager, base.resolved(QUrl(junction_delete)), "POST", QJsonObject{
        {QStringLiteral("expected_generation"), QStringLiteral("4")},
        {QStringLiteral("edits"), QJsonArray{QJsonObject{
            {QStringLiteral("uuid"), id(junction.uuid)},
            {QStringLiteral("expected_version"), QStringLiteral("1")},
            {QStringLiteral("delete"), true}}}}}).status == 400);

    const QJsonObject compensate{{QStringLiteral("expected_generation"), QStringLiteral("4")}};
    const QString undo_path = QStringLiteral("branches/") + branch_id + QStringLiteral("/undo");
    const QString redo_path = QStringLiteral("branches/") + branch_id + QStringLiteral("/redo");
    REQUIRE(send(manager, base.resolved(QUrl(undo_path)), "POST", compensate, guest_token).status == 409);
    const HttpResult undone = send(manager, base.resolved(QUrl(undo_path)), "POST", compensate);
    REQUIRE(undone.status == 200);
    REQUIRE(undone.json().value(QStringLiteral("generation")).toString() == QStringLiteral("5"));
    REQUIRE(send(manager, base.resolved(QUrl(custom_path)), "GET").json()
            .value(QStringLiteral("content_base64")).toString() == QStringLiteral("aGVsbG8="));
    REQUIRE(send(manager, base.resolved(QUrl(redo_path)), "POST",
                 {{QStringLiteral("expected_generation"), QStringLiteral("5")}}).status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(custom_path)), "GET").json()
            .value(QStringLiteral("content_base64")).toString() == QStringLiteral("d29ybGQ="));
    const QUuid extra_a = createUuidV7();
    const QUuid extra_b = createUuidV7();
    const HttpResult grouped = send(manager, base.resolved(QUrl(groups_path)), "POST", QJsonObject{
        {QStringLiteral("expected_generation"), QStringLiteral("6")},
        {QStringLiteral("edits"), QJsonArray{
            QJsonObject{{QStringLiteral("uuid"), id(extra_a)},
                        {QStringLiteral("kind"), QStringLiteral("test.payload")},
                        {QStringLiteral("expected_version"), QStringLiteral("-1")},
                        {QStringLiteral("content_base64"), QStringLiteral("YQ==")}},
            QJsonObject{{QStringLiteral("uuid"), id(extra_b)},
                        {QStringLiteral("kind"), QStringLiteral("test.payload")},
                        {QStringLiteral("expected_version"), QStringLiteral("-1")},
                        {QStringLiteral("content_base64"), QStringLiteral("Yg==")}}}}});
    REQUIRE(grouped.status == 201);
    REQUIRE(grouped.json().value(QStringLiteral("generation")).toString() == QStringLiteral("8"));
    REQUIRE(database.object({root->uuid}, QUuid(revision_id), extra_a, &error)->content == QByteArrayLiteral("a"));
    REQUIRE(database.object({root->uuid}, QUuid(revision_id), extra_b, &error)->content == QByteArrayLiteral("b"));
    REQUIRE(send(manager, base.resolved(QUrl(undo_path)), "POST",
                 {{QStringLiteral("expected_generation"), QStringLiteral("8")}}).status == 200);
    REQUIRE(database.object({root->uuid}, QUuid(revision_id), extra_a, &error)->deleted);
    REQUIRE(database.object({root->uuid}, QUuid(revision_id), extra_b, &error)->deleted);
    REQUIRE(send(manager, base.resolved(QUrl(redo_path)), "POST",
                 {{QStringLiteral("expected_generation"), QStringLiteral("10")}}).status == 200);
    REQUIRE(!database.object({root->uuid}, QUuid(revision_id), extra_a, &error)->deleted);
    REQUIRE(!database.object({root->uuid}, QUuid(revision_id), extra_b, &error)->deleted);
    const HttpResult history = send(manager, base.resolved(QUrl(groups_read_path)), "GET");
    REQUIRE(history.status == 200);
    REQUIRE(history.json().value(QStringLiteral("groups")).toArray().size() == 8);
    const HttpResult finalized = send(manager, base.resolved(QUrl(QStringLiteral("branches/") + branch_id +
        QStringLiteral("/finalize"))), "POST",
        {{QStringLiteral("expected_generation"), QStringLiteral("12")},
         {QStringLiteral("title"), QStringLiteral("REST baseline")}});
    REQUIRE(finalized.status == 200);
    REQUIRE(database.verifyProject({root->uuid}, QUuid(project_id)).valid);
    REQUIRE(send(manager, base.resolved(QUrl(net_get_path)), "GET").status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(groups_read_path)), "GET").status == 200);
    REQUIRE(send(manager, base.resolved(QUrl(groups_path)), "POST", group_body).status == 409);
    return 0;
}
