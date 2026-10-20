#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ModelCodec;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, \
                #condition, error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

namespace
{
bool hasReference(const QSqlDatabase &connection, const QUuid &revision_uuid,
                  const QUuid &source_uuid, const QUuid &target_uuid)
{
    QSqlQuery query(connection);
    if (!query.prepare(QStringLiteral(
            "SELECT 1 FROM hydraulic_network_references WHERE storage_uuid=(SELECT b.working_storage_uuid FROM "
            "branches b JOIN revisions r ON r.branch_uuid=b.uuid AND r.project_uuid=b.project_uuid WHERE r.uuid=? "
            "AND r.state='working') AND source_uuid=? AND target_uuid=? LIMIT 1")))
        return false;
    query.addBindValue(revision_uuid.toRfc4122());
    query.addBindValue(source_uuid.toRfc4122());
    query.addBindValue(target_uuid.toRfc4122());
    return query.exec() && query.next();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("local-resources.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Local hydraulic resources"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    network.nodes_reservoirs.append(reservoir);
    network.nodes_junctions.append(junction);
    for (int i = 0; i < 300; ++i)
    {
        HydraulicNodeJunction extra;
        extra.uuid = createUuidV7();
        network.nodes_junctions.append(extra);
    }

    HydraulicPipeMaterial material_a;
    material_a.uuid = createUuidV7();
    HydraulicPipeMaterial material_b;
    material_b.uuid = createUuidV7();
    network.pipe_materials = {material_a, material_b};
    HydraulicCurvePumpHead head_a;
    head_a.uuid = createUuidV7();
    HydraulicCurvePumpHead head_b;
    head_b.uuid = createUuidV7();
    network.curves_pump_head = {head_a, head_b};
    HydraulicCurvePumpEfficiency efficiency;
    efficiency.uuid = createUuidV7();
    network.curves_pump_efficiency.append(efficiency);
    HydraulicPatternTime pattern_a;
    pattern_a.uuid = createUuidV7();
    HydraulicPatternTime pattern_b;
    pattern_b.uuid = createUuidV7();
    network.patterns_time = {pattern_a, pattern_b};
    HydraulicCurveValveHeadloss loss_a;
    loss_a.uuid = createUuidV7();
    HydraulicCurveValveHeadloss loss_b;
    loss_b.uuid = createUuidV7();
    network.curves_valve_headloss = {loss_a, loss_b};
    HydraulicCurveValveCharacteristic characteristic;
    characteristic.uuid = createUuidV7();
    network.curves_valve_characteristic.append(characteristic);

    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = junction.uuid;
    pipe.material_uuid = material_a.uuid;
    network.links_pipes.append(pipe);

    HydraulicLinkPump pump;
    pump.uuid = createUuidV7();
    pump.node_uuid_from = reservoir.uuid;
    pump.node_uuid_to = junction.uuid;
    pump.head_curve_uuid = head_a.uuid;
    pump.speed_pattern_uuid = pattern_a.uuid;
    network.links_pumps.append(pump);

    HydraulicLinkValve valve;
    valve.uuid = createUuidV7();
    valve.node_uuid_from = reservoir.uuid;
    valve.node_uuid_to = junction.uuid;
    valve.head_loss_curve_uuid = loss_a.uuid;
    network.links_valves.append(valve);

    REQUIRE(aowis::db::validateHydraulicNetwork(network).valid());
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));
    qint64 generation = db.revision(actor, revision, &error)->generation;

