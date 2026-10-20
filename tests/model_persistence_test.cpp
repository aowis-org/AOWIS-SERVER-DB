#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QDate>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ModelCodec;
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
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    Database database;
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("model.sqlite")), &error));
    const std::optional<aowis::db::UserInfo> admin = database.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = database.createProject(actor, QStringLiteral("Model persistence"), &error);
    REQUIRE(project.has_value());

    NetworkHydraulic network;
    network.id = QStringLiteral("water");
    network.uuid = createUuidV7();
    network.title_line_1 = QStringLiteral("Hydraulic network é 水");
    network.duration_s = 0x1234567812345678ULL;
    network.options_hydraulic.maximum_trials = 401;
    network.options_reaction.global_pipe_bulk_reaction.coefficient = -0.08;
    network.options_report.fields_node.elevation.precision = 11;
    network.multi_species.options.timestep_s = 47.25;
    network.multi_species.options.area_units = MultiSpeciesAreaUnits::SquareCentimetres;
    MultiSpeciesSpecies species;
    species.uuid = createUuidV7();
    species.id = QStringLiteral("CL2");
    species.molecular_diffusivity_m2_per_s = 1.12e-9;
    species.note = QStringLiteral("chemical test");
    network.multi_species.species.append(species);
    network.map_backdrop.enabled = true;
    network.map_backdrop.file = QStringLiteral("relative/path.tif");

    HydraulicNodeJunction junction;
    junction.id = QStringLiteral("J1");
    junction.uuid = createUuidV7();
    junction.coordinate_wgs84.longitude_deg = 18.123456789012345;
    junction.coordinate_wgs84.latitude_deg = 11.987654321012345;
    junction.coordinate_wgs84.altitude_m = 541.000000002;
    junction.metadata.tags = {QStringLiteral("primary"), QStringLiteral("réseau")};
    junction.metadata.date_installed = QDate(2001, 9, 13);
    junction.metadata.description = QStringLiteral("Water supply node");
    HydraulicDemand demand;
    demand.category_name = QStringLiteral("Domestic");
    demand.base_demand_m3_per_h = 13.14;
    junction.demands.append(demand);
    network.nodes_junctions.append(junction);

    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    reservoir.id = QStringLiteral("R1");
    network.nodes_reservoirs.append(reservoir);
    HydraulicNodeTank tank;
    tank.uuid = createUuidV7();
    tank.id = QStringLiteral("T1");
    tank.geometry_input_type = HydraulicNodeTankGeometryInputType::VolumeCurve;
    network.nodes_tanks.append(tank);

    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = junction.uuid;
    pipe.length_measured_m = 83.125;
    pipe.diameter_mm = 123.456789123;
    pipe.roughness_darcy_weisbach_mm = 0.007;
    HydraulicLinkVertex vertex;
    vertex.coordinate_wgs84.latitude_deg = 11.1;
    pipe.vertices.append(vertex);
    network.links_pipes.append(pipe);

    HydraulicLinkPump pump;
    pump.uuid = createUuidV7();
    pump.node_uuid_from = junction.uuid;
    pump.node_uuid_to = tank.uuid;
    pump.definition_type = HydraulicLinkPumpDefinitionType::ThreePointCurve;
    network.links_pumps.append(pump);
    HydraulicLinkValve valve;
    valve.uuid = createUuidV7();
    valve.type = HydraulicLinkValveType::PCV;
    valve.setting_position_percent = 62.375;
    network.links_valves.append(valve);

    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_position = 0.654321987654321;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = junction.uuid;
    WaterMeter meter;
    meter.uuid = createUuidV7();
    meter.serial_number = QStringLiteral("ABC-1234");
    point.meter = meter;
    network.demand_points.append(point);

    HydraulicPipeMaterial material;
    material.uuid = createUuidV7();
    material.description = QStringLiteral("Ductile iron");
    HydraulicPipeMaterialRoughnessAtAge roughness;
    roughness.age_years = 15;
    roughness.roughness_hazen_williams = 112.3;
    material.roughness_by_age.append(roughness);
    network.pipe_materials.append(material);

    HydraulicPatternTime pattern;
    pattern.uuid = createUuidV7();
    pattern.multipliers = {0.1234567890123, 0.9876543212345};
    network.patterns_time.append(pattern);

    HydraulicCurveTankVolume cv;
    cv.uuid = createUuidV7();
    cv.points.append({10.123, 100.456});
    network.curves_tank_volume.append(cv);
    HydraulicCurvePumpHead ph;
    ph.uuid = createUuidV7();
    ph.points.append({0.1, 100.3});
    network.curves_pump_head.append(ph);
    HydraulicCurvePumpEfficiency pe;
    pe.uuid = createUuidV7();
    pe.points.append({0.2, 86.77});
    network.curves_pump_efficiency.append(pe);
    HydraulicCurveValveHeadloss vh;
    vh.uuid = createUuidV7();
    vh.points.append({0.3, 2.22});
    network.curves_valve_headloss.append(vh);
    HydraulicCurveValveCharacteristic vc;
    vc.uuid = createUuidV7();
    vc.points.append({20.2, 40.4});
    network.curves_valve_characteristic.append(vc);
    HydraulicCurveGeneric cg;
    cg.uuid = createUuidV7();
    cg.points.append({5.55, 8.888});
    network.curves_generic.append(cg);

    HydraulicControlSimple simple;
    simple.uuid = createUuidV7();
    simple.link_uuid = pipe.uuid;
    simple.setting.valve_pressure_head_m = 34.56;
    network.controls_simple.append(simple);
    HydraulicControlRule rule;
    rule.uuid = createUuidV7();
    HydraulicControlRulePremise premise;
    premise.object_uuid = junction.uuid;
    premise.pressure_head_m = 20.8;
    rule.premises.append(premise);
    network.controls_rules.append(rule);
    HydraulicMapLabel label;
    label.uuid = createUuidV7();
    label.text = QStringLiteral("Village");
    network.map_labels.append(label);

    const QByteArray encoded = ModelCodec<NetworkHydraulic>::encode(network);
    REQUIRE(!encoded.isEmpty());
    const std::optional<NetworkHydraulic> standalone = ModelCodec<NetworkHydraulic>::decode(encoded, &error);
    REQUIRE(standalone.has_value());
    REQUIRE(ModelCodec<NetworkHydraulic>::encode(*standalone) == encoded);
    REQUIRE(!ModelCodec<HydraulicNodeTank>::decode(encoded, &error));
    REQUIRE(!ModelCodec<NetworkHydraulic>::decode(encoded.left(encoded.size()-5), &error));
    REQUIRE(!ModelCodec<NetworkHydraulic>::decode(encoded + QByteArray("trailing"), &error));

    REQUIRE(database.saveHydraulicNetwork(actor, project->main_branch_uuid, 0, network, &error));
    const std::optional<NetworkHydraulic> loaded = database.loadHydraulicNetwork(actor, project->working_revision_uuid, network.uuid, &error);
    REQUIRE(loaded.has_value());
    REQUIRE(ModelCodec<NetworkHydraulic>::encode(*loaded) == encoded);
    const std::optional<aowis::db::RevisionInfo> current = database.revision(actor, project->working_revision_uuid, &error);
    REQUIRE(current.has_value() && current->generation == 19); // network root + 18 entity kinds
    REQUIRE(database.objects(actor, project->working_revision_uuid, &error).size() == 19);
    REQUIRE(database.objectGroupsForRevision(actor, project->working_revision_uuid, &error).size() == 1);
    REQUIRE(database.objectGroupsForRevision(actor, project->working_revision_uuid, &error)
                .first().object_event_uuids.size() == 19);
    const QVector<aowis::db::ObjectChange> initial_events =
        database.objectHistoryForRevision(actor, project->working_revision_uuid, &error);
    REQUIRE(initial_events.size() == 19);
    for (qsizetype index = 0; index < initial_events.size(); ++index)
    {
        REQUIRE(initial_events.at(index).sequence == index + 1);
        REQUIRE(initial_events.at(index).actor_uuid == actor.actor_uuid);
        REQUIRE(initial_events.at(index).action == QStringLiteral("put"));
        REQUIRE(initial_events.at(index).before_version == -1);
        REQUIRE(initial_events.at(index).after_version == 1);
    }
    REQUIRE(database.object(actor, project->working_revision_uuid, pipe.uuid, &error)->kind == QStringLiteral("hydraulic.pipe"));
    REQUIRE(!database.saveHydraulicNetwork(actor, project->main_branch_uuid, 0, network, &error));
    NetworkHydraulic conflicting = network;
    conflicting.uuid = createUuidV7();
    REQUIRE(!database.saveHydraulicNetwork(actor, project->main_branch_uuid,
                                           current->generation, conflicting, &error));
    REQUIRE(database.revision(actor, project->working_revision_uuid, &error)->generation == current->generation);
    REQUIRE(database.saveHydraulicNetwork(actor, project->main_branch_uuid, current->generation, network, &error));
    REQUIRE(database.revision(actor, project->working_revision_uuid, &error)->generation == current->generation);
    REQUIRE(database.objectGroupsForRevision(actor, project->working_revision_uuid, &error).size() == 1);

    HydraulicNodeJunction updated = junction;
    updated.elevation_m = 123.456789;
    REQUIRE(aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid, updated, 1, &error));
    const std::optional<NetworkHydraulic> after_edit = database.loadHydraulicNetwork(actor, project->working_revision_uuid, network.uuid, &error);
    REQUIRE(after_edit.has_value() && after_edit->nodes_junctions[0].elevation_m == updated.elevation_m);
    REQUIRE(!aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid, updated, 1, &error));
    REQUIRE(database.objectGroupsForRevision(actor, project->working_revision_uuid, &error).size() == 2);

    NetworkHydraulic smaller = *after_edit;
    smaller.links_valves.clear();
    const qint64 generation = database.revision(actor, project->working_revision_uuid, &error)->generation;
    REQUIRE(database.saveHydraulicNetwork(actor, project->main_branch_uuid, generation, smaller, &error));
    REQUIRE(database.object(actor, project->working_revision_uuid, valve.uuid, &error)->deleted);
    REQUIRE(database.loadHydraulicNetwork(actor, project->working_revision_uuid, network.uuid, &error)->links_valves.isEmpty());
    REQUIRE(database.objectGroupsForRevision(actor, project->working_revision_uuid, &error).size() == 3);
    REQUIRE(database.objectGroupsForRevision(actor, project->working_revision_uuid, &error)
                .last().object_event_uuids.size() == 2);

    const qint64 final_generation = database.revision(actor, project->working_revision_uuid, &error)->generation;
    const std::optional<aowis::db::Finalization> finalized = database.finalize(
        actor, project->main_branch_uuid, final_generation, QStringLiteral("Typed Model"), QStringLiteral("all fields"), &error);
    REQUIRE(finalized.has_value());
    REQUIRE(database.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(database.verifyProjectJournal(actor, project->project_uuid).valid);
    REQUIRE(database.loadHydraulicNetwork(actor, project->working_revision_uuid, network.uuid, &error)->nodes_junctions[0].elevation_m == updated.elevation_m);
    REQUIRE(database.loadHydraulicNetwork(actor, finalized->new_working_revision_uuid, network.uuid, &error).has_value());
    const std::optional<aowis::db::BranchInfo> alternative = database.createBranch(
        actor, project->project_uuid, QStringLiteral("alternative"), project->working_revision_uuid, &error);
    REQUIRE(alternative.has_value());
    REQUIRE(database.loadHydraulicNetwork(actor, alternative->head_revision_uuid, network.uuid, &error).has_value());

    database.close();
    REQUIRE(database.openSqlite(directory.filePath(QStringLiteral("model.sqlite")), &error));
    REQUIRE(database.loadHydraulicNetwork(actor, project->working_revision_uuid, network.uuid, &error).has_value());
    REQUIRE(database.verifyProject(actor, project->project_uuid).valid);

    // Finalized model data lives only in the Merkle tree; its editable
    // successor is independently materialized for efficient writes.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("model-merkle-inspector"));
        inspector.setDatabaseName(directory.filePath(QStringLiteral("model.sqlite")));
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM revision_objects WHERE storage_uuid=(SELECT b.working_storage_uuid FROM branches " "b JOIN revisions r ON r.branch_uuid=b.uuid AND r.project_uuid=b.project_uuid WHERE r.uuid=? AND " "r.state='working')")));
        query.addBindValue(project->working_revision_uuid.toRfc4122());
        REQUIRE(query.exec() && query.next() && query.value(0).toInt() == 0);
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("model-merkle-inspector"));
    const std::optional<NetworkHydraulic> sealed_network = database.loadHydraulicNetwork(
        actor, project->working_revision_uuid, network.uuid, &error);
    REQUIRE(sealed_network && sealed_network->nodes_junctions.size() == 1 &&
            sealed_network->nodes_junctions.first().elevation_m == updated.elevation_m);
    REQUIRE(database.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
