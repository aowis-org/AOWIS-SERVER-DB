#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ModelCodec;
using aowis::db::ObjectEdit;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, \
                #condition, error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database db;
    REQUIRE(db.openSqlite(directory.filePath(QStringLiteral("incremental-groups.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Incremental group validation"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    HydraulicNodeJunction other;
    other.uuid = createUuidV7();
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = junction.uuid;
    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_allocation_mode =
        HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = junction.uuid;
    point.attachment.pipe_position = 0.5;
    network.nodes_reservoirs.append(reservoir);
    network.nodes_junctions.append(junction);
    network.nodes_junctions.append(other);
    network.links_pipes.append(pipe);
    network.demand_points.append(point);
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));
    qint64 generation = db.revision(actor, revision, &error)->generation;

    // Several existing entities are edited in one group, without changing
    // their UUID references. Demand point position changes are locally
    // checked for finiteness and bounds, not merely compared as opaque data.
    HydraulicLinkPipe widened = pipe;
    widened.diameter_mm = 280.5;
    HydraulicNodeJunction raised = junction;
    raised.elevation_m = 45.25;
    HydraulicDemandPoint moved = point;
    moved.attachment.pipe_position = 0.875;
    const QVector<ObjectEdit> scalars{
        {pipe.uuid, QStringLiteral("hydraulic.pipe"),
         ModelCodec<HydraulicLinkPipe>::encode(widened), 1},
        {junction.uuid, QStringLiteral("hydraulic.junction"),
         ModelCodec<HydraulicNodeJunction>::encode(raised), 1},
        {point.uuid, QStringLiteral("hydraulic.demand_point"),
         ModelCodec<HydraulicDemandPoint>::encode(moved), 1}
    };
    const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, generation, scalars, &error);
    REQUIRE(group.has_value());
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.object(actor, revision, pipe.uuid, &error)->version == 2);
    REQUIRE(db.loadHydraulicNetwork(actor, revision, network.uuid, &error)
        ->demand_points.first().attachment.pipe_position == 0.875);
    REQUIRE(!db.commitObjectGroup(actor, branch, generation - 1, scalars, &error));

    // Persisted grouped inverse/reapplication must preserve graph validity.
    REQUIRE(db.undoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.loadHydraulicNetwork(actor, revision, network.uuid, &error)
        ->demand_points.first().attachment.pipe_position == 0.5);
    REQUIRE(db.redoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.loadHydraulicNetwork(actor, revision, network.uuid, &error)
        ->links_pipes.first().diameter_mm == 280.5);

    const qsizetype history = db.objectHistoryForRevision(actor, revision, &error).size();
    const qsizetype groups = db.objectGroupsForRevision(actor, revision, &error).size();
    const qint64 point_version = db.object(actor, revision, point.uuid, &error)->version;
    const qint64 pipe_version = db.object(actor, revision, pipe.uuid, &error)->version;

    // An invalid constraint in an otherwise reference-neutral batch must
    // still reject the WHOLE transaction, including its immutable journal.
    HydraulicDemandPoint invalid_point = moved;
    invalid_point.attachment.pipe_position = 1.5;
    REQUIRE(!db.commitObjectGroup(actor, branch, generation,
        {{point.uuid, QStringLiteral("hydraulic.demand_point"),
          ModelCodec<HydraulicDemandPoint>::encode(invalid_point), point_version}}, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history);
    REQUIRE(db.objectGroupsForRevision(actor, revision, &error).size() == groups);

    // Endpoint changes require the conservative full-network path. The
    // unchanged demand-point assignment makes this edit inconsistent.
    HydraulicLinkPipe redirected = widened;
    redirected.node_uuid_to = other.uuid;
    REQUIRE(!db.commitObjectGroup(actor, branch, generation,
        {{pipe.uuid, QStringLiteral("hydraulic.pipe"),
          ModelCodec<HydraulicLinkPipe>::encode(redirected), pipe_version}}, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);

    HydraulicDemandPoint reassigned = moved;
    reassigned.attachment.pipe_assigned_junction_uuid = other.uuid;
    const std::optional<QUuid> moved_group = db.commitObjectGroup(actor, branch, generation,
        {{pipe.uuid, QStringLiteral("hydraulic.pipe"),
          ModelCodec<HydraulicLinkPipe>::encode(redirected), pipe_version},
         {point.uuid, QStringLiteral("hydraulic.demand_point"),
          ModelCodec<HydraulicDemandPoint>::encode(reassigned), point_version}}, &error);
    REQUIRE(moved_group.has_value());
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.undoObjectGroup(actor, branch, generation, &error) == moved_group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.redoObjectGroup(actor, branch, generation, &error) == moved_group);
    generation = db.revision(actor, revision, &error)->generation;
    const std::optional<NetworkHydraulic> final_state =
        db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(final_state.has_value());
    REQUIRE(aowis::db::validateHydraulicNetwork(*final_state).valid());
    REQUIRE(final_state->links_pipes.first().node_uuid_to == other.uuid);

    // Malformed existing object bytes cannot sneak through the shortcut.
    REQUIRE(!db.putObject(actor, branch, pipe.uuid, QStringLiteral("hydraulic.pipe"),
                          QByteArray("invalid model bytes"),
                          db.object(actor, revision, pipe.uuid, &error)->version, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    const std::optional<aowis::db::Finalization> finalized =
        db.finalize(actor, branch, generation, QStringLiteral("Validated groups"), {}, &error);
    REQUIRE(finalized.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
