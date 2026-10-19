#include <aowis/db/http_routes.h>
#include <aowis/db/http_hardening.h>
#include <aowis/db/model_codec.h>

#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <functional>
#include <initializer_list>
#include <QSet>
#include <QUrlQuery>
#include <utility>

namespace aowis::db
{
namespace
{
using Status = QHttpServerResponse::StatusCode;

QString uuidString(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

std::optional<QUuid> parseUuid(const QString &text)
{
    const QUuid uuid(text);
    if (uuid.isNull())
        return std::nullopt;
    if (uuidString(uuid).compare(text, Qt::CaseInsensitive) != 0)
        return std::nullopt;
    return uuid;
}

QHttpServerResponse jsonError(Status code, const QString &message)
{
    return QHttpServerResponse(QJsonObject{{QStringLiteral("error"), message}}, code);
}

QHttpServerResponse unauthorized()
{
    return jsonError(Status::Unauthorized, QStringLiteral("Authentication required"));
}

QHttpServerResponse unavailable()
{
    // Do not disclose the existence of projects/branches without read access.
    return jsonError(Status::NotFound, QStringLiteral("Resource not found or unavailable"));
}

std::optional<QJsonObject> jsonBody(const QHttpServerRequest &request, qsizetype max_bytes = 65536)
{
    const QByteArray body = request.body();
    if (body.isEmpty() || body.size() > max_bytes)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

std::optional<SecurityListOptions> securityListOptions(const QHttpServerRequest &request)
{
    SecurityListOptions options;
    QSet<QString> seen;
    const QUrlQuery query(request.url());
    for (const QPair<QString, QString> &item : query.queryItems())
    {
        if (seen.contains(item.first))
            return std::nullopt;
        seen.insert(item.first);
        if (item.first == QStringLiteral("limit"))
        {
            bool valid = false;
            const int limit = item.second.toInt(&valid);
            if (!valid || limit < 1 || limit > 200 || QString::number(limit) != item.second)
                return std::nullopt;
            options.limit = limit;
        }
        else if (item.first == QStringLiteral("after_uuid"))
        {
            const std::optional<QUuid> uuid = parseUuid(item.second);
            if (!uuid)
                return std::nullopt;
            options.after_uuid = *uuid;
        }
        else if (item.first == QStringLiteral("include_inactive"))
        {
            if (item.second != QStringLiteral("true") && item.second != QStringLiteral("false"))
                return std::nullopt;
            options.include_inactive = item.second == QStringLiteral("true");
        }
        else
            return std::nullopt;
    }
    return options;
}

// Merge queries have two required, named UUID parameters. Reject repeated or
// unexpected parameters rather than silently changing the compared revisions.
struct RevisionPair
{
    QUuid left;
    QUuid right;
};

std::optional<RevisionPair> revisionPairQuery(const QHttpServerRequest &request)
{
    if (!request.body().isEmpty())
        return std::nullopt;
    const QUrlQuery query(request.url());
    const QList<QPair<QString, QString>> items = query.queryItems();
    if (items.size() != 2)
        return std::nullopt;
    std::optional<QUuid> left;
    std::optional<QUuid> right;
    for (const QPair<QString, QString> &item : items)
    {
        const std::optional<QUuid> uuid = parseUuid(item.second);
        if (!uuid)
            return std::nullopt;
        if (item.first == QStringLiteral("left_revision_uuid") && !left)
            left = uuid;
        else if (item.first == QStringLiteral("right_revision_uuid") && !right)
            right = uuid;
        else
            return std::nullopt;
    }
    if (!left || !right)
        return std::nullopt;
    return RevisionPair{*left, *right};
}

QJsonObject comparisonJson(const RevisionComparison &comparison)
{
    QJsonArray bases;
    for (const QUuid &uuid : comparison.merge_base_revision_uuids)
        bases.append(uuidString(uuid));
    return {{QStringLiteral("project_uuid"), uuidString(comparison.project_uuid)},
            {QStringLiteral("left_revision_uuid"), uuidString(comparison.left_revision_uuid)},
            {QStringLiteral("right_revision_uuid"), uuidString(comparison.right_revision_uuid)},
            {QStringLiteral("left_is_ancestor_of_right"), comparison.left_is_ancestor_of_right},
            {QStringLiteral("right_is_ancestor_of_left"), comparison.right_is_ancestor_of_left},
            {QStringLiteral("merge_base_revision_uuids"), bases}};
}

QString changeKindName(ObjectMergeKind kind)
{
    switch (kind)
    {
    case ObjectMergeKind::LeftOnly: return QStringLiteral("left_only");
    case ObjectMergeKind::RightOnly: return QStringLiteral("right_only");
    case ObjectMergeKind::BothSame: return QStringLiteral("both_same");
    case ObjectMergeKind::Conflict: return QStringLiteral("conflict");
    case ObjectMergeKind::FieldsMerged: return QStringLiteral("fields_merged");
    }
    return QStringLiteral("unknown");
}

// Mutation endpoints accept only documented JSON fields. Silently ignoring
// unexpected keys is unsafe for rights management (typos can grant wrong rights).
bool exactFields(const QJsonObject &body, std::initializer_list<const char *> fields)
{
    if (body.size() != static_cast<int>(fields.size()))
        return false;
    for (const char *field : fields)
    {
        if (!body.contains(QString::fromLatin1(field)))
            return false;
    }
    return true;
}

// A merge commit accepts only explicitly specified conflict choices. Keep
// JSON validation separate from the core: the database still validates the
// choices against its authoritative, transaction-checked merge preview.
std::optional<ObjectMergeSide> mergeSide(const QJsonValue &value)
{
    if (!value.isString())
        return std::nullopt;
    if (value.toString() == QStringLiteral("left"))
        return ObjectMergeSide::Left;
    if (value.toString() == QStringLiteral("right"))
        return ObjectMergeSide::Right;
    return std::nullopt;
}

std::optional<QVector<ObjectMergeResolution>> mergeResolutions(const QJsonValue &value)
{
    if (!value.isArray())
        return std::nullopt;
    const QJsonArray entries = value.toArray();
    if (entries.size() > 256)
        return std::nullopt;
    QVector<ObjectMergeResolution> resolutions;
    resolutions.reserve(entries.size());
    QSet<QUuid> seen_objects;
    int total_fields = 0;
    for (const QJsonValue &entry : entries)
    {
        if (!entry.isObject())
            return std::nullopt;
        const QJsonObject item = entry.toObject();
        if (!item.value(QStringLiteral("object_uuid")).isString())
            return std::nullopt;
        const std::optional<QUuid> uuid = parseUuid(item.value(QStringLiteral("object_uuid")).toString());
        if (!uuid || seen_objects.contains(*uuid))
            return std::nullopt;
        seen_objects.insert(*uuid);
        ObjectMergeResolution resolution;
        resolution.object_uuid = *uuid;
        if (item.contains(QStringLiteral("side")))
        {
            if (!exactFields(item, {"object_uuid", "side"}))
                return std::nullopt;
            const std::optional<ObjectMergeSide> side = mergeSide(item.value(QStringLiteral("side")));
            if (!side)
                return std::nullopt;
            resolution.side = *side;
        }
        else
        {
            if (!exactFields(item, {"object_uuid", "field_resolutions"}) ||
                !item.value(QStringLiteral("field_resolutions")).isArray())
                return std::nullopt;
            const QJsonArray fields = item.value(QStringLiteral("field_resolutions")).toArray();
            if (fields.isEmpty() || fields.size() > 64 || total_fields + fields.size() > 1024)
                return std::nullopt;
            total_fields += fields.size();
            QSet<QString> seen_fields;
            for (const QJsonValue &field_value : fields)
            {
                if (!field_value.isObject())
                    return std::nullopt;
                const QJsonObject field = field_value.toObject();
                if (!exactFields(field, {"field_name", "side"}) ||
                    !field.value(QStringLiteral("field_name")).isString())
                    return std::nullopt;
                const QString name = field.value(QStringLiteral("field_name")).toString();
                const std::optional<ObjectMergeSide> side = mergeSide(field.value(QStringLiteral("side")));
                if (!side || name.isEmpty() || name.size() > 256 || seen_fields.contains(name))
                    return std::nullopt;
                seen_fields.insert(name);
                resolution.field_resolutions.append(ObjectMergeFieldResolution{name, *side});
            }
        }
        resolutions.append(resolution);
    }
    return resolutions;
}

bool emptyMutationRequest(const QHttpServerRequest &request)
{
    return !request.url().hasQuery() && request.body().isEmpty();
}

std::optional<AccessScope> scopeFromJson(const QJsonObject &body)
{
    const QJsonValue kind = body.value(QStringLiteral("scope_kind"));
    const QJsonValue uuid_value = body.value(QStringLiteral("scope_uuid"));
    if (!kind.isString())
        return std::nullopt;
    if (kind.toString() == QStringLiteral("system"))
    {
        if (!uuid_value.isNull())
            return std::nullopt;
        return AccessScope::system();
    }
    if (!uuid_value.isString())
        return std::nullopt;
    const std::optional<QUuid> uuid = parseUuid(uuid_value.toString());
    if (!uuid)
        return std::nullopt;
    if (kind.toString() == QStringLiteral("project"))
        return AccessScope::project(*uuid);
    if (kind.toString() == QStringLiteral("branch"))
        return AccessScope::branch(*uuid);
    return std::nullopt;
}

QJsonObject catalogUserJson(const UserInfo &item)
{
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("username"), item.username},
            {QStringLiteral("disabled"), item.disabled}};
}

QJsonObject catalogGroupJson(const GroupInfo &item)
{
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("name"), item.name},
            {QStringLiteral("disabled"), item.disabled}};
}