    QSqlDatabase inspector = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), QStringLiteral("local-resources-inspector"));
    inspector.setDatabaseName(path);
    REQUIRE(inspector.open());

    // Changing an existing pipe's material or a pump/valve's referenced
    // resources must use typed, network-scoped lookups and delta-index writes.
    HydraulicLinkPipe changed_pipe = pipe;
    changed_pipe.material_uuid = material_b.uuid;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, changed_pipe, 1, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pipe.uuid, material_b.uuid));
    REQUIRE(!hasReference(inspector, revision, pipe.uuid, material_a.uuid));

    HydraulicLinkPump changed_pump = pump;
    changed_pump.head_curve_uuid = head_b.uuid;
    changed_pump.speed_pattern_uuid = pattern_b.uuid;
    changed_pump.efficiency_curve_uuid = efficiency.uuid;
    changed_pump.price_pattern_uuid = pattern_b.uuid;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, changed_pump, 1, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_b.uuid));
    REQUIRE(!hasReference(inspector, revision, pump.uuid, head_a.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, efficiency.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, pattern_b.uuid));

    HydraulicLinkValve changed_valve = valve;
    changed_valve.head_loss_curve_uuid = loss_b.uuid;
    changed_valve.characteristic_curve_uuid = characteristic.uuid;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, changed_valve, 1, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, valve.uuid, loss_b.uuid));
    REQUIRE(!hasReference(inspector, revision, valve.uuid, loss_a.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, characteristic.uuid));

    const qsizetype count_before = db.objectHistoryForRevision(actor, revision, &error).size();
    HydraulicLinkPipe invalid_pipe = changed_pipe;
    invalid_pipe.material_uuid = head_b.uuid; // Wrong resource type.
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pipe, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == count_before);
    REQUIRE(!hasReference(inspector, revision, pipe.uuid, head_b.uuid));

    HydraulicLinkPump invalid_pump = changed_pump;
    invalid_pump.head_curve_uuid = loss_b.uuid; // Valve curve, not pump-head curve.
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    invalid_pump = changed_pump;
    invalid_pump.speed_pattern_uuid = material_b.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    invalid_pump = changed_pump;
    invalid_pump.efficiency_curve_uuid = head_b.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    invalid_pump = changed_pump;
    invalid_pump.price_pattern_uuid = createUuidV7();
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));

    HydraulicLinkValve invalid_valve = changed_valve;
    invalid_valve.head_loss_curve_uuid = head_b.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_valve, 2, &error));
    invalid_valve = changed_valve;
    invalid_valve.characteristic_curve_uuid = material_b.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_valve, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == count_before);

    // A valid resource in another network is still an invalid target.
    NetworkHydraulic other;
    other.uuid = createUuidV7();
    HydraulicCurvePumpHead foreign_curve;
    foreign_curve.uuid = createUuidV7();
    other.curves_pump_head.append(foreign_curve);
    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, other, &error));
    generation = db.revision(actor, revision, &error)->generation;
    const qsizetype history_after_other_network =
        db.objectHistoryForRevision(actor, revision, &error).size();
    invalid_pump = changed_pump;
    invalid_pump.head_curve_uuid = foreign_curve.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history_after_other_network);

    // One invalid resource invalidates the entire multi-object transaction,
    // including an otherwise valid pipe material change in the same group.
    const qsizetype history_before_invalid_group =
        db.objectHistoryForRevision(actor, revision, &error).size();
    invalid_pump = changed_pump;
    invalid_pump.head_curve_uuid = loss_b.uuid;
    REQUIRE(!db.commitObjectGroup(actor, branch, generation,
        {{pipe.uuid, QStringLiteral("hydraulic.pipe"),
          ModelCodec<HydraulicLinkPipe>::encode(pipe), 2},
         {pump.uuid, QStringLiteral("hydraulic.pump"),
          ModelCodec<HydraulicLinkPump>::encode(invalid_pump), 2}}, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history_before_invalid_group);
    REQUIRE(hasReference(inspector, revision, pipe.uuid, material_b.uuid));
    REQUIRE(!hasReference(inspector, revision, pipe.uuid, material_a.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_b.uuid));
    REQUIRE(!hasReference(inspector, revision, pump.uuid, loss_b.uuid));

    // Grouped resource updates and their inverses must maintain identical
    // references, state and append-only history after closing and reopening.
    const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, generation,
        {{pipe.uuid, QStringLiteral("hydraulic.pipe"),
          ModelCodec<HydraulicLinkPipe>::encode(pipe), 2},
         {pump.uuid, QStringLiteral("hydraulic.pump"),
          ModelCodec<HydraulicLinkPump>::encode(pump), 2},
         {valve.uuid, QStringLiteral("hydraulic.valve"),
          ModelCodec<HydraulicLinkValve>::encode(valve), 2}}, &error);
    REQUIRE(group.has_value());
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pipe.uuid, material_a.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_a.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, loss_a.uuid));

    REQUIRE(db.undoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pipe.uuid, material_b.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_b.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, loss_b.uuid));

    REQUIRE(db.redoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pipe.uuid, material_a.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_a.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, loss_a.uuid));

    const std::optional<NetworkHydraulic> restored =
        db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(restored.has_value());
    REQUIRE(aowis::db::validateHydraulicNetwork(*restored).valid());
    REQUIRE(restored->links_pipes.first().material_uuid == material_a.uuid);
    REQUIRE(restored->links_pumps.first().head_curve_uuid == head_a.uuid);
    REQUIRE(restored->links_valves.first().head_loss_curve_uuid == loss_a.uuid);

    const std::optional<aowis::db::Finalization> finalization =
        db.finalize(actor, branch, generation, QStringLiteral("Local resource references"), {}, &error);
    REQUIRE(finalization.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    inspector.close();
    inspector = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("local-resources-inspector"));
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
