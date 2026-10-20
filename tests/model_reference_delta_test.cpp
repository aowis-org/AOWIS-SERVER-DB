#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
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

namespace
{
int referenceCount(const QSqlDatabase &database, const QUuid &revision,
                   const QUuid &target)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_references WHERE storage_uuid=(SELECT b.working_storage_uuid "
            "FROM branches b JOIN revisions r ON r.branch_uuid=b.uuid AND r.project_uuid=b.project_uuid WHERE "
            "r.uuid=? AND r.state='working') AND target_uuid=?")))
        return -1;
    query.addBindValue(revision.toRfc4122());
    query.addBindValue(target.toRfc4122());
    return query.exec() && query.next() ? query.value(0).toInt() : -1;
}

int networkReferenceCount(const QSqlDatabase &database, const QUuid &revision,
                          const QUuid &network)
{
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_references WHERE storage_uuid=(SELECT b.working_storage_uuid "
            "FROM branches b JOIN revisions r ON r.branch_uuid=b.uuid AND r.project_uuid=b.project_uuid WHERE "
            "r.uuid=? AND r.state='working') AND network_uuid=?")))
        return -1;
    query.addBindValue(revision.toRfc4122());
    query.addBindValue(network.toRfc4122());
    return query.exec() && query.next() ? query.value(0).toInt() : -1;
}