QJsonObject catalogRoleJson(const RoleInfo &item)
{
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("key"), item.key},
            {QStringLiteral("builtin"), item.builtin}};
}

QJsonObject catalogPermissionJson(const PermissionInfo &item)
{
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("key"), item.key},
            {QStringLiteral("builtin"), item.builtin}};
}

QJsonObject membershipJson(const GroupMembershipInfo &item)
{
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("group_uuid"), uuidString(item.group_uuid)},
            {QStringLiteral("user_uuid"), uuidString(item.user_uuid)},
            {QStringLiteral("created_by_uuid"), uuidString(item.created_by_uuid)},
            {QStringLiteral("created_at_ms"), QString::number(item.created_at_ms)},
            {QStringLiteral("revoked"), item.revoked}};
}

QJsonObject grantJson(const RolePermissionGrantInfo &item)
{
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("role_uuid"), uuidString(item.role_uuid)},
            {QStringLiteral("permission_uuid"), uuidString(item.permission_uuid)},
            {QStringLiteral("revoked"), item.revoked}};
}

QJsonObject assignmentJson(const RoleAssignmentInfo &item)
{
    QString scope_kind = QStringLiteral("system");
    if (item.scope.kind == AccessScopeKind::Project)
        scope_kind = QStringLiteral("project");
    else if (item.scope.kind == AccessScopeKind::Branch)
        scope_kind = QStringLiteral("branch");
    return {{QStringLiteral("uuid"), uuidString(item.uuid)},
            {QStringLiteral("principal_kind"), item.principal_kind == PrincipalKind::User ?
                QStringLiteral("user") : QStringLiteral("group")},
            {QStringLiteral("principal_uuid"), uuidString(item.principal_uuid)},
            {QStringLiteral("role_uuid"), uuidString(item.role_uuid)},
            {QStringLiteral("scope_kind"), scope_kind},
            {QStringLiteral("scope_uuid"), item.scope.kind == AccessScopeKind::System ?
                QJsonValue(QJsonValue::Null) : QJsonValue(uuidString(item.scope.resource_uuid))},
            {QStringLiteral("created_by_uuid"), uuidString(item.created_by_uuid)},
            {QStringLiteral("created_at_ms"), QString::number(item.created_at_ms)},
            {QStringLiteral("revoked"), item.revoked}};
}

QJsonObject projectJson(const ProjectInfo &project)
{
    return {{QStringLiteral("uuid"), uuidString(project.uuid)},
            {QStringLiteral("name"), project.name},
            {QStringLiteral("created_by_uuid"), uuidString(project.created_by_uuid)}};
}

QJsonObject branchJson(const BranchInfo &branch)
{
    return {{QStringLiteral("uuid"), uuidString(branch.uuid)},
            {QStringLiteral("project_uuid"), uuidString(branch.project_uuid)},
            {QStringLiteral("name"), branch.name},
            {QStringLiteral("head_revision_uuid"), uuidString(branch.head_revision_uuid)},
            {QStringLiteral("created_by_uuid"), uuidString(branch.created_by_uuid)}};
}

QJsonObject revisionJson(const RevisionInfo &revision)
{
    QJsonArray parents;
    for (const QUuid &parent : revision.parent_revision_uuids)
        parents.append(uuidString(parent));
    // 64-bit values are decimal strings to avoid precision loss in JSON.
    return {{QStringLiteral("uuid"), uuidString(revision.uuid)},
            {QStringLiteral("project_uuid"), uuidString(revision.project_uuid)},
            {QStringLiteral("branch_uuid"), uuidString(revision.branch_uuid)},
            {QStringLiteral("number"), QString::number(revision.number)},
            {QStringLiteral("generation"), QString::number(revision.generation)},
            {QStringLiteral("finalized"), revision.finalized},
            {QStringLiteral("parent_revision_uuids"), parents},
            {QStringLiteral("created_by_uuid"), uuidString(revision.created_by_uuid)},
            {QStringLiteral("finalized_by_uuid"), uuidString(revision.finalized_by_uuid)},
            {QStringLiteral("snapshot_sha256"), QString::fromLatin1(revision.snapshot_sha256.toHex())},
            {QStringLiteral("chain_sha256"), QString::fromLatin1(revision.chain_sha256.toHex())}};
}

std::optional<qint64> expectedGeneration(const QJsonObject &object)
{
    const QJsonValue generation = object.value(QStringLiteral("expected_generation"));
    if (!generation.isString())
        return std::nullopt;
    const QString text = generation.toString();
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    if (!ok || value < 0 || QString::number(value) != text)
        return std::nullopt;
    return value;
}

// Require canonical padded base64. Qt's permissive decoder otherwise accepts
// malformed input, making payloads ambiguous to clients and audit tools.
std::optional<QByteArray> decodeBase64(const QJsonValue &value, qsizetype max_bytes)
{
    if (!value.isString())
        return std::nullopt;
    const QByteArray encoded = value.toString().toLatin1();
    if (encoded.size() > ((max_bytes + 2) / 3) * 4)
        return std::nullopt;
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    if (decoded.size() > max_bytes || decoded.toBase64() != encoded)
        return std::nullopt;
    return decoded;
}

std::optional<qint64> expectedVersion(const QJsonValue &value)
{
    if (!value.isString())
        return std::nullopt;
    const QString text = value.toString();
    bool ok = false;
    const qint64 version = text.toLongLong(&ok);
    if (!ok || version < -1 || QString::number(version) != text)
        return std::nullopt;
    return version;
}

