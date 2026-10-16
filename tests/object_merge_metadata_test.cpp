#include <aowis/db/database.h>
#include <aowis/db/model_codec.h>
#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QDate>
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
    HydraulicNodeJunction value;
    value.uuid = uuid;
    value.id = id;
    value.elevation_m = 5.0;
    value.metadata.description = QStringLiteral("baseline");
    value.metadata.comment = QStringLiteral("original note");
    value.metadata.tags = {QStringLiteral("original")};
    return value;
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
    const QString path = directory.filePath(QStringLiteral("metadata-merge.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(
        QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        actor, QStringLiteral("nested metadata merge"), &error);
    REQUIRE(project);

    const QString kind = QStringLiteral("hydraulic.junction");
    const QUuid automatic_uuid = createUuidV7();
    const QUuid conflicting_uuid = createUuidV7();
    const QUuid local_uuid = createUuidV7();
    const HydraulicNodeJunction base_automatic = junction(automatic_uuid, QStringLiteral("auto"));
    const HydraulicNodeJunction base_conflicting = junction(conflicting_uuid, QStringLiteral("conflict"));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, automatic_uuid, kind,
        ModelCodec<HydraulicNodeJunction>::encode(base_automatic), -1, &error));
    REQUIRE(db.putObject(actor, project->main_branch_uuid, conflicting_uuid, kind,
        ModelCodec<HydraulicNodeJunction>::encode(base_conflicting), -1, &error));
    const std::optional<aowis::db::Finalization> baseline = db.finalize(
        actor, project->main_branch_uuid, 2, QStringLiteral("baseline"), {}, &error);
    REQUIRE(baseline);
    const std::optional<aowis::db::BranchInfo> left = db.createBranch(
        actor, project->project_uuid, QStringLiteral("left"), baseline->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right = db.createBranch(
        actor, project->project_uuid, QStringLiteral("right"), baseline->finalized_revision_uuid, &error);
    REQUIRE(left && right);

    HydraulicNodeJunction left_auto = base_automatic;
    left_auto.metadata.description = QStringLiteral("new description");
    HydraulicNodeJunction right_auto = base_automatic;
    right_auto.metadata.comment = QStringLiteral("new comment");
    HydraulicNodeJunction left_conflict = base_conflicting;
    left_conflict.metadata.enabled = false;
    left_conflict.metadata.date_installed = QDate(2025, 5, 7);
    left_conflict.metadata.description = QStringLiteral("local description");
    left_conflict.metadata.tags = {QStringLiteral("left")};
    HydraulicNodeJunction right_conflict = base_conflicting;
    right_conflict.metadata.model_role = EntityModelRole::PlannedAsset;
    right_conflict.metadata.date_added = QDate(2026, 2, 9);
    right_conflict.metadata.description = QStringLiteral("remote description");
    right_conflict.metadata.comment = QStringLiteral("remote note");
    right_conflict.metadata.tags = {QStringLiteral("right")};

    for (bool use_left : {true, false})
    {
        const QUuid branch_uuid = use_left ? left->uuid : right->uuid;
        REQUIRE(db.putObject(actor, branch_uuid, automatic_uuid, kind,
            ModelCodec<HydraulicNodeJunction>::encode(use_left ? left_auto : right_auto), 1, &error));
        REQUIRE(db.putObject(actor, branch_uuid, conflicting_uuid, kind,
            ModelCodec<HydraulicNodeJunction>::encode(use_left ? left_conflict : right_conflict), 1, &error));
    }
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        actor, left->uuid, 2, QStringLiteral("left"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        actor, right->uuid, 2, QStringLiteral("right"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid working = left_final->new_working_revision_uuid;
    const HydraulicNodeJunction local = junction(local_uuid, QStringLiteral("local-only"));
    REQUIRE(db.putObject(actor, left->uuid, local_uuid, kind,
        ModelCodec<HydraulicNodeJunction>::encode(local), -1, &error));

    const std::optional<ObjectMergePreview> preview = db.previewObjectMerge(
        actor, working, right_final->finalized_revision_uuid, &error);
    REQUIRE(preview);
    REQUIRE(preview->conflict_count == 1);
    REQUIRE(preview->changes.size() == 3);
    const ObjectMergeChange *automatic = findChange(*preview, automatic_uuid);
    REQUIRE(automatic && automatic->kind == ObjectMergeKind::FieldsMerged);
    REQUIRE(automatic->automatically_merged);
    const std::optional<HydraulicNodeJunction> expected_auto = ModelCodec<HydraulicNodeJunction>::decode(
        automatic->automatically_merged->content, &error);
    REQUIRE(expected_auto);
    REQUIRE(expected_auto->metadata.description == QStringLiteral("new description"));
    REQUIRE(expected_auto->metadata.comment == QStringLiteral("new comment"));
    REQUIRE(expected_auto->metadata.tags == QStringList{QStringLiteral("original")});
    const ObjectMergeChange *conflicting = findChange(*preview, conflicting_uuid);
    REQUIRE(conflicting && conflicting->kind == ObjectMergeKind::Conflict);
    REQUIRE(conflicting->conflicting_fields ==
        (QStringList{QStringLiteral("metadata.description"), QStringLiteral("metadata.tags")}));

    const ObjectMergeResolution resolve = fieldChoice(conflicting_uuid, {
        {QStringLiteral("metadata.description"), ObjectMergeSide::Left},
        {QStringLiteral("metadata.tags"), ObjectMergeSide::Right}});
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {fieldChoice(conflicting_uuid, {{QStringLiteral("metadata"), ObjectMergeSide::Right},
                                         {QStringLiteral("metadata.tags"), ObjectMergeSide::Right}})},
        QStringLiteral("legacy field name"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {fieldChoice(conflicting_uuid, {{QStringLiteral("metadata.description"), ObjectMergeSide::Left}})},
        QStringLiteral("incomplete fields"), {}, &error));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid,
        {fieldChoice(conflicting_uuid, {{QStringLiteral("metadata.description"), ObjectMergeSide::Left},
                                         {QStringLiteral("metadata.description"), ObjectMergeSide::Right}})},
        QStringLiteral("duplicate fields"), {}, &error));
    REQUIRE(db.revision(actor, working, &error)->generation == 1);

    // A late parent-insertion failure must undo synthetic nested-field writes,
    // their object journal group and any finalization metadata as one unit.
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("metadata-merge-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_metadata_merge BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'rollback'); END")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("metadata-merge-fixture"));
    REQUIRE(!db.mergeObjectRevisions(actor, left->uuid, working, 1,
        right_final->finalized_revision_uuid, {resolve}, QStringLiteral("rollback"), {}, &error));
    REQUIRE(db.revision(actor, working, &error)->generation == 1);
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 1);
    REQUIRE(db.object(actor, working, conflicting_uuid, &error)->content ==
        ModelCodec<HydraulicNodeJunction>::encode(left_conflict));
    REQUIRE(db.object(actor, working, local_uuid, &error)->content ==
        ModelCodec<HydraulicNodeJunction>::encode(local));
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("metadata-merge-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_metadata_merge")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("metadata-merge-fixture"));

    const std::optional<aowis::db::Finalization> merged = db.mergeObjectRevisions(
        actor, left->uuid, working, 1, right_final->finalized_revision_uuid, {resolve},
        QStringLiteral("nested metadata merge"), {}, &error);
    REQUIRE(merged && merged->finalized_revision_uuid == working);
    REQUIRE(db.object(actor, working, automatic_uuid, &error)->content ==
        automatic->automatically_merged->content);
    const std::optional<HydraulicNodeJunction> combined = ModelCodec<HydraulicNodeJunction>::decode(
        db.object(actor, working, conflicting_uuid, &error)->content, &error);
    REQUIRE(combined);
    REQUIRE(!combined->metadata.enabled);
    REQUIRE(combined->metadata.model_role == EntityModelRole::PlannedAsset);
    REQUIRE(combined->metadata.date_added == std::optional<QDate>(QDate(2026, 2, 9)));
    REQUIRE(combined->metadata.date_installed == std::optional<QDate>(QDate(2025, 5, 7)));
    REQUIRE(combined->metadata.description == QStringLiteral("local description"));
    REQUIRE(combined->metadata.comment == QStringLiteral("remote note"));
    REQUIRE(combined->metadata.tags == QStringList{QStringLiteral("right")});
    REQUIRE(db.object(actor, working, local_uuid, &error)->content ==
        ModelCodec<HydraulicNodeJunction>::encode(local));
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).size() == 2);
    REQUIRE(db.revision(actor, working, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
