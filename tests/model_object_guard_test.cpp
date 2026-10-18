#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>

#include <array>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ModelCodec;
using aowis::db::ObjectEdit;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    Database db;
    REQUIRE(db.openSqlite(temp.filePath(QStringLiteral("model_guard.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> user = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(user.has_value());
    const OperationContext actor{user->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(actor, QStringLiteral("Graph guard"), &error);
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
    network.nodes_reservoirs.append(reservoir);
    network.nodes_junctions.append(junction);
    network.nodes_junctions.append(other);
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = junction.uuid;
    network.links_pipes.append(pipe);
    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_position = 0.5;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = junction.uuid;
    network.demand_points.append(point);

    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));
    const qint64 generation = db.revision(actor, revision, &error)->generation;
    const qsizetype events = db.objectHistoryForRevision(actor, revision, &error).size();
    const qsizetype groups = db.objectGroupsForRevision(actor, revision, &error).size();
    REQUIRE(groups == 1);
    const QByteArray original_pipe = db.object(actor, revision, pipe.uuid, &error)->content;

    // Verify the persisted root actually indexes this pipe, and that the
    // Reference validator rejects the proposed value before testing the
    // Transactional integrity guard. These preconditions distinguish a guard bypass
    // from a malformed fixture or an unindexed object.
    const std::optional<NetworkHydraulic> stored_network =
        db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(stored_network.has_value());
    REQUIRE(stored_network->links_pipes.size() == 1);
    REQUIRE(stored_network->links_pipes.first().uuid == pipe.uuid);

    // The low-level typed API must not write a dangling endpoint.
    HydraulicLinkPipe bad_pipe = pipe;
    bad_pipe.node_uuid_to = createUuidV7();
    NetworkHydraulic proposed_network = *stored_network;
    proposed_network.links_pipes[0] = bad_pipe;
    const aowis::db::ModelValidationReport proposed_report =
        aowis::db::validateHydraulicNetwork(proposed_network);
    REQUIRE(!proposed_report.valid());
    REQUIRE(proposed_report.summary().contains(QStringLiteral("node_uuid_to")));
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, bad_pipe, 1, &error));
    REQUIRE(error.contains(QStringLiteral("node_uuid_to")));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == events);
    REQUIRE(db.objectGroupsForRevision(actor, revision, &error).size() == groups);
    REQUIRE(db.object(actor, revision, pipe.uuid, &error)->content == original_pipe);

    // Deleting a referenced junction would break the existing pipe/attachment.
    REQUIRE(!db.deleteObject(actor, branch, junction.uuid, 1, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(!db.object(actor, revision, junction.uuid, &error)->deleted);

    // Editing only a root index to remove a pipe would leave the old pipe
    // live but unreachable. This must be rejected without touching history.
    const QByteArray root_bytes = db.object(actor, revision, network.uuid, &error)->content;
    QDataStream source(root_bytes);
    source.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 format_version = 0;
    QByteArray header;
    source >> magic >> format_version >> header;
    std::array<QList<QUuid>, 18> lists;
    for (QList<QUuid> &list : lists)
        source >> list;
    REQUIRE(source.status() == QDataStream::Ok && source.atEnd());
    REQUIRE(lists[12].size() == 1); // links_pipes
    lists[12].clear();
    QByteArray orphaning_index;
    QDataStream target(&orphaning_index, QIODevice::WriteOnly);
    target.setVersion(QDataStream::Qt_6_0);
    target << magic << format_version << header;
    for (const QList<QUuid> &list : lists)
        target << list;
    REQUIRE(target.status() == QDataStream::Ok);
    REQUIRE(!db.putObject(actor, branch, network.uuid, QStringLiteral("hydraulic.network"),
                          orphaning_index, 1, &error));
    REQUIRE(error.contains(QStringLiteral("without deleting it")));
    REQUIRE(db.object(actor, revision, network.uuid, &error)->content == root_bytes);
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);

    // A multi-object edit must validate the FINAL graph, not an intermediate
    // state in which the pipe endpoint has changed but the attachment has not.
    HydraulicLinkPipe relocated_pipe = pipe;
    relocated_pipe.node_uuid_to = other.uuid;
    HydraulicDemandPoint relocated_point = point;
    relocated_point.attachment.pipe_assigned_junction_uuid = other.uuid;
    const QVector<ObjectEdit> relocate{
        {pipe.uuid, QStringLiteral("hydraulic.pipe"), ModelCodec<HydraulicLinkPipe>::encode(relocated_pipe), 1},
        {point.uuid, QStringLiteral("hydraulic.demand_point"), ModelCodec<HydraulicDemandPoint>::encode(relocated_point), 1}
    };
    HydraulicNodeJunction elevated = junction;
    elevated.elevation_m = 450.5;
    REQUIRE(!db.commitObjectGroup(actor, branch, generation,
        {relocate.at(0), {junction.uuid, QStringLiteral("hydraulic.junction"),
            ModelCodec<HydraulicNodeJunction>::encode(elevated), 1}}, &error));
    REQUIRE(error.contains(QStringLiteral("assigned_junction_uuid")));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == events);

    const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, generation, relocate, &error);
    REQUIRE(group.has_value());
    REQUIRE(db.revision(actor, revision, &error)->generation == generation + 2);
    const std::optional<NetworkHydraulic> changed = db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(changed.has_value());
    REQUIRE(changed->links_pipes.first().node_uuid_to == other.uuid);
    REQUIRE(changed->demand_points.first().attachment.pipe_assigned_junction_uuid == other.uuid);

    REQUIRE(db.undoObjectGroup(actor, branch, generation + 2, &error) == group);
    const std::optional<NetworkHydraulic> reverted = db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(reverted.has_value());
    REQUIRE(reverted->links_pipes.first().node_uuid_to == junction.uuid);
    REQUIRE(reverted->demand_points.first().attachment.pipe_assigned_junction_uuid == junction.uuid);
    REQUIRE(db.redoObjectGroup(actor, branch, generation + 4, &error) == group);

    // Raw low-level edits of indexed objects use the same validation.
    REQUIRE(!db.putObject(actor, branch, pipe.uuid, QStringLiteral("hydraulic.pipe"),
                          QByteArray("invalid binary data"), 4, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation + 6);
    REQUIRE(!db.deleteObject(actor, branch, network.uuid, 1, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation + 6);

    db.close();
    REQUIRE(db.openSqlite(temp.filePath(QStringLiteral("model_guard.sqlite")), &error));
    REQUIRE(db.loadHydraulicNetwork(actor, revision, network.uuid, &error).has_value());
    const std::optional<aowis::db::Finalization> final = db.finalize(actor, branch,
        generation + 6, QStringLiteral("Validated edits"), {}, &error);
    REQUIRE(final.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // Undo of the original network creation is a legitimate full deletion:
    // all indexed entities and its root are tombstoned in the same transaction.
    const std::optional<aowis::db::ProjectCreation> second =
        db.createProject(actor, QStringLiteral("Delete entire network"), &error);
    REQUIRE(second.has_value());
    NetworkHydraulic other_network;
    other_network.uuid = createUuidV7();
    HydraulicNodeJunction single;
    single.uuid = createUuidV7();
    other_network.nodes_junctions.append(single);
    REQUIRE(db.saveHydraulicNetwork(actor, second->main_branch_uuid, 0, other_network, &error));
    const qint64 initial_generation =
        db.revision(actor, second->working_revision_uuid, &error)->generation;
    const std::optional<QUuid> initial_group =
        db.objectGroupsForRevision(actor, second->working_revision_uuid, &error).first().uuid;
    REQUIRE(db.undoObjectGroup(actor, second->main_branch_uuid, initial_generation, &error) == initial_group);
    REQUIRE(!db.loadHydraulicNetwork(actor, second->working_revision_uuid, other_network.uuid, &error));
    REQUIRE(db.redoObjectGroup(actor, second->main_branch_uuid, initial_generation + 2, &error) == initial_group);
    REQUIRE(db.loadHydraulicNetwork(actor, second->working_revision_uuid, other_network.uuid, &error).has_value());
    return 0;
}