QJsonObject objectGroupJson(const ObjectGroup &group)
{
    QJsonArray events;
    for (const QUuid &uuid : group.object_event_uuids)
        events.append(uuidString(uuid));
    return {{QStringLiteral("uuid"), uuidString(group.uuid)},
            {QStringLiteral("revision_uuid"), uuidString(group.revision_uuid)},
            {QStringLiteral("actor_uuid"), uuidString(group.actor_uuid)},
            {QStringLiteral("target_group_uuid"), group.target_group_uuid.isNull()
                ? QJsonValue(QJsonValue::Null) : QJsonValue(uuidString(group.target_group_uuid))},
            {QStringLiteral("action"), group.action},
            {QStringLiteral("first_sequence"), QString::number(group.first_sequence)},
            {QStringLiteral("last_sequence"), QString::number(group.last_sequence)},
            {QStringLiteral("occurred_at_ms"), QString::number(group.occurred_at_ms)},
            {QStringLiteral("object_event_uuids"), events}};
}

QJsonObject workingGeneration(const Database &database, const OperationContext &actor,
                              const QUuid &branch_uuid)
{
    const std::optional<BranchInfo> branch = database.branch(actor, branch_uuid);
    if (!branch)
        return {};
    const std::optional<RevisionInfo> revision = database.revision(actor, branch->head_revision_uuid);
    if (!revision)
        return {};
    return {{QStringLiteral("revision_uuid"), uuidString(revision->uuid)},
            {QStringLiteral("generation"), QString::number(revision->generation)}};
}

} // namespace

HttpRoutes::HttpRoutes(Database &database, Authenticate authenticate)
    : database_(database), authenticate_(std::move(authenticate))
{
    this->login_clock_.start();
}

std::optional<OperationContext> HttpRoutes::actorFor(const QHttpServerRequest &request) const
{
    if (!this->authenticate_)
        return std::nullopt;
    const std::optional<OperationContext> actor = this->authenticate_(request);
    if (!actor || actor->actor_uuid.isNull())
        return std::nullopt;
    return actor;
}

