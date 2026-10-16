#include <aowis/db/database.h>
#include <aowis/db/model_codec.h>
#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ModelCodec;
using aowis::db::ObjectMergeChange;
using aowis::db::ObjectMergeFieldResolution;
using aowis::db::ObjectMergeKind;
using aowis::db::ObjectMergePreview;
using aowis::db::ObjectMergeResolution;
using aowis::db::ObjectMergeSide;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

static const ObjectMergeChange *findChange(const ObjectMergePreview &preview, const QUuid &uuid)
{
    for (const ObjectMergeChange &change : preview.changes)
        if (change.object_uuid == uuid)
            return &change;
    return nullptr;
}

static HydraulicNodeJunction junction(const QUuid &uuid, const QString &id)
{
    HydraulicNodeJunction result;
    result.uuid = uuid;
    result.id = id;
    result.elevation_m = 5.0;
    return result;
}

static ObjectMergeResolution fieldChoice(const QUuid &uuid,
                                         const QVector<ObjectMergeFieldResolution> &fields)
{
    ObjectMergeResolution result;
    result.object_uuid = uuid;
    result.field_resolutions = fields;
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("field-resolutions.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(
        QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        actor, QStringLiteral("per-field resolutions"), &error);
    REQUIRE(project);

    const QString typed = QStringLiteral("hydraulic.junction");
    const QString opaque = QStringLiteral("hydraulic.label");
    const QUuid first = createUuidV7();
    const QUuid second = createUuidV7();
    const QUuid fallback = createUuidV7();
    const QUuid local_only = createUuidV7();
    const HydraulicNodeJunction common_first = junction(first, QStringLiteral("A"));
    const HydraulicNodeJunction common_second = junction(second, QStringLiteral("B"));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, first, typed,
        ModelCodec<HydraulicNodeJunction>::encode(common_first), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, second, typed,
        ModelCodec<HydraulicNodeJunction>::encode(common_second), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, fallback, opaque,
        QByteArray("original"), -1, &error));
    const std::optional<aowis::db::Finalization> baseline = db.finalize(
        actor, project->main_branch_uuid, 3, QStringLiteral("baseline"), {}, &error);
    REQUIRE(baseline);
    const std::optional<aowis::db::BranchInfo> left = db.createBranch(
        actor, project->project_uuid, QStringLiteral("left"), baseline->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(
        actor, project->project_uuid, QStringLiteral("right"), baseline->finalized_revision_uuid, &error);
    REQUIRE(left && right);

    HydraulicNodeJunction left_first = common_first;
    left_first.id = QStringLiteral("A-local");
    left_first.elevation_m = 10.0;
    left_first.terrain_elevation_m = 42.0; // independent left edit
    left_first.metadata.description = QStringLiteral("local metadata");
    HydraulicNodeJunction right_first = common_first;
    right_first.id = QStringLiteral("A-remote");
    right_first.elevation_m = 12.0;
    right_first.initial_water_age_h = 3.0; // independent right edit
    right_first.metadata.comment = QStringLiteral("remote metadata");
    right_first.metadata.description = QStringLiteral("remote metadata");

    HydraulicNodeJunction left_second = common_second;
    left_second.elevation_m = 100.0;
    HydraulicNodeJunction right_second = common_second;
    right_second.elevation_m = 200.0;
    for (bool use_left : {true, false})
    {
        const QUuid branch_uuid = use_left ? left->uuid : right->uuid;
        REQUIRE(db.putObject(actor, branch_uuid, first, typed,
            ModelCodec<HydraulicNodeJunction>::encode(use_left ? left_first : right_first), 1, &error));
        REQUIRE(db.putObject(actor, branch_uuid, second, typed,
            ModelCodec<HydraulicNodeJunction>::encode(use_left ? left_second : right_second), 1, &error));
        REQUIRE(db.putObject(actor, branch_uuid, fallback, opaque,
            use_left ? QByteArray("local opaque") : QByteArray("remote opaque"), 1, &error));
    }
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        actor, left->uuid, 3, QStringLiteral("left"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        actor, right->uuid, 3, QStringLiteral("right"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid working = left_final->new_working_revision_uuid;
    REQUIRE(db.putObject(actor, left->uuid, local_only, opaque,
        QByteArray("unfinished local work"), -1, &error));

    const std::optional<ObjectMergePreview> preview = db.previewObjectMerge(
        actor, working, right_final->finalized_revision_uuid, &error);
    REQUIRE(preview);
    REQUIRE(preview->left_generation == 1);
    REQUIRE(preview->conflict_count == 3);
    const ObjectMergeChange *first_change = findChange(*preview, first);
    const ObjectMergeChange *second_change = findChange(*preview, second);
    const ObjectMergeChange *opaque_change = findChange(*preview, fallback);
    REQUIRE(first_change && first_change->kind == ObjectMergeKind::Conflict);
    REQUIRE(first_change->conflicting_fields ==
        (QStringList{QStringLiteral("id"), QStringLiteral("elevation_m"), QStringLiteral("metadata.description")}));
    REQUIRE(second_change && second_change->conflicting_fields ==
        QStringList{QStringLiteral("elevation_m")});
    REQUIRE(opaque_change && opaque_change->conflicting_fields.isEmpty());

    const ObjectMergeResolution resolved_first = fieldChoice(first, {
        {QStringLiteral("id"), ObjectMergeSide::Right},
        {QStringLiteral("elevation_m"), ObjectMergeSide::Left},
        {QStringLiteral("metadata.description"), ObjectMergeSide::Right}});
    const ObjectMergeResolution resolved_second = fieldChoice(second, {
        {QStringLiteral("elevation_m"), ObjectMergeSide::Right}});
    const ObjectMergeResolution resolved_opaque{fallback, ObjectMergeSide::Left};
    const QVector<ObjectMergeResolution> choices{resolved_first, resolved_second, resolved_opaque};

    // Fail closed on missing, duplicate, unexpected or unsupported field names.
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid, {resolved_first, resolved_second},
        QStringLiteral("missing opaque choice"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {fieldChoice(first, {{QStringLiteral("id"), ObjectMergeSide::Left}}),
         resolved_second, resolved_opaque}, QStringLiteral("missing field"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {fieldChoice(first, {{QStringLiteral("id"), ObjectMergeSide::Left},
                             {QStringLiteral("id"), ObjectMergeSide::Right},
                             {QStringLiteral("metadata.description"), ObjectMergeSide::Left}}),
         resolved_second, resolved_opaque}, QStringLiteral("duplicate field"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {fieldChoice(first, {{QStringLiteral("id"), ObjectMergeSide::Left},
                             {QStringLiteral("elevation_m"), ObjectMergeSide::Right},
                             {QStringLiteral("fake_field"), ObjectMergeSide::Left}}),
         resolved_second, resolved_opaque}, QStringLiteral("unknown field"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {resolved_first, resolved_second,
         fieldChoice(fallback, {{QStringLiteral("arbitrary"), ObjectMergeSide::Right}})},
        QStringLiteral("opaque field"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 0,
        right_final->finalized_revision_uuid, choices, QStringLiteral("stale"), {}, &error));
    REQUIRE(db.revision(actor, working, &error)->generation == 1);
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 1);

    // Fail *after* writing merged entity bytes. Neither those edits nor the
    // new journal group or parent edge may survive transaction rollback.
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("field-resolution-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_field_resolution BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'late rollback'); END")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("field-resolution-fixture"));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid, choices, QStringLiteral("rollback"), {}, &error));
    REQUIRE(db.revision(actor, working, &error)->generation == 1);
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 1);
    REQUIRE(db.object(actor, working, first, &error)->content ==
        ModelCodec<HydraulicNodeJunction>::encode(left_first));
    REQUIRE(db.object(actor, working, local_only, &error)->content ==
        QByteArray("unfinished local work"));
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("field-resolution-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_field_resolution")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("field-resolution-fixture"));

    const std::optional<aowis::db::Finalization> merged = db.mergeObjectRevisions(
        actor, left->uuid, working, 1, right_final->finalized_revision_uuid, choices,
        QStringLiteral("per-field merge"), QStringLiteral("preserve independent edits"), &error);
    REQUIRE(merged);
    REQUIRE(merged->finalized_revision_uuid == working);
    const std::optional<HydraulicNodeJunction> combined = ModelCodec<HydraulicNodeJunction>::decode(
        db.object(actor, working, first, &error)->content, &error);
    REQUIRE(combined);
    REQUIRE(combined->id == QStringLiteral("A-remote"));
    REQUIRE(combined->elevation_m == 10.0);
    REQUIRE(combined->terrain_elevation_m == 42.0);
    REQUIRE(combined->initial_water_age_h == 3.0);
    REQUIRE(combined->metadata.description == QStringLiteral("remote metadata"));
    REQUIRE(combined->metadata.comment == QStringLiteral("remote metadata"));
    REQUIRE(db.object(actor, working, second, &error)->content ==
        ModelCodec<HydraulicNodeJunction>::encode(right_second));
    REQUIRE(db.object(actor, working, fallback, &error)->content == QByteArray("local opaque"));
    REQUIRE(db.object(actor, working, local_only, &error)->content ==
        QByteArray("unfinished local work"));
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 2);
    REQUIRE(db.revision(actor, working, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
