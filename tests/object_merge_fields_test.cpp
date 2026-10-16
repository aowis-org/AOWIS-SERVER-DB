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

static HydraulicNodeJunction junction(const QUuid &uuid, const QString &id, double elevation)
{
    HydraulicNodeJunction value;
    value.uuid = uuid;
    value.id = id;
    value.elevation_m = elevation;
    return value;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("field-merge.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        actor, QStringLiteral("field merge"), &error);
    REQUIRE(project);

    const QString typed_kind = QStringLiteral("hydraulic.junction");
    const QString opaque_kind = QStringLiteral("hydraulic.label");
    const QUuid independent = createUuidV7();
    const QUuid overlap = createUuidV7();
    const QUuid nested = createUuidV7();
    const QUuid opaque = createUuidV7();
    const QUuid local_extra = createUuidV7();

    const HydraulicNodeJunction common_independent = junction(independent, QStringLiteral("J1"), 5.0);
    const HydraulicNodeJunction common_overlap = junction(overlap, QStringLiteral("J2"), 5.0);
    const HydraulicNodeJunction common_nested = junction(nested, QStringLiteral("J3"), 5.0);
    REQUIRE(db.putObject(actor, project->main_branch_uuid, independent, typed_kind,
                         ModelCodec<HydraulicNodeJunction>::encode(common_independent), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, overlap, typed_kind,
                         ModelCodec<HydraulicNodeJunction>::encode(common_overlap), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, nested, typed_kind,
                         ModelCodec<HydraulicNodeJunction>::encode(common_nested), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, opaque, opaque_kind,
                         QByteArray("base"), -1, &error));
    const std::optional<aowis::db::Finalization> baseline = db.finalize(
        actor, project->main_branch_uuid, 4, QStringLiteral("base"), {}, &error);
    REQUIRE(baseline);
    const std::optional<aowis::db::BranchInfo> left = db.createBranch(actor,
        project->project_uuid, QStringLiteral("left"), baseline->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(actor,
        project->project_uuid, QStringLiteral("right"), baseline->finalized_revision_uuid, &error);
    REQUIRE(left && right);

    HydraulicNodeJunction left_independent = common_independent;
    left_independent.elevation_m = 10.0;
    HydraulicNodeJunction right_independent = common_independent;
    right_independent.id = QStringLiteral("J1-remote");
    HydraulicNodeJunction left_overlap = common_overlap;
    left_overlap.elevation_m = 11.0;
    HydraulicNodeJunction right_overlap = common_overlap;
    right_overlap.elevation_m = 12.0;
    right_overlap.id = QStringLiteral("J2-remote");
    HydraulicNodeJunction left_nested = common_nested;
    left_nested.metadata.description = QStringLiteral("left");
    HydraulicNodeJunction right_nested = common_nested;
    right_nested.metadata.comment = QStringLiteral("right");

    for (bool use_left : {true, false})
    {
        const QUuid branch = use_left ? left->uuid : right->uuid;
        const HydraulicNodeJunction &first = use_left ? left_independent : right_independent;
        const HydraulicNodeJunction &second = use_left ? left_overlap : right_overlap;
        const HydraulicNodeJunction &third = use_left ? left_nested : right_nested;
        REQUIRE(db.putObject(actor, branch, independent, typed_kind,
                             ModelCodec<HydraulicNodeJunction>::encode(first), 1, &error));
        REQUIRE(db.putObject(actor, branch, overlap, typed_kind,
                             ModelCodec<HydraulicNodeJunction>::encode(second), 1, &error));
        REQUIRE(db.putObject(actor, branch, nested, typed_kind,
                             ModelCodec<HydraulicNodeJunction>::encode(third), 1, &error));
        REQUIRE(db.putObject(actor, branch, opaque, opaque_kind,
                             use_left ? QByteArray("local") : QByteArray("remote"), 1, &error));
    }
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        actor, left->uuid, 4, QStringLiteral("left"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        actor, right->uuid, 4, QStringLiteral("right"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid working = left_final->new_working_revision_uuid;
    REQUIRE(db.putObject(actor, left->uuid, local_extra, opaque_kind,
                         QByteArray("local-work"), -1, &error));
    const std::optional<ObjectMergePreview> preview = db.previewObjectMerge(
        actor, working, right_final->finalized_revision_uuid, &error);
    REQUIRE(preview);
    REQUIRE(preview->left_generation == 1);
    REQUIRE(preview->conflict_count == 2);
    REQUIRE(preview->changes.size() == 5);

    const ObjectMergeChange *automatic = findChange(*preview, independent);
    REQUIRE(automatic && automatic->kind == ObjectMergeKind::FieldsMerged);
    REQUIRE(automatic->automatically_merged);
    const std::optional<HydraulicNodeJunction> combined = ModelCodec<HydraulicNodeJunction>::decode(
        automatic->automatically_merged->content, &error);
    REQUIRE(combined);
    REQUIRE(combined->uuid == independent);
    REQUIRE(combined->elevation_m == 10.0);
    REQUIRE(combined->id == QStringLiteral("J1-remote"));

    const ObjectMergeChange *overlap_change = findChange(*preview, overlap);
    REQUIRE(overlap_change && overlap_change->kind == ObjectMergeKind::Conflict);
    REQUIRE(overlap_change->conflicting_fields == QStringList{QStringLiteral("elevation_m")});
    REQUIRE(!overlap_change->automatically_merged);
    const ObjectMergeChange *nested_change = findChange(*preview, nested);
    REQUIRE(nested_change && nested_change->kind == ObjectMergeKind::FieldsMerged);
    REQUIRE(nested_change->conflicting_fields.isEmpty());
    REQUIRE(nested_change->automatically_merged);
    const std::optional<HydraulicNodeJunction> combined_metadata = ModelCodec<HydraulicNodeJunction>::decode(
        nested_change->automatically_merged->content, &error);
    REQUIRE(combined_metadata);
    REQUIRE(combined_metadata->metadata.description == QStringLiteral("left"));
    REQUIRE(combined_metadata->metadata.comment == QStringLiteral("right"));
    const ObjectMergeChange *opaque_change = findChange(*preview, opaque);
    REQUIRE(opaque_change && opaque_change->kind == ObjectMergeKind::Conflict);
    REQUIRE(opaque_change->conflicting_fields.isEmpty()); // Unsupported payload: entity resolution.
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid, {}, QStringLiteral("unresolved"), {}, &error));

    const QVector<ObjectMergeResolution> choices{{overlap, ObjectMergeSide::Right},
        {opaque, ObjectMergeSide::Left}};
    // Force a late failure: automatic field synthesis and earlier writes must
    // also roll back, leaving prior local changes and history untouched.
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("merge-fields-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_field_merge BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'force rollback'); END")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge-fields-fixture"));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid, choices, QStringLiteral("rollback"), {}, &error));
    REQUIRE(db.revision(actor, working, &error)->generation == 1);
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 1);
    REQUIRE(db.object(actor, working, independent, &error)->content ==
            ModelCodec<HydraulicNodeJunction>::encode(left_independent));
    REQUIRE(db.object(actor, working, local_extra, &error)->content == QByteArray("local-work"));
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("merge-fields-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_field_merge")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge-fields-fixture"));

    const std::optional<aowis::db::Finalization> merged = db.mergeObjectRevisions(
        actor, left->uuid, working, 1, right_final->finalized_revision_uuid,
        choices, QStringLiteral("merged fields"), {}, &error);
    REQUIRE(merged);
    REQUIRE(merged->finalized_revision_uuid == working);
    const std::optional<HydraulicNodeJunction> final_independent = ModelCodec<HydraulicNodeJunction>::decode(
        db.object(actor, working, independent, &error)->content, &error);
    REQUIRE(final_independent);
    REQUIRE(final_independent->elevation_m == 10.0);
    REQUIRE(final_independent->id == QStringLiteral("J1-remote"));
    REQUIRE(db.object(actor, working, overlap, &error)->content ==
            ModelCodec<HydraulicNodeJunction>::encode(right_overlap));
    const std::optional<HydraulicNodeJunction> final_nested = ModelCodec<HydraulicNodeJunction>::decode(
        db.object(actor, working, nested, &error)->content, &error);
    REQUIRE(final_nested);
    REQUIRE(final_nested->metadata.description == QStringLiteral("left"));
    REQUIRE(final_nested->metadata.comment == QStringLiteral("right"));
    REQUIRE(db.object(actor, working, local_extra, &error)->content == QByteArray("local-work"));
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 2);
    REQUIRE(db.revision(actor, working, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