bool HttpRoutes::registerOn(QHttpServer &server)
{
    const QHttpServerRequest::Methods get = QHttpServerRequest::Method::Get;
    const QHttpServerRequest::Methods post = QHttpServerRequest::Method::Post;
    const QHttpServerRequest::Methods put = QHttpServerRequest::Method::Put;

    // A health check intentionally contains no database or identity details.
    if (!server.route(QStringLiteral("/api/v1/health"), get, []() {
        return QHttpServerResponse(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
    }))
        return false;

    // Local password enrollment can exchange verified credentials for a
    // regular revocable bearer session. Never accept caller-supplied actor UUID.
    if (!server.route(QStringLiteral("/api/v1/session/login"), post,
                      [this](const QHttpServerRequest &request) {
        if (request.url().hasQuery())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid login request"));
        const std::optional<QJsonObject> body = jsonBody(request, 2048);
        if (!body || !exactFields(*body, {"username", "password"}) ||
            !body->value(QStringLiteral("username")).isString() ||
            !body->value(QStringLiteral("password")).isString())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid login request"));
        const QString username = body->value(QStringLiteral("username")).toString();
        // Throttle *before* the expensive password KDF, including unknown
        // usernames and failed credentials. Count successes too, so probing
        // a valid account never provides an unlimited retry path. This is a
        // process-wide HTTP safeguard, not a replacement for proxy limits.
        if (!this->login_throttle_.allowAttempt(username, this->login_clock_.elapsed()))
            return jsonError(static_cast<Status>(429), QStringLiteral("Too many login attempts"));
        const QByteArray secret = body->value(QStringLiteral("password")).toString().toUtf8();
        QString error;
        const std::optional<IssuedSession> session = this->database_.loginWithPassword(
            username, secret, 3600, &error);
        if (!session)
            return unauthorized();
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("session_uuid"), uuidString(session->uuid)},
            {QStringLiteral("user_uuid"), uuidString(session->user_uuid)},
            {QStringLiteral("expires_at_ms"), QString::number(session->expires_at_ms)},
            {QStringLiteral("bearer_token"), QString::fromLatin1(session->bearer_token)}});
    }))
        return false;

    // Anonymous redemption of a short-lived recovery capability issued
    // locally by a trusted administrator. No username or target user UUID is
    // accepted. The token is never logged, persisted in plaintext or reusable.
    if (!server.route(QStringLiteral("/api/v1/session/recover"), post,
                      [this](const QHttpServerRequest &request) {
        if (request.url().hasQuery())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid recovery request"));
        const std::optional<QJsonObject> body = jsonBody(request, 4096);
        if (!body || !exactFields(*body, {"recovery_token", "new_password"}) ||
            !body->value(QStringLiteral("recovery_token")).isString() ||
            !body->value(QStringLiteral("new_password")).isString())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid recovery request"));
        QString error;
        if (!this->database_.redeemPasswordRecovery(
                body->value(QStringLiteral("recovery_token")).toString().toLatin1(),
                body->value(QStringLiteral("new_password")).toString().toUtf8(), &error))
        {
            if (error == QStringLiteral("Invalid new password"))
                return jsonError(Status::BadRequest, QStringLiteral("Invalid new password"));
            if (error == QStringLiteral("Invalid or expired recovery token"))
                return unauthorized();
            return jsonError(Status::InternalServerError, QStringLiteral("Recovery failed"));
        }
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("changed"), true},
            {QStringLiteral("sessions_revoked"), true}});
    }))
        return false;

    // Session self-service. The token is resolved by the DB itself;
    // neither actor UUID nor session UUID can be supplied by the client.
    // The host's authenticator must also accept it, so these routes use the
    // same authentication boundary as all other protected API endpoints.
    if (!server.route(QStringLiteral("/api/v1/session/current"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!emptyMutationRequest(request))
            return jsonError(Status::BadRequest, QStringLiteral("Unexpected query or body"));
        const std::optional<QByteArray> token = parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        if (!token)
            return unauthorized();
        const std::optional<ActiveSession> session = this->database_.activeSession(*token);
        if (!session || session->user_uuid != actor->actor_uuid)
            return unauthorized();
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("session_uuid"), uuidString(session->uuid)},
            {QStringLiteral("user_uuid"), uuidString(session->user_uuid)},
            {QStringLiteral("expires_at_ms"), QString::number(session->expires_at_ms)}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/session/current"),
                      QHttpServerRequest::Method::Delete,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!emptyMutationRequest(request))
            return jsonError(Status::BadRequest, QStringLiteral("Unexpected query or body"));
        const std::optional<QByteArray> token = parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        if (!token)
            return unauthorized();
        const std::optional<ActiveSession> session = this->database_.activeSession(*token);
        if (!session || session->user_uuid != actor->actor_uuid)
            return unauthorized();
        QString error;
        if (!this->database_.logoutSession(*token, &error))
        {
            if (error == QStringLiteral("Invalid or expired session"))
                return unauthorized();
            return jsonError(Status::InternalServerError, QStringLiteral("Session logout failed"));
        }
        return QHttpServerResponse(QJsonObject{{QStringLiteral("revoked"), true}});
    }))
        return false;

    // Revoke every other active session in one audited transaction.
    // Only the presented bearer selects the user; no user or session UUID
    // is accepted. This is deliberately separate from DELETE current.
    if (!server.route(QStringLiteral("/api/v1/session/revoke-others"), post,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!emptyMutationRequest(request))
            return jsonError(Status::BadRequest, QStringLiteral("Unexpected query or body"));
        const std::optional<QByteArray> token = parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        if (!token)
            return unauthorized();
        const std::optional<ActiveSession> current = this->database_.activeSession(*token);
        if (!current || current->user_uuid != actor->actor_uuid)
            return unauthorized();
        QString error;
        const std::optional<qint64> count = this->database_.revokeOtherSessions(*token, &error);
        if (!count)
        {
            if (error == QStringLiteral("Invalid or expired session"))
                return unauthorized();
            return jsonError(Status::InternalServerError, QStringLiteral("Session revocation failed"));
        }
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("revoked_count"), QString::number(*count)},
            {QStringLiteral("current_session_uuid"), uuidString(current->uuid)}});
    }))
        return false;

    // Self-service management of the holder's OTHER sessions. Only an
    // authenticated token decides whose sessions are visible or revocable.
    // Revoke does not accept a user UUID and never exposes token hashes.
    if (!server.route(QStringLiteral("/api/v1/sessions"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!request.body().isEmpty())
            return jsonError(Status::BadRequest, QStringLiteral("Unexpected request body"));
        int limit = 100;
        QUuid after_uuid;
        QSet<QString> seen;
        for (const QPair<QString, QString> &item : QUrlQuery(request.url()).queryItems())
        {
            if (seen.contains(item.first))
                return jsonError(Status::BadRequest, QStringLiteral("Duplicate query field"));
            seen.insert(item.first);
            if (item.first == QStringLiteral("limit"))
            {
                bool ok = false;
                const int value = item.second.toInt(&ok);
                if (!ok || value < 1 || value > 100 || QString::number(value) != item.second)
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid limit"));
                limit = value;
            }
            else if (item.first == QStringLiteral("after_uuid"))
            {
                const std::optional<QUuid> parsed = parseUuid(item.second);
                if (!parsed)
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid cursor"));
                after_uuid = *parsed;
            }
            else
                return jsonError(Status::BadRequest, QStringLiteral("Unknown query field"));
        }
        const std::optional<QByteArray> token = parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        if (!token)
            return unauthorized();
        const std::optional<ActiveSession> current = this->database_.activeSession(*token);
        if (!current || current->user_uuid != actor->actor_uuid)
            return unauthorized();
        QString error;
        const QVector<OwnSessionInfo> sessions = this->database_.ownSessions(*token, after_uuid, limit, &error);
        if (error == QStringLiteral("Invalid or expired session"))
            return unauthorized();
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Session listing failed"));
        QJsonArray records;
        for (const OwnSessionInfo &entry : sessions)
            records.append(QJsonObject{
                {QStringLiteral("session_uuid"), uuidString(entry.uuid)},
                {QStringLiteral("created_at_ms"), QString::number(entry.created_at_ms)},
                {QStringLiteral("expires_at_ms"), QString::number(entry.expires_at_ms)},
                {QStringLiteral("current"), entry.current}});
        return QHttpServerResponse(QJsonObject{{QStringLiteral("sessions"), records}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/sessions/<arg>"),
                      QHttpServerRequest::Method::Delete,
                      [this](const QString &raw_uuid, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!emptyMutationRequest(request))
            return jsonError(Status::BadRequest, QStringLiteral("Unexpected query or body"));
        const std::optional<QUuid> uuid = parseUuid(raw_uuid);
        if (!uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid session UUID"));
        const std::optional<QByteArray> token = parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        if (!token)
            return unauthorized();
        const std::optional<ActiveSession> current = this->database_.activeSession(*token);
        if (!current || current->user_uuid != actor->actor_uuid)
            return unauthorized();
        QString error;
        if (!this->database_.revokeOwnSession(*token, *uuid, &error))
        {
            if (error == QStringLiteral("Session not found or unavailable"))
                return unavailable();
            if (error == QStringLiteral("Invalid or expired session"))
                return unauthorized();
            return jsonError(Status::InternalServerError, QStringLiteral("Revocation failed"));
        }
        return QHttpServerResponse(QJsonObject{{QStringLiteral("revoked"), true},
                                               {QStringLiteral("session_uuid"), uuidString(*uuid)}});
    }))
        return false;

    // A bearer-session holder may rotate only their own password.
    // The old password is mandatory; a stolen token alone is insufficient.
    // Rotation revokes every bearer session and appends a security audit event.
    if (!server.route(QStringLiteral("/api/v1/session/password"), post,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (request.url().hasQuery())
            return jsonError(Status::BadRequest, QStringLiteral("Unexpected query"));
        const std::optional<QByteArray> token = parseBearerAuthorization(
            request.value(QByteArrayLiteral("authorization")));
        if (!token)
            return unauthorized();
        const std::optional<ActiveSession> session = this->database_.activeSession(*token);
        if (!session || session->user_uuid != actor->actor_uuid)
            return unauthorized();
        const std::optional<QJsonObject> body = jsonBody(request, 4096);
        if (!body || !exactFields(*body, {"current_password", "new_password"}) ||
            !body->value(QStringLiteral("current_password")).isString() ||
            !body->value(QStringLiteral("new_password")).isString())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid password change request"));
        QString error;
        if (!this->database_.changeOwnPassword(*token,
                body->value(QStringLiteral("current_password")).toString().toUtf8(),
                body->value(QStringLiteral("new_password")).toString().toUtf8(), &error))
        {
            if (error.startsWith(QStringLiteral("New password")))
                return jsonError(Status::BadRequest, QStringLiteral("Invalid new password"));
            if (error == QStringLiteral("Invalid or expired session"))
                return unauthorized();
            if (error == QStringLiteral("Current password incorrect or temporarily unavailable"))
                return jsonError(Status::Forbidden, QStringLiteral("Current password incorrect or unavailable"));
            return jsonError(Status::InternalServerError, QStringLiteral("Password change failed"));
        }
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("changed"), true},
            {QStringLiteral("sessions_revoked"), true}});
    }))
        return false;

    // Security catalog routes are deliberately read-only. They never expose
    // bearer tokens and require system-level security.manage; project owners
    // do not acquire this permission through project ownership.
    if (!server.route(QStringLiteral("/api/v1/admin/users"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!options)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query"));
        QString error;
        const QVector<UserInfo> records = this->database_.securityUsers(*actor, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const UserInfo &item : records)
            items.append(catalogUserJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("users"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/groups"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!options)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query"));
        QString error;
        const QVector<GroupInfo> records = this->database_.securityGroups(*actor, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const GroupInfo &item : records)
            items.append(catalogGroupJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("groups"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/roles"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!options)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query"));
        QString error;
        const QVector<RoleInfo> records = this->database_.securityRoles(*actor, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const RoleInfo &item : records)
            items.append(catalogRoleJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("roles"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/permissions"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!options)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query"));
        QString error;
        const QVector<PermissionInfo> records = this->database_.securityPermissions(*actor, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const PermissionInfo &item : records)
            items.append(catalogPermissionJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("permissions"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/groups/<arg>/members"), get,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<QUuid> uuid = parseUuid(id);
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!uuid || !options)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query or UUID"));
        QString error;
        const QVector<GroupMembershipInfo> records = this->database_.securityGroupMembers(*actor, *uuid, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const GroupMembershipInfo &item : records)
            items.append(membershipJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("memberships"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/roles/<arg>/permissions"), get,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<QUuid> uuid = parseUuid(id);
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!uuid || !options)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query or UUID"));
        QString error;
        const QVector<RolePermissionGrantInfo> records = this->database_.securityRolePermissions(*actor, *uuid, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const RolePermissionGrantInfo &item : records)
            items.append(grantJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("grants"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/principals/<arg>/<arg>/assignments"), get,
                      [this](const QString &kind, const QString &id,
                             const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<QUuid> uuid = parseUuid(id);
        const std::optional<SecurityListOptions> options = securityListOptions(request);
        if (!uuid || !options || (kind != QStringLiteral("user") && kind != QStringLiteral("group")))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid catalog query or principal"));
        QString error;
        const QVector<RoleAssignmentInfo> records = this->database_.securityRoleAssignments(
            *actor, kind == QStringLiteral("user") ? PrincipalKind::User : PrincipalKind::Group,
            *uuid, *options, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Catalog read failed"));
        QJsonArray items;
        for (const RoleAssignmentInfo &item : records)
            items.append(assignmentJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("assignments"), items}});
    }))
        return false;

    // Mutation routes reuse the *audited* database functions. The
    // authenticated host determines the actor; HTTP bodies cannot set it.
    // Bootstrap and session/token provisioning deliberately have no HTTP route.
    const std::function<bool(const QString &, const QString &,
                             std::function<std::optional<QJsonObject>(const OperationContext &, const QString &)>)>
        registerCreation = [this, &server, post](const QString &route,
                                                  const QString &field,
                                                  std::function<std::optional<QJsonObject>(const OperationContext &, const QString &)> create) {
        return server.route(route, post,
            [this, field, create](const QHttpServerRequest &request) {
                const std::optional<OperationContext> actor = actorFor(request);
                if (!actor)
                    return unauthorized();
                if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
                    return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
                const std::optional<QJsonObject> body = jsonBody(request, 4096);
                if (request.url().hasQuery() || !body || body->size() != 1 ||
                    !body->value(field).isString() || body->value(field).toString().isEmpty())
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid creation request"));
                const std::optional<QJsonObject> result = create(*actor, body->value(field).toString());
                if (!result)
                    return jsonError(Status::Conflict, QStringLiteral("Creation rejected"));
                return QHttpServerResponse(*result, Status::Created);
            });
    };
    if (!registerCreation(QStringLiteral("/api/v1/admin/users"), QStringLiteral("username"),
                          [this](const OperationContext &actor, const QString &value) -> std::optional<QJsonObject> {
        const std::optional<UserInfo> info = this->database_.createUser(actor, value);
        return info ? std::optional<QJsonObject>(catalogUserJson(*info)) : std::nullopt;
    }) ||
        !registerCreation(QStringLiteral("/api/v1/admin/groups"), QStringLiteral("name"),
                          [this](const OperationContext &actor, const QString &value) -> std::optional<QJsonObject> {
        const std::optional<GroupInfo> info = this->database_.createGroup(actor, value);
        return info ? std::optional<QJsonObject>(catalogGroupJson(*info)) : std::nullopt;
    }) ||
        !registerCreation(QStringLiteral("/api/v1/admin/roles"), QStringLiteral("key"),
                          [this](const OperationContext &actor, const QString &value) -> std::optional<QJsonObject> {
        const std::optional<RoleInfo> info = this->database_.createRole(actor, value);
        return info ? std::optional<QJsonObject>(catalogRoleJson(*info)) : std::nullopt;
    }) ||
        !registerCreation(QStringLiteral("/api/v1/admin/permissions"), QStringLiteral("key"),
                          [this](const OperationContext &actor, const QString &value) -> std::optional<QJsonObject> {
        const std::optional<PermissionInfo> info = this->database_.createPermission(actor, value);
        return info ? std::optional<QJsonObject>(catalogPermissionJson(*info)) : std::nullopt;
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/groups/<arg>/members"), post,
                      [this](const QString &group_id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<QUuid> group_uuid = parseUuid(group_id);
        const std::optional<QJsonObject> body = jsonBody(request, 4096);
        if (request.url().hasQuery() || !group_uuid || !body || !exactFields(*body, {"user_uuid"}))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid membership request"));
        const std::optional<QUuid> user_uuid = parseUuid(body->value(QStringLiteral("user_uuid")).toString());
        if (!user_uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid user UUID"));
        const std::optional<QUuid> uuid = this->database_.addGroupMember(*actor, *group_uuid, *user_uuid);
        if (!uuid)
            return jsonError(Status::Conflict, QStringLiteral("Membership rejected"));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("uuid"), uuidString(*uuid)}}, Status::Created);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/roles/<arg>/permissions"), post,
                      [this](const QString &role_id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<QUuid> role_uuid = parseUuid(role_id);
        const std::optional<QJsonObject> body = jsonBody(request, 4096);
        if (request.url().hasQuery() || !role_uuid || !body || !exactFields(*body, {"permission_uuid"}))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid permission grant request"));
        const std::optional<QUuid> permission_uuid = parseUuid(body->value(QStringLiteral("permission_uuid")).toString());
        if (!permission_uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid permission UUID"));
        const std::optional<QUuid> uuid = this->database_.grantRolePermission(*actor, *role_uuid, *permission_uuid);
        if (!uuid)
            return jsonError(Status::Conflict, QStringLiteral("Permission grant rejected"));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("uuid"), uuidString(*uuid)}}, Status::Created);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/admin/principals/<arg>/<arg>/assignments"), post,
                      [this](const QString &principal_kind, const QString &principal_id,
                             const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
            return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
        const std::optional<QUuid> principal_uuid = parseUuid(principal_id);
        const std::optional<QJsonObject> body = jsonBody(request, 4096);
        if (request.url().hasQuery() || !principal_uuid || !body ||
            (principal_kind != QStringLiteral("user") && principal_kind != QStringLiteral("group")) ||
            !exactFields(*body, {"role_uuid", "scope_kind", "scope_uuid"}))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid role assignment request"));
        const std::optional<QUuid> role_uuid = parseUuid(body->value(QStringLiteral("role_uuid")).toString());
        const std::optional<AccessScope> scope = scopeFromJson(*body);
        if (!role_uuid || !scope)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid role or scope"));
        const std::optional<QUuid> uuid = this->database_.assignRole(
            *actor, principal_kind == QStringLiteral("user") ? PrincipalKind::User : PrincipalKind::Group,
            *principal_uuid, *role_uuid, *scope);
        if (!uuid)
            return jsonError(Status::Conflict, QStringLiteral("Role assignment rejected"));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("uuid"), uuidString(*uuid)}}, Status::Created);
    }))
        return false;

    // A revoked grant is retained for audit/history. DELETE marks it revoked,
    // never deletes the UUID, so regranting creates a distinct grant.
    const QHttpServerRequest::Methods remove = QHttpServerRequest::Method::Delete;
    const std::function<bool(const QString &, std::function<bool(const OperationContext &, const QUuid &)>)>
        registerRevocation = [this, &server, remove](const QString &route,
                                                      std::function<bool(const OperationContext &, const QUuid &)> revoke) {
        return server.route(route, remove,
            [this, revoke](const QString &id, const QHttpServerRequest &request) {
                const std::optional<OperationContext> actor = actorFor(request);
                if (!actor)
                    return unauthorized();
                if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
                    return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
                const std::optional<QUuid> uuid = parseUuid(id);
                if (!uuid || !emptyMutationRequest(request))
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid revocation request"));
                if (!revoke(*actor, *uuid))
                    return jsonError(Status::Conflict, QStringLiteral("Revocation rejected"));
                return QHttpServerResponse(QJsonObject{{QStringLiteral("uuid"), uuidString(*uuid)},
                                                       {QStringLiteral("revoked"), true}});
            });
    };
    if (!registerRevocation(QStringLiteral("/api/v1/admin/memberships/<arg>"),
                            [this](const OperationContext &actor, const QUuid &id) {
        return this->database_.removeGroupMember(actor, id);
    }) ||
        !registerRevocation(QStringLiteral("/api/v1/admin/permission-grants/<arg>"),
                            [this](const OperationContext &actor, const QUuid &id) {
        return this->database_.revokeRolePermission(actor, id);
    }) ||
        !registerRevocation(QStringLiteral("/api/v1/admin/role-assignments/<arg>"),
                            [this](const OperationContext &actor, const QUuid &id) {
        return this->database_.revokeRoleAssignment(actor, id);
    }))
        return false;

    const std::function<bool(const QString &, std::function<bool(const OperationContext &, const QUuid &)>)>
        registerUserState = [this, &server, post](const QString &route,
                                                  std::function<bool(const OperationContext &, const QUuid &)> change) {
        return server.route(route, post,
            [this, change](const QString &id, const QHttpServerRequest &request) {
                const std::optional<OperationContext> actor = actorFor(request);
                if (!actor)
                    return unauthorized();
                if (!this->database_.hasPermission(*actor, QStringLiteral("security.manage"), AccessScope::system()))
                    return jsonError(Status::Forbidden, QStringLiteral("Permission denied"));
                const std::optional<QUuid> uuid = parseUuid(id);
                if (!uuid || !emptyMutationRequest(request))
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid user state request"));
                if (!change(*actor, *uuid))
                    return jsonError(Status::Conflict, QStringLiteral("User state change rejected"));
                return QHttpServerResponse(QJsonObject{{QStringLiteral("uuid"), uuidString(*uuid)}});
            });
    };
    if (!registerUserState(QStringLiteral("/api/v1/admin/users/<arg>/disable"),
                           [this](const OperationContext &actor, const QUuid &id) {
        return this->database_.disableUser(actor, id);
    }) ||
        !registerUserState(QStringLiteral("/api/v1/admin/users/<arg>/enable"),
                           [this](const OperationContext &actor, const QUuid &id) {
        return this->database_.enableUser(actor, id);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/projects"), get, [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        QString error;
        const QVector<ProjectInfo> items = this->database_.projects(*actor, &error);
        if (!error.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Project listing failed"));
        QJsonArray projects;
        for (const ProjectInfo &item : items)
            projects.append(projectJson(item));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("projects"), projects}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/projects"), post, [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QJsonObject> body = jsonBody(request);
        if (!body || !body->value(QStringLiteral("name")).isString())
            return jsonError(Status::BadRequest, QStringLiteral("Expected JSON object with project name"));
        const QString name = body->value(QStringLiteral("name")).toString();
        if (name.trimmed().isEmpty() || name.size() > 256)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid project name"));
        QString error;
        const std::optional<ProjectCreation> result = this->database_.createProject(*actor, name, &error);
        if (!result)
            return jsonError(Status::Forbidden, QStringLiteral("Project creation denied or failed"));
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("project_uuid"), uuidString(result->project_uuid)},
            {QStringLiteral("main_branch_uuid"), uuidString(result->main_branch_uuid)},
            {QStringLiteral("working_revision_uuid"), uuidString(result->working_revision_uuid)}}, Status::Created);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/projects/<arg>"), get,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> uuid = parseUuid(id);
        if (!uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid project UUID"));
        const std::optional<ProjectInfo> result = this->database_.project(*actor, *uuid);
        return result ? QHttpServerResponse(projectJson(*result)) : unavailable();
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/projects/<arg>/branches"), get,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> uuid = parseUuid(id);
        if (!uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid project UUID"));
        // Confirm read access before returning an empty collection.
        if (!this->database_.project(*actor, *uuid))
            return unavailable();
        QString error;
        const QVector<BranchInfo> result = this->database_.branches(*actor, *uuid, &error);
        if (!error.isEmpty())
            return unavailable();
        QJsonArray items;
        for (const BranchInfo &branch : result)
            items.append(branchJson(branch));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("branches"), items}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/projects/<arg>/branches"), post,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> project_uuid = parseUuid(id);
        const std::optional<QJsonObject> body = jsonBody(request);
        if (!project_uuid || request.url().hasQuery() || !body ||
            !exactFields(*body, {"name", "base_finalized_revision_uuid"}))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid branch creation request"));
        const QJsonValue name_value = body->value(QStringLiteral("name"));
        const QJsonValue base_value = body->value(QStringLiteral("base_finalized_revision_uuid"));
        if (!name_value.isString() || !base_value.isString())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid branch creation fields"));
        const QString name = name_value.toString();
        const std::optional<QUuid> base_uuid = parseUuid(base_value.toString());
        if (!base_uuid || name.trimmed().isEmpty() || name.size() > 256)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid branch name or base revision UUID"));
        if (!this->database_.hasPermission(*actor, QStringLiteral("project.branch.create"),
                                           AccessScope::project(*project_uuid)))
            return jsonError(Status::Forbidden, QStringLiteral("Branch creation denied"));
        QString error;
        const std::optional<BranchInfo> result = this->database_.createBranch(
            *actor, *project_uuid, name, *base_uuid, &error);
        if (!result)
            return jsonError(Status::Conflict, QStringLiteral("Branch creation invalid or conflicted"));
        return QHttpServerResponse(branchJson(*result), Status::Created);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/branches/<arg>/revisions"), get,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> uuid = parseUuid(id);
        if (!uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid branch UUID"));
        if (!this->database_.branch(*actor, *uuid))
            return unavailable();
        QString error;
        const QVector<RevisionInfo> result = this->database_.revisions(*actor, *uuid, &error);
        if (!error.isEmpty())
            return unavailable();
        QJsonArray items;
        for (const RevisionInfo &revision : result)
            items.append(revisionJson(revision));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("revisions"), items}});
    }))
        return false;

    // Read-only DAG and three-way merge summaries for remote GUI clients.
    // The core enforces read access to both tips and (for preview) the merge
    // base. All failures are deliberately indistinguishable from unavailable
    // resources, so UUID probes cannot reveal private revision relationships.
    if (!server.route(QStringLiteral("/api/v1/merge/compare"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<RevisionPair> pair = revisionPairQuery(request);
        if (!pair)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid revision comparison query"));
        QString error;
        const std::optional<RevisionComparison> comparison = this->database_.compareRevisions(
            *actor, pair->left, pair->right, &error);
        return comparison ? QHttpServerResponse(comparisonJson(*comparison)) : unavailable();
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/merge/preview"), get,
                      [this](const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<RevisionPair> pair = revisionPairQuery(request);
        if (!pair)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid merge preview query"));
        QString error;
        // Bound retained entity payloads while scanning the SQLite revisions,
        // before building the response. Unchanged entities are streamed.
        const std::optional<ObjectMergePreview> preview = this->database_.previewObjectMergeBounded(
            *actor, pair->left, pair->right, 1024, 16 * 1024 * 1024, &error);
        if (!preview)
        {
            if (error == QStringLiteral("Merge preview exceeds changed-entity limit"))
                return jsonError(Status::Conflict, QStringLiteral("Merge preview exceeds 1024 changed entities"));
            if (error == QStringLiteral("Merge preview exceeds changed-payload limit"))
                return jsonError(Status::Conflict, QStringLiteral("Merge preview exceeds 16 MiB changed payloads"));
            return unavailable();
        }
        // Keep the serialized JSON size limit independent of payload limits.
        if (preview->changes.size() > 1024)
            return jsonError(Status::Conflict, QStringLiteral("Merge preview exceeds 1024 changed entities"));
        QJsonArray changes;
        for (const ObjectMergeChange &change : preview->changes)
        {
            QJsonArray fields;
            for (const QString &field : change.conflicting_fields)
                fields.append(field);
            // This is a summary, NOT an export of entity payloads. Authorized
            // clients can retrieve existing source objects via revision GET.
            const std::optional<ModelObject> &representative = change.left ?
                change.left : (change.right ? change.right : change.base);
            changes.append(QJsonObject{
                {QStringLiteral("object_uuid"), uuidString(change.object_uuid)},
                {QStringLiteral("kind"), representative ? representative->kind : QString()},
                {QStringLiteral("classification"), changeKindName(change.kind)},
                {QStringLiteral("base_present"), change.base.has_value()},
                {QStringLiteral("left_present"), change.left.has_value()},
                {QStringLiteral("right_present"), change.right.has_value()},
                {QStringLiteral("conflicting_fields"), fields}});
        }
        QJsonObject result{
            {QStringLiteral("project_uuid"), uuidString(preview->project_uuid)},
            {QStringLiteral("base_revision_uuid"), uuidString(preview->base_revision_uuid)},
            {QStringLiteral("left_revision_uuid"), uuidString(preview->left_revision_uuid)},
            {QStringLiteral("right_revision_uuid"), uuidString(preview->right_revision_uuid)},
            {QStringLiteral("left_generation"), QString::number(preview->left_generation)},
            {QStringLiteral("right_generation"), QString::number(preview->right_generation)},
            {QStringLiteral("conflict_count"), QString::number(preview->conflict_count)},
            {QStringLiteral("changes"), changes}};
        const QByteArray encoded = QJsonDocument(result).toJson(QJsonDocument::Compact);
        if (encoded.size() > 1024 * 1024)
            return jsonError(Status::Conflict, QStringLiteral("Merge preview exceeds 1 MiB"));
        return QHttpServerResponse(result);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/revisions/<arg>"), get,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> uuid = parseUuid(id);
        if (!uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid revision UUID"));
        const std::optional<RevisionInfo> result = this->database_.revision(*actor, *uuid);
        return result ? QHttpServerResponse(revisionJson(*result)) : unavailable();
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/revisions/<arg>/objects/<arg>"), get,
                      [this](const QString &revision_id, const QString &object_id,
                             const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> revision_uuid = parseUuid(revision_id);
        const std::optional<QUuid> object_uuid = parseUuid(object_id);
        if (!revision_uuid || !object_uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid object or revision UUID"));
        const std::optional<ModelObject> object = this->database_.object(*actor, *revision_uuid, *object_uuid);
        if (!object)
            return unavailable();
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("uuid"), uuidString(object->uuid)},
            {QStringLiteral("kind"), object->kind},
            {QStringLiteral("content_base64"), QString::fromLatin1(object->content.toBase64())},
            {QStringLiteral("version"), QString::number(object->version)},
            {QStringLiteral("deleted"), object->deleted}});
    }))
        return false;

    // The typed network representation contains every nested Model field.
    // Binary encoding is versioned in ModelCodec; HTTP uses canonical base64
    // transport rather than attempting lossy JSON floating-point conversion.
    if (!server.route(QStringLiteral("/api/v1/revisions/<arg>/networks/<arg>"), get,
                      [this](const QString &revision_id, const QString &network_id,
                             const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> revision_uuid = parseUuid(revision_id);
        const std::optional<QUuid> network_uuid = parseUuid(network_id);
        if (!revision_uuid || !network_uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid revision or network UUID"));
        QString error;
        const std::optional<NetworkHydraulic> network = this->database_.loadHydraulicNetwork(
            *actor, *revision_uuid, *network_uuid, &error);
        if (!network)
            return unavailable();
        const QByteArray encoded = ModelCodec<NetworkHydraulic>::encode(*network);
        if (encoded.isEmpty())
            return jsonError(Status::InternalServerError, QStringLiteral("Network encoding failed"));
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("revision_uuid"), uuidString(*revision_uuid)},
            {QStringLiteral("network_uuid"), uuidString(network->uuid)},
            {QStringLiteral("codec"), QStringLiteral("aowis.model.network_hydraulic.v1")},
            {QStringLiteral("content_base64"), QString::fromLatin1(encoded.toBase64())}});
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/branches/<arg>/networks/<arg>"), put,
                      [this](const QString &branch_id, const QString &network_id,
                             const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> branch_uuid = parseUuid(branch_id);
        const std::optional<QUuid> network_uuid = parseUuid(network_id);
        const std::optional<QJsonObject> body = jsonBody(request, 8 * 1024 * 1024);
        if (!branch_uuid || !network_uuid || !body)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid network request"));
        const std::optional<qint64> generation = expectedGeneration(*body);
        if (!generation || body->value(QStringLiteral("codec")).toString() !=
                               QStringLiteral("aowis.model.network_hydraulic.v1"))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid codec or expected_generation"));
        const std::optional<QByteArray> bytes = decodeBase64(body->value(QStringLiteral("content_base64")),
                                                              6 * 1024 * 1024);
        if (!bytes || bytes->isEmpty())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid network content_base64"));
        QString error;
        const std::optional<NetworkHydraulic> network = ModelCodec<NetworkHydraulic>::decode(*bytes, &error);
        if (!network || network->uuid != *network_uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid network data or mismatched UUID"));
        if (!this->database_.saveHydraulicNetwork(*actor, *branch_uuid, *generation, *network, &error))
            return jsonError(Status::Conflict, QStringLiteral("Network edit denied, invalid, or conflicted"));
        QJsonObject result{{QStringLiteral("network_uuid"), uuidString(*network_uuid)}};
        const QJsonObject generation_info = workingGeneration(this->database_, *actor, *branch_uuid);
        for (const QString &key : generation_info.keys())
            result.insert(key, generation_info.value(key));
        return QHttpServerResponse(result);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/branches/<arg>/object-groups"), post,
                      [this](const QString &branch_id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> branch_uuid = parseUuid(branch_id);
        const std::optional<QJsonObject> body = jsonBody(request, 8 * 1024 * 1024);
        if (!branch_uuid || !body)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid group request"));
        const std::optional<qint64> generation = expectedGeneration(*body);
        const QJsonValue edits_value = body->value(QStringLiteral("edits"));
        if (!generation || !edits_value.isArray())
            return jsonError(Status::BadRequest, QStringLiteral("Expected decimal-string generation and edits array"));
        const QJsonArray values = edits_value.toArray();
        if (values.isEmpty() || values.size() > 128)
            return jsonError(Status::BadRequest, QStringLiteral("Expected 1..128 object edits"));
        // Check existing kinds as well as incoming ones. Otherwise a client
        // could rename an indexed hydraulic entity or delete it without
        // passing typed network validation.
        const std::optional<BranchInfo> current_branch = this->database_.branch(*actor, *branch_uuid);
        if (!current_branch)
            return unavailable();
        QVector<ObjectEdit> edits;
        QSet<QUuid> ids;
        qsizetype total_bytes = 0;
        for (const QJsonValue &value : values)
        {
            if (!value.isObject())
                return jsonError(Status::BadRequest, QStringLiteral("Invalid object edit"));
            const QJsonObject entry = value.toObject();
            const std::optional<QUuid> uuid = parseUuid(entry.value(QStringLiteral("uuid")).toString());
            const std::optional<qint64> version = expectedVersion(entry.value(QStringLiteral("expected_version")));
            if (!uuid || !version || ids.contains(*uuid))
                return jsonError(Status::BadRequest, QStringLiteral("Invalid or repeated object UUID/version"));
            ids.insert(*uuid);
            const std::optional<ModelObject> current = this->database_.object(
                *actor, current_branch->head_revision_uuid, *uuid);
            if (current && current->kind.startsWith(QStringLiteral("hydraulic."), Qt::CaseInsensitive))
                return jsonError(Status::BadRequest, QStringLiteral("Use typed hydraulic-network endpoint"));
            const bool deleted = entry.value(QStringLiteral("delete")).toBool(false);
            const QString kind = entry.value(QStringLiteral("kind")).toString();
            // Generic transport must not bypass typed hydraulic codec validation.
            if (kind.startsWith(QStringLiteral("hydraulic."), Qt::CaseInsensitive))
                return jsonError(Status::BadRequest, QStringLiteral("Use typed hydraulic-network endpoint"));
            ObjectEdit edit;
            edit.object_uuid = *uuid;
            edit.expected_version = *version;
            if (deleted)
            {
                if (entry.contains(QStringLiteral("content_base64")) || entry.value(QStringLiteral("delete")) != QJsonValue(true))
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid deletion edit"));
            }
            else
            {
                if (kind.isEmpty() || kind.size() > 128 ||
                    !entry.value(QStringLiteral("delete")).isUndefined())
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid object kind or operation"));
                const std::optional<QByteArray> bytes = decodeBase64(entry.value(QStringLiteral("content_base64")),
                                                                      1024 * 1024);
                if (!bytes)
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid object content_base64"));
                total_bytes += bytes->size();
                if (total_bytes > 6 * 1024 * 1024)
                    return jsonError(Status::BadRequest, QStringLiteral("Object batch exceeds payload limit"));
                edit.kind = kind;
                edit.content = *bytes;
            }
            edits.append(edit);
        }
        QString error;
        const std::optional<QUuid> group_uuid = this->database_.commitObjectGroup(
            *actor, *branch_uuid, *generation, edits, &error);
        if (!group_uuid)
            return jsonError(Status::Conflict, QStringLiteral("Object edit denied, invalid, or conflicted"));
        QJsonObject result{{QStringLiteral("group_uuid"), uuidString(*group_uuid)}};
        const QJsonObject generation_info = workingGeneration(this->database_, *actor, *branch_uuid);
        for (const QString &key : generation_info.keys())
            result.insert(key, generation_info.value(key));
        return QHttpServerResponse(result, Status::Created);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/revisions/<arg>/object-groups"), get,
                      [this](const QString &revision_id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> revision_uuid = parseUuid(revision_id);
        if (!revision_uuid)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid revision UUID"));
        if (!this->database_.revision(*actor, *revision_uuid))
            return unavailable();
        QString error;
        const QVector<ObjectGroup> groups = this->database_.objectGroupsForRevision(*actor, *revision_uuid, &error);
        if (!error.isEmpty())
            return unavailable();
        QJsonArray items;
        for (const ObjectGroup &group : groups)
            items.append(objectGroupJson(group));
        return QHttpServerResponse(QJsonObject{{QStringLiteral("groups"), items}});
    }))
        return false;

    const std::function<bool(const QString &, bool)> registerCompensation =
        [this, &server, post](const QString &route, bool redo) {
        return server.route(route, post,
            [this, redo](const QString &branch_id, const QHttpServerRequest &request) {
                const std::optional<OperationContext> actor = actorFor(request);
                if (!actor)
                    return unauthorized();
                const std::optional<QUuid> branch_uuid = parseUuid(branch_id);
                const std::optional<QJsonObject> body = jsonBody(request);
                if (!branch_uuid || !body)
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid request"));
                const std::optional<qint64> generation = expectedGeneration(*body);
                if (!generation)
                    return jsonError(Status::BadRequest, QStringLiteral("Invalid expected_generation"));
                QString error;
                const std::optional<QUuid> group = redo
                    ? this->database_.redoObjectGroup(*actor, *branch_uuid, *generation, &error)
                    : this->database_.undoObjectGroup(*actor, *branch_uuid, *generation, &error);
                if (!group)
                    return jsonError(Status::Conflict, QStringLiteral("Undo/redo denied or conflicted"));
                QJsonObject result{{QStringLiteral("target_group_uuid"), uuidString(*group)}};
                const QJsonObject generation_info = workingGeneration(this->database_, *actor, *branch_uuid);
                for (const QString &key : generation_info.keys())
                    result.insert(key, generation_info.value(key));
                return QHttpServerResponse(result);
            });
    };
    if (!registerCompensation(QStringLiteral("/api/v1/branches/<arg>/undo"), false) ||
        !registerCompensation(QStringLiteral("/api/v1/branches/<arg>/redo"), true))
        return false;

    // Commit a two-parent merge through the authoritative transactional
    // core. Never trust a previous preview: UUID, generation, permissions and
    // physical object state are rechecked by mergeObjectRevisions().
    if (!server.route(QStringLiteral("/api/v1/branches/<arg>/merge"), post,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> branch_uuid = parseUuid(id);
        const std::optional<QJsonObject> body = jsonBody(request);
        if (!branch_uuid || request.url().hasQuery() || !body ||
            !exactFields(*body, {"expected_working_revision_uuid", "expected_generation",
                                 "right_finalized_revision_uuid", "resolutions", "title", "message"}))
            return jsonError(Status::BadRequest, QStringLiteral("Invalid merge request"));
        const QJsonValue head_value = body->value(QStringLiteral("expected_working_revision_uuid"));
        const QJsonValue remote_value = body->value(QStringLiteral("right_finalized_revision_uuid"));
        const QJsonValue title_value = body->value(QStringLiteral("title"));
        const QJsonValue message_value = body->value(QStringLiteral("message"));
        if (!head_value.isString() || !remote_value.isString() ||
            !title_value.isString() || !message_value.isString())
            return jsonError(Status::BadRequest, QStringLiteral("Invalid merge request fields"));
        const std::optional<QUuid> working_uuid = parseUuid(head_value.toString());
        const std::optional<QUuid> right_uuid = parseUuid(remote_value.toString());
        const std::optional<qint64> generation = expectedGeneration(*body);
        const std::optional<QVector<ObjectMergeResolution>> resolutions =
            mergeResolutions(body->value(QStringLiteral("resolutions")));
        const QString title = title_value.toString();
        const QString message = message_value.toString();
        if (!working_uuid || !right_uuid || !generation || !resolutions ||
            title.trimmed().isEmpty() || title.size() > 256 || message.size() > 4096)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid merge request fields"));
        QString error;
        const std::optional<Finalization> result = this->database_.mergeObjectRevisions(
            *actor, *branch_uuid, *working_uuid, *generation, *right_uuid,
            *resolutions, title, message, &error);
        if (!result)
            return jsonError(Status::Conflict, QStringLiteral("Merge denied, stale, or conflicted"));
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("finalized_revision_uuid"), uuidString(result->finalized_revision_uuid)},
            {QStringLiteral("new_working_revision_uuid"), uuidString(result->new_working_revision_uuid)},
            {QStringLiteral("snapshot_sha256"), QString::fromLatin1(result->snapshot_sha256.toHex())},
            {QStringLiteral("chain_sha256"), QString::fromLatin1(result->chain_sha256.toHex())}},
            Status::Created);
    }))
        return false;

    if (!server.route(QStringLiteral("/api/v1/branches/<arg>/finalize"), post,
                      [this](const QString &id, const QHttpServerRequest &request) {
        const std::optional<OperationContext> actor = actorFor(request);
        if (!actor)
            return unauthorized();
        const std::optional<QUuid> uuid = parseUuid(id);
        const std::optional<QJsonObject> body = jsonBody(request);
        if (!uuid || !body)
            return jsonError(Status::BadRequest, QStringLiteral("Invalid request"));
        const std::optional<qint64> generation = expectedGeneration(*body);
        if (!generation || !body->value(QStringLiteral("title")).isString())
            return jsonError(Status::BadRequest, QStringLiteral("Expected title and decimal-string expected_generation"));
        QString error;
        const std::optional<Finalization> result = this->database_.finalize(
            *actor, *uuid, *generation, body->value(QStringLiteral("title")).toString(),
            body->value(QStringLiteral("message")).toString(), &error);
        if (!result)
            return jsonError(Status::Conflict, QStringLiteral("Revision finalization denied or conflicted"));
        return QHttpServerResponse(QJsonObject{
            {QStringLiteral("finalized_revision_uuid"), uuidString(result->finalized_revision_uuid)},
            {QStringLiteral("new_working_revision_uuid"), uuidString(result->new_working_revision_uuid)},
            {QStringLiteral("snapshot_sha256"), QString::fromLatin1(result->snapshot_sha256.toHex())},
            {QStringLiteral("chain_sha256"), QString::fromLatin1(result->chain_sha256.toHex())}});
    }))
        return false;

    return true;
}

} // namespace aowis::db
