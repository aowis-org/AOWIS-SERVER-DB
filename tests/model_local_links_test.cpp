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
            "SELECT 1 FROM hydraulic_network_references "
            "WHERE revision_uuid=? AND source_uuid=? AND target_uuid=? LIMIT 1")))
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
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("local-links.sqlite"));

    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Local pump and valve endpoints"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    HydraulicNodeJunction first;
    first.uuid = createUuidV7();
    HydraulicNodeJunction second;
    second.uuid = createUuidV7();
    network.nodes_reservoirs.append(reservoir);
    network.nodes_junctions.append(first);
    network.nodes_junctions.append(second);
    for (int i = 0; i < 400; ++i)
    {
        HydraulicNodeJunction unrelated;
        unrelated.uuid = createUuidV7();
        network.nodes_junctions.append(unrelated);
    }
    HydraulicCurvePumpHead head_curve;
    head_curve.uuid = createUuidV7();
    network.curves_pump_head.append(head_curve);
    HydraulicCurveValveHeadloss loss_curve;
    loss_curve.uuid = createUuidV7();
    network.curves_valve_headloss.append(loss_curve);
    HydraulicPatternTime speed_pattern;
    speed_pattern.uuid = createUuidV7();
    network.patterns_time.append(speed_pattern);

    HydraulicLinkPump pump;
    pump.uuid = createUuidV7();
    pump.node_uuid_from = reservoir.uuid;
    pump.node_uuid_to = first.uuid;
    pump.head_curve_uuid = head_curve.uuid;
    pump.speed_pattern_uuid = speed_pattern.uuid;
    network.links_pumps.append(pump);
    HydraulicLinkValve valve;
    valve.uuid = createUuidV7();
    valve.node_uuid_from = reservoir.uuid;
    valve.node_uuid_to = first.uuid;
    valve.head_loss_curve_uuid = loss_curve.uuid;
    network.links_valves.append(valve);
    HydraulicControlSimple control;
    control.uuid = createUuidV7();
    control.link_uuid = pump.uuid;
    control.trigger_node_uuid = reservoir.uuid;
    network.controls_simple.append(control);

    REQUIRE(aowis::db::validateHydraulicNetwork(network).valid());
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));
    qint64 generation = db.revision(actor, revision, &error)->generation;

    QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("local-links-inspector"));
    inspector.setDatabaseName(path);
    REQUIRE(inspector.open());
    REQUIRE(hasReference(inspector, revision, pump.uuid, first.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, first.uuid));
    REQUIRE(hasReference(inspector, revision, control.uuid, pump.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_curve.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, loss_curve.uuid));

    // Existing pump endpoint changed while curve, pattern and control references
    // stay stable. The new node must belong to the same hydraulic network.
    HydraulicLinkPump moved_pump = pump;
    moved_pump.node_uuid_to = second.uuid;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, moved_pump, 1, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pump.uuid, second.uuid));
    REQUIRE(!hasReference(inspector, revision, pump.uuid, first.uuid));
    REQUIRE(hasReference(inspector, revision, pump.uuid, head_curve.uuid));
    REQUIRE(hasReference(inspector, revision, control.uuid, pump.uuid));

    HydraulicLinkValve moved_valve = valve;
    moved_valve.node_uuid_to = second.uuid;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, moved_valve, 1, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, valve.uuid, second.uuid));
    REQUIRE(!hasReference(inspector, revision, valve.uuid, first.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, loss_curve.uuid));

    // Rejected endpoints must not alter model state, indexes, or event history.
    const qsizetype count_before = db.objectHistoryForRevision(actor, revision, &error).size();
    HydraulicLinkPump invalid_pump = moved_pump;
    invalid_pump.node_uuid_to = valve.uuid; // A link is not a valid node.
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == count_before);
    REQUIRE(!hasReference(inspector, revision, pump.uuid, valve.uuid));
    invalid_pump.node_uuid_to = createUuidV7();
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == count_before);

    HydraulicLinkValve invalid_valve = moved_valve;
    invalid_valve.node_uuid_to = pump.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_valve, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == count_before);
    REQUIRE(!hasReference(inspector, revision, valve.uuid, pump.uuid));

    // Even a real node UUID is invalid when it belongs to another network
    // stored in the same project and working revision.
    NetworkHydraulic other_network;
    other_network.uuid = createUuidV7();
    HydraulicNodeJunction foreign_node;
    foreign_node.uuid = createUuidV7();
    other_network.nodes_junctions.append(foreign_node);
    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, other_network, &error));
    generation = db.revision(actor, revision, &error)->generation;
    // A successful network save legitimately appends events. Establish a new
    // baseline before checking that subsequent rejected edits append none.
    const qsizetype history_after_other_network =
        db.objectHistoryForRevision(actor, revision, &error).size();
    REQUIRE(history_after_other_network > count_before);
    invalid_pump.node_uuid_to = foreign_node.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_pump, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history_after_other_network);
    REQUIRE(!hasReference(inspector, revision, pump.uuid, foreign_node.uuid));

    // Changing a different class of reference is NOT covered by the bounded
    // endpoint path; it must still be rejected through full validation.
    HydraulicLinkPump invalid_curve = moved_pump;
    invalid_curve.head_curve_uuid = createUuidV7();
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_curve, 2, &error));
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history_after_other_network);
    HydraulicLinkValve invalid_loss = moved_valve;
    invalid_loss.head_loss_curve_uuid = createUuidV7();
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid_loss, 2, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history_after_other_network);

    // Reverse two links in one atomic edit. Undo and redo must restore the
    // stored links and their derived reference-index rows together.
    const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, generation,
        {{pump.uuid, QStringLiteral("hydraulic.pump"),
          ModelCodec<HydraulicLinkPump>::encode(pump), 2},
         {valve.uuid, QStringLiteral("hydraulic.valve"),
          ModelCodec<HydraulicLinkValve>::encode(valve), 2}}, &error);
    REQUIRE(group.has_value());
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pump.uuid, first.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, first.uuid));
    REQUIRE(!hasReference(inspector, revision, pump.uuid, second.uuid));
    REQUIRE(db.undoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pump.uuid, second.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, second.uuid));
    REQUIRE(db.redoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pump.uuid, first.uuid));
    REQUIRE(hasReference(inspector, revision, valve.uuid, first.uuid));

    const std::optional<NetworkHydraulic> restored =
        db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(restored.has_value());
    REQUIRE(aowis::db::validateHydraulicNetwork(*restored).valid());
    REQUIRE(restored->links_pumps.first().node_uuid_to == first.uuid);
    REQUIRE(restored->links_valves.first().node_uuid_to == first.uuid);
    const std::optional<aowis::db::Finalization> sealed =
        db.finalize(actor, branch, generation, QStringLiteral("Validated link endpoints"), {}, &error);
    REQUIRE(sealed.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    inspector.close();
    inspector = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("local-links-inspector"));
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
