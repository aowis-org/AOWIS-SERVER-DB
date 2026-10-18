#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
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
int referenceCount(QSqlDatabase &inspector, const QUuid &revision, const QUuid &target)
{
    QSqlQuery query(inspector);
    if (!query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_references "
            "WHERE revision_uuid=? AND target_uuid=?")))
        return -1;
    query.addBindValue(revision.toRfc4122());
    query.addBindValue(target.toRfc4122());
    return query.exec() && query.next() ? query.value(0).toInt() : -1;
}

bool hasReference(QSqlDatabase &inspector, const QUuid &revision,
                  const QUuid &source, const QUuid &target)
{
    QSqlQuery query(inspector);
    if (!query.prepare(QStringLiteral(
            "SELECT 1 FROM hydraulic_network_references "
            "WHERE revision_uuid=? AND source_uuid=? AND target_uuid=? LIMIT 1")))
        return false;
    query.addBindValue(revision.toRfc4122());
    query.addBindValue(source.toRfc4122());
    query.addBindValue(target.toRfc4122());
    return query.exec() && query.next();
}

// Verify the backend leaves the indexed reference IDs as exact SQLite BLOBs.
bool referenceUuidStorage(QSqlDatabase &inspector, const QUuid &revision,
                          const QUuid &network, const QUuid &source, const QUuid &target)
{
    QSqlQuery query(inspector);
    if (!query.prepare(QStringLiteral(
            "SELECT typeof(revision_uuid),revision_uuid,typeof(network_uuid),network_uuid,"
            "typeof(source_uuid),source_uuid,typeof(target_uuid),target_uuid "
            "FROM hydraulic_network_references WHERE revision_uuid=? AND network_uuid=? "
            "AND source_uuid=? AND target_uuid=? LIMIT 1")))
        return false;
    query.addBindValue(revision.toRfc4122());
    query.addBindValue(network.toRfc4122());
    query.addBindValue(source.toRfc4122());
    query.addBindValue(target.toRfc4122());
    if (!query.exec() || !query.next())
        return false;
    const QUuid uuids[]{revision, network, source, target};
    for (int index = 0; index < 4; ++index)
    {
        if (query.value(index * 2).toString() != QStringLiteral("blob") ||
            query.value(index * 2 + 1).toByteArray() != uuids[index].toRfc4122())
            return false;
    }
    return true;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString file = temp.filePath(QStringLiteral("references.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(file, &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Reverse references"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    network.nodes_junctions.append(junction);
    network.nodes_reservoirs.append(reservoir);
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = junction.uuid;
    network.links_pipes.append(pipe);
    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = junction.uuid;
    point.attachment.pipe_position = 0.5;
    network.demand_points.append(point);
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));

    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("references-inspect"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        REQUIRE(referenceCount(inspector, revision, junction.uuid) == 2); // pipe + point
        REQUIRE(hasReference(inspector, revision, pipe.uuid, reservoir.uuid));
        REQUIRE(referenceUuidStorage(inspector, revision, network.uuid, pipe.uuid, reservoir.uuid));
        REQUIRE(referenceUuidStorage(inspector, revision, network.uuid, point.uuid, pipe.uuid));
        REQUIRE(hasReference(inspector, revision, point.uuid, pipe.uuid));
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("references-inspect"));

    // Scalar edits retain every reference and must remain individually reversible.
    HydraulicLinkPipe wider = pipe;
    wider.diameter_mm = 325.75;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, wider, 1, &error));
    const std::optional<HydraulicLinkPipe> loaded =
        aowis::db::loadHydraulicEntity<HydraulicLinkPipe>(db, actor, revision, pipe.uuid, &error);
    REQUIRE(loaded.has_value() && loaded->diameter_mm == 325.75);
    HydraulicLinkPipe invalid = wider;
    invalid.node_uuid_to = createUuidV7();
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, invalid, 2, &error));
    REQUIRE(db.loadHydraulicNetwork(actor, revision, network.uuid, &error)->links_pipes.first().node_uuid_to == junction.uuid);

    // A grouped structural edit updates both the pipe and demand point.
    HydraulicNodeJunction alternative;
    alternative.uuid = createUuidV7();
    network.nodes_junctions.append(alternative);
    network.links_pipes[0] = wider;
    const qint64 before_save = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.saveHydraulicNetwork(actor, branch, before_save, network, &error));
    const qint64 before_group = db.revision(actor, revision, &error)->generation;
    HydraulicLinkPipe redirected = wider;
    redirected.node_uuid_to = alternative.uuid;
    HydraulicDemandPoint reassigned = point;
    reassigned.attachment.pipe_assigned_junction_uuid = alternative.uuid;
    const QVector<ObjectEdit> changes{
        {pipe.uuid, QStringLiteral("hydraulic.pipe"), ModelCodec<HydraulicLinkPipe>::encode(redirected), 2},
        {point.uuid, QStringLiteral("hydraulic.demand_point"), ModelCodec<HydraulicDemandPoint>::encode(reassigned), 1}
    };
    const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, before_group, changes, &error);
    REQUIRE(group.has_value());
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("references-edited"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        REQUIRE(referenceCount(inspector, revision, junction.uuid) == 0);
        REQUIRE(referenceCount(inspector, revision, alternative.uuid) == 2);
        REQUIRE(referenceUuidStorage(inspector, revision, network.uuid, pipe.uuid, alternative.uuid));
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("references-edited"));
    const qint64 after_group = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.undoObjectGroup(actor, branch, after_group, &error) == group);
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("references-undo"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        REQUIRE(referenceCount(inspector, revision, junction.uuid) == 2);
        REQUIRE(referenceUuidStorage(inspector, revision, network.uuid, pipe.uuid, junction.uuid));
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("references-undo"));
    REQUIRE(db.redoObjectGroup(actor, branch,
        db.revision(actor, revision, &error)->generation, &error) == group);

    const qint64 before_finalize = db.revision(actor, revision, &error)->generation;
    const std::optional<aowis::db::Finalization> sealed = db.finalize(actor, branch,
        before_finalize, QStringLiteral("Indexed"), {}, &error);
    REQUIRE(sealed.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);

    // More than 512 references exercises two full fresh-network batches and
    // a remainder. Duplicate graph references must be stored only once.
    // Check ordered group links and stored membership as well.
    const std::optional<aowis::db::ProjectCreation> batch_project =
        db.createProject(actor, QStringLiteral("Reference batch"), &error);
    REQUIRE(batch_project.has_value());
    NetworkHydraulic large_network;
    large_network.uuid = createUuidV7();
    HydraulicNodeReservoir large_source;
    large_source.uuid = createUuidV7();
    large_network.nodes_reservoirs.append(large_source);
    large_network.options_report.selection_nodes.mode =
        HydraulicSimulationReportSelectionMode::Selected;
    large_network.options_report.selection_nodes.uuids.append(large_source.uuid);
    large_network.options_report.selection_nodes.uuids.append(large_source.uuid);
    QUuid previous_node = large_source.uuid;
    for (int index = 0; index < 265; ++index)
    {
        HydraulicNodeJunction next;
        next.uuid = createUuidV7();
        large_network.nodes_junctions.append(next);
        HydraulicLinkPipe link;
        link.uuid = createUuidV7();
        link.node_uuid_from = previous_node;
        link.node_uuid_to = next.uuid;
        large_network.links_pipes.append(link);
        previous_node = next.uuid;
    }
    REQUIRE(db.saveHydraulicNetwork(actor, batch_project->main_branch_uuid, 0, large_network, &error));
    const QVector<aowis::db::ObjectChange> large_history = db.objectHistoryForRevision(
        actor, batch_project->working_revision_uuid, &error);
    REQUIRE(large_history.size() == 532);
    const QVector<aowis::db::ObjectGroup> large_groups = db.objectGroupsForRevision(
        actor, batch_project->working_revision_uuid, &error);
    REQUIRE(large_groups.size() == 1);
    REQUIRE(large_groups.first().object_event_uuids.size() == large_history.size());
    for (qsizetype item = 0; item < large_history.size(); ++item)
    {
        REQUIRE(large_history.at(item).sequence == item + 1);
        REQUIRE(large_groups.first().object_event_uuids.at(item) == large_history.at(item).uuid);
    }
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          QStringLiteral("references-batch"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery count(inspector);
        REQUIRE(count.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_references "
            "WHERE revision_uuid=? AND network_uuid=?")));
        count.addBindValue(batch_project->working_revision_uuid.toRfc4122());
        count.addBindValue(large_network.uuid.toRfc4122());
        REQUIRE(count.exec() && count.next());
        REQUIRE(count.value(0).toInt() == 531);
        REQUIRE(referenceCount(inspector, batch_project->working_revision_uuid,
                               large_source.uuid) == 2); // first pipe + unique report selection
        REQUIRE(referenceUuidStorage(inspector, batch_project->working_revision_uuid,
                                     large_network.uuid, large_network.links_pipes.last().uuid,
                                     large_network.nodes_junctions.last().uuid));
        count.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("references-batch"));
    REQUIRE(db.undoObjectGroup(actor, batch_project->main_branch_uuid,
        db.revision(actor, batch_project->working_revision_uuid, &error)->generation, &error).has_value());
    REQUIRE(!db.loadHydraulicNetwork(actor, batch_project->working_revision_uuid,
                                     large_network.uuid, &error));
    REQUIRE(db.redoObjectGroup(actor, batch_project->main_branch_uuid,
        db.revision(actor, batch_project->working_revision_uuid, &error)->generation, &error).has_value());
    REQUIRE(db.loadHydraulicNetwork(actor, batch_project->working_revision_uuid,
                                    large_network.uuid, &error).has_value());
    const std::optional<aowis::db::RevisionInfo> batch_revision = db.revision(
        actor, batch_project->working_revision_uuid, &error);
    REQUIRE(batch_revision.has_value());
    const std::optional<aowis::db::Finalization> batch_final = db.finalize(
        actor, batch_project->main_branch_uuid, batch_revision->generation,
        QStringLiteral("Batched network"), {}, &error);
    REQUIRE(batch_final.has_value());
    REQUIRE(db.verifyProject(actor, batch_project->project_uuid).valid);
    REQUIRE(db.loadHydraulicNetwork(actor, batch_final->new_working_revision_uuid,
                                    large_network.uuid, &error).has_value());
    return 0;
}