bool sql(QSqlDatabase &database, const QString &statement, QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement))
        return true;
    if (error != nullptr)
        *error = query.lastError().text();
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("reference-delta.sqlite"));

    Database db;
    REQUIRE(db.openSqlite(file, &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Reference deltas"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    HydraulicNodeJunction old_junction;
    old_junction.uuid = createUuidV7();
    HydraulicNodeJunction new_junction;
    new_junction.uuid = createUuidV7();
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = old_junction.uuid;
    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = old_junction.uuid;
    point.attachment.pipe_position = 0.5;
    network.nodes_reservoirs.append(reservoir);
    network.nodes_junctions.append(old_junction);
    network.nodes_junctions.append(new_junction);
    network.links_pipes.append(pipe);
    network.demand_points.append(point);
    REQUIRE(aowis::db::validateHydraulicNetwork(network).valid());
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));
    qint64 generation = db.revision(actor, revision, &error)->generation;

    // SQL triggers are an exact test of *no writes*, unlike timing checks.
    // They remain installed while both full-network and grouped edit/undo/redo
    // paths run. An old-style full DELETE + INSERT would fail immediately.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("reference-delta-inspector"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        REQUIRE(networkReferenceCount(inspector, revision, network.uuid) == 4);
        REQUIRE(referenceCount(inspector, revision, old_junction.uuid) == 2);
        REQUIRE(sql(inspector, QStringLiteral(
            "CREATE TRIGGER no_reference_delete BEFORE DELETE ON hydraulic_network_references "
            "BEGIN SELECT RAISE(ABORT,'unchanged reference deleted'); END"), &error));
        REQUIRE(sql(inspector, QStringLiteral(
            "CREATE TRIGGER no_reference_insert BEFORE INSERT ON hydraulic_network_references "
            "BEGIN SELECT RAISE(ABORT,'unchanged reference inserted'); END"), &error));

        // A full-network save with the same graph references must not update
        // the reverse-reference index, although its scalar payload is changed.
        network.links_pipes[0].diameter_mm = 212.5;
        REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, network, &error));
        generation = db.revision(actor, revision, &error)->generation;
        REQUIRE(networkReferenceCount(inspector, revision, network.uuid) == 4);

        // Multiple object edits force complete network validation, but not
        // reference-table churn. Undo and redo must also avoid the churn.
        HydraulicLinkPipe revised_pipe = network.links_pipes.first();
        revised_pipe.roughness_hazen_williams = 125.125;
        HydraulicDemandPoint revised_point = network.demand_points.first();
        revised_point.attachment.pipe_position = 0.75;
        const QVector<ObjectEdit> edits{
            {pipe.uuid, QStringLiteral("hydraulic.pipe"),
             ModelCodec<HydraulicLinkPipe>::encode(revised_pipe), 2},
            {point.uuid, QStringLiteral("hydraulic.demand_point"),
             ModelCodec<HydraulicDemandPoint>::encode(revised_point), 1}
        };
        const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, generation, edits, &error);
        REQUIRE(group.has_value());
        generation = db.revision(actor, revision, &error)->generation;
        REQUIRE(db.undoObjectGroup(actor, branch, generation, &error) == group);
        generation = db.revision(actor, revision, &error)->generation;
        REQUIRE(db.redoObjectGroup(actor, branch, generation, &error) == group);
        generation = db.revision(actor, revision, &error)->generation;
        REQUIRE(networkReferenceCount(inspector, revision, network.uuid) == 4);
        REQUIRE(sql(inspector, QStringLiteral("DROP TRIGGER no_reference_delete"), &error));
        REQUIRE(sql(inspector, QStringLiteral("DROP TRIGGER no_reference_insert"), &error));

        // Change two references while protecting a third, unchanged edge.
        // This distinguishes true deltas from a conditional full rebuild.
        const QString reservoir_hex = QString::fromLatin1(reservoir.uuid.toRfc4122().toHex());
        const QString pipe_hex = QString::fromLatin1(pipe.uuid.toRfc4122().toHex());
        REQUIRE(sql(inspector, QStringLiteral(
            "CREATE TRIGGER protect_unchanged_delete BEFORE DELETE ON hydraulic_network_references "
            "WHEN OLD.source_uuid=x'%1' AND OLD.target_uuid=x'%2' "
            "BEGIN SELECT RAISE(ABORT,'unmodified edge deleted'); END")
            .arg(pipe_hex, reservoir_hex), &error));
        REQUIRE(sql(inspector, QStringLiteral(
            "CREATE TRIGGER protect_unchanged_insert BEFORE INSERT ON hydraulic_network_references "
            "WHEN NEW.source_uuid=x'%1' AND NEW.target_uuid=x'%2' "
            "BEGIN SELECT RAISE(ABORT,'unmodified edge inserted'); END")
            .arg(pipe_hex, reservoir_hex), &error));

        network.links_pipes[0] = revised_pipe;
        network.demand_points[0] = revised_point;
        network.links_pipes[0].node_uuid_to = new_junction.uuid;
        network.demand_points[0].attachment.pipe_assigned_junction_uuid = new_junction.uuid;
        REQUIRE(aowis::db::validateHydraulicNetwork(network).valid());

        // A failure while inserting one of the actual deltas must roll back
        // the object changes, reference removals, and edit journal together.
        const QString new_hex = QString::fromLatin1(new_junction.uuid.toRfc4122().toHex());
        REQUIRE(sql(inspector, QStringLiteral(
            "CREATE TRIGGER reject_new_edge BEFORE INSERT ON hydraulic_network_references "
            "WHEN NEW.target_uuid=x'%1' "
            "BEGIN SELECT RAISE(ABORT,'injected reference delta failure'); END")
            .arg(new_hex), &error));
        REQUIRE(!db.saveHydraulicNetwork(actor, branch, generation, network, &error));
        REQUIRE(db.revision(actor, revision, &error)->generation == generation);
        REQUIRE(referenceCount(inspector, revision, old_junction.uuid) == 2);
        REQUIRE(referenceCount(inspector, revision, new_junction.uuid) == 0);
        const std::optional<NetworkHydraulic> restored =
            db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
        REQUIRE(restored.has_value());
        REQUIRE(restored->links_pipes.first().node_uuid_to == old_junction.uuid);
        REQUIRE(sql(inspector, QStringLiteral("DROP TRIGGER reject_new_edge"), &error));

        REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, network, &error));
        generation = db.revision(actor, revision, &error)->generation;
        REQUIRE(networkReferenceCount(inspector, revision, network.uuid) == 4);
        REQUIRE(referenceCount(inspector, revision, old_junction.uuid) == 0);
        REQUIRE(referenceCount(inspector, revision, new_junction.uuid) == 2);
        REQUIRE(referenceCount(inspector, revision, reservoir.uuid) == 1);
        REQUIRE(referenceCount(inspector, revision, pipe.uuid) == 1);
        REQUIRE(sql(inspector, QStringLiteral("DROP TRIGGER protect_unchanged_delete"), &error));
        REQUIRE(sql(inspector, QStringLiteral("DROP TRIGGER protect_unchanged_insert"), &error));
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("reference-delta-inspector"));

    const std::optional<aowis::db::Finalization> sealed = db.finalize(
        actor, branch, generation, QStringLiteral("Deltas"), {}, &error);
    REQUIRE(sealed.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.loadHydraulicNetwork(actor, sealed->new_working_revision_uuid,
                                    network.uuid, &error).has_value());
    db.close();
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
