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
qint64 rowCount(QSqlDatabase &connection, const QString &table,
                const QUuid &revision_uuid)
{
    QSqlQuery query(connection);
    if (!query.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE revision_uuid=?").arg(table)))
        return -1;
    query.addBindValue(revision_uuid.toRfc4122());
    if (!query.exec() || !query.next())
        return -1;
    return query.value(0).toLongLong();
}

bool hasReference(QSqlDatabase &connection, const QUuid &revision_uuid,
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
    const QString path = temp.filePath(QStringLiteral("local-structure.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Local structural validation"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    HydraulicNodeJunction original;
    original.uuid = createUuidV7();
    HydraulicNodeJunction alternative;
    alternative.uuid = createUuidV7();
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = reservoir.uuid;
    pipe.node_uuid_to = original.uuid;
    HydraulicDemandPoint point;
    point.uuid = createUuidV7();
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = pipe.uuid;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = original.uuid;
    point.attachment.pipe_position = 0.5;
    network.nodes_reservoirs.append(reservoir);
    network.nodes_junctions.append(original);
    network.nodes_junctions.append(alternative);
    network.links_pipes.append(pipe);
    network.demand_points.append(point);
    // A large, unrelated portion of the same network must remain unaffected.
    for (int i = 0; i < 400; ++i)
    {
        HydraulicNodeJunction unreferenced;
        unreferenced.uuid = createUuidV7();
        network.nodes_junctions.append(unreferenced);
    }
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, network, &error));
    qint64 generation = db.revision(actor, revision, &error)->generation;

    QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("local-structure-inspector"));
    inspector.setDatabaseName(path);
    REQUIRE(inspector.open());
    REQUIRE(hasReference(inspector, revision, pipe.uuid, original.uuid));
    REQUIRE(hasReference(inspector, revision, point.uuid, original.uuid));
    const qsizetype history_before = db.objectHistoryForRevision(actor, revision, &error).size();

    // Changing only the pipe breaks the demand point's assigned endpoint.
    HydraulicLinkPipe redirected = pipe;
    redirected.node_uuid_to = alternative.uuid;
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, redirected, 1, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history_before);
    REQUIRE(hasReference(inspector, revision, pipe.uuid, original.uuid));
    REQUIRE(!hasReference(inspector, revision, pipe.uuid, alternative.uuid));

    // The two edits form a valid final state, even though either independent
    // pipe edit would be invalid. Indexed references must follow the change.
    HydraulicDemandPoint reassigned = point;
    reassigned.attachment.pipe_assigned_junction_uuid = alternative.uuid;
    const std::optional<QUuid> group = db.commitObjectGroup(actor, branch, generation,
        {{pipe.uuid, QStringLiteral("hydraulic.pipe"),
          ModelCodec<HydraulicLinkPipe>::encode(redirected), 1},
         {point.uuid, QStringLiteral("hydraulic.demand_point"),
          ModelCodec<HydraulicDemandPoint>::encode(reassigned), 1}}, &error);
    REQUIRE(group.has_value());
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(!hasReference(inspector, revision, pipe.uuid, original.uuid));
    REQUIRE(hasReference(inspector, revision, pipe.uuid, alternative.uuid));
    REQUIRE(hasReference(inspector, revision, point.uuid, alternative.uuid));
    REQUIRE(!hasReference(inspector, revision, point.uuid, original.uuid));

    REQUIRE(db.undoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pipe.uuid, original.uuid));
    REQUIRE(hasReference(inspector, revision, point.uuid, original.uuid));
    REQUIRE(!hasReference(inspector, revision, pipe.uuid, alternative.uuid));
    REQUIRE(db.redoObjectGroup(actor, branch, generation, &error) == group);
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, pipe.uuid, alternative.uuid));
    REQUIRE(hasReference(inspector, revision, point.uuid, alternative.uuid));

    // Target-category validation: an endpoint must be a node in this network.
    HydraulicLinkPipe bad_target = redirected;
    bad_target.node_uuid_to = point.uuid; // Exists, but is a demand point.
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, bad_target, 4, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    bad_target.node_uuid_to = createUuidV7(); // Not a member of this network.
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, branch, bad_target, 4, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);

    // A new invalid point attachment must fail without altering the journal.
    HydraulicDemandPoint wrong = reassigned;
    wrong.attachment.pipe_uuid = alternative.uuid; // Junction, not a pipe.
    const qsizetype history = db.objectHistoryForRevision(actor, revision, &error).size();
    const qint64 point_version = db.object(actor, revision, point.uuid, &error)->version;
    REQUIRE(!db.putObject(actor, branch, point.uuid, QStringLiteral("hydraulic.demand_point"),
                          ModelCodec<HydraulicDemandPoint>::encode(wrong), point_version, &error));
    REQUIRE(db.objectHistoryForRevision(actor, revision, &error).size() == history);
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);
    REQUIRE(hasReference(inspector, revision, point.uuid, pipe.uuid));

    // A legitimate standalone endpoint edit on a pipe without attached demand
    // points also updates just the changed outgoing reference.
    HydraulicLinkPipe spare;
    spare.uuid = createUuidV7();
    spare.node_uuid_from = reservoir.uuid;
    spare.node_uuid_to = original.uuid;
    network.links_pipes.append(spare);
    network.links_pipes[0] = redirected;
    network.demand_points[0] = reassigned;
    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, network, &error));
    generation = db.revision(actor, revision, &error)->generation;
    HydraulicLinkPipe spare_redirected = spare;
    spare_redirected.node_uuid_to = alternative.uuid;
    REQUIRE(aowis::db::putHydraulicEntity(db, actor, branch, spare_redirected, 1, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(hasReference(inspector, revision, spare.uuid, alternative.uuid));
    REQUIRE(!hasReference(inspector, revision, spare.uuid, original.uuid));

    // Switching the attachment to a direct junction eliminates the pipe
    // reference without rewriting unrelated reverse-reference rows.
    HydraulicDemandPoint at_junction = reassigned;
    at_junction.attachment.type = HydraulicDemandPointAttachmentType::Junction;
    at_junction.attachment.junction_uuid = alternative.uuid;
    const qint64 current_point_version = db.object(actor, revision, point.uuid, &error)->version;
    REQUIRE(db.putObject(actor, branch, point.uuid, QStringLiteral("hydraulic.demand_point"),
                         ModelCodec<HydraulicDemandPoint>::encode(at_junction),
                         current_point_version, &error));
    generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(!hasReference(inspector, revision, point.uuid, pipe.uuid));
    REQUIRE(hasReference(inspector, revision, point.uuid, alternative.uuid));

    const std::optional<NetworkHydraulic> final_state =
        db.loadHydraulicNetwork(actor, revision, network.uuid, &error);
    REQUIRE(final_state.has_value());
    REQUIRE(aowis::db::validateHydraulicNetwork(*final_state).valid());
    // Finalization must transfer exactly the validated SQL working state to
    // its successor, including its derived hydraulic graph indexes. The
    // immutable historical revision retains none of these materialized rows.
    const qint64 before_objects = rowCount(inspector, QStringLiteral("revision_objects"), revision);
    const qint64 before_members = rowCount(inspector, QStringLiteral("hydraulic_network_members"), revision);
    const qint64 before_refs = rowCount(inspector, QStringLiteral("hydraulic_network_references"), revision);
    REQUIRE(before_objects > 400 && before_members > 400 && before_refs > 0);
    const std::optional<aowis::db::Finalization> finalized =
        db.finalize(actor, branch, generation, QStringLiteral("Local structural changes"), {}, &error);
    REQUIRE(finalized.has_value());
    for (const QString &table : {QStringLiteral("revision_objects"),
                                 QStringLiteral("hydraulic_network_members"),
                                 QStringLiteral("hydraulic_network_references")})
        REQUIRE(rowCount(inspector, table, revision) == 0);
    REQUIRE(rowCount(inspector, QStringLiteral("revision_objects"),
                     finalized->new_working_revision_uuid) == before_objects);
    REQUIRE(rowCount(inspector, QStringLiteral("hydraulic_network_members"),
                     finalized->new_working_revision_uuid) == before_members);
    REQUIRE(rowCount(inspector, QStringLiteral("hydraulic_network_references"),
                     finalized->new_working_revision_uuid) == before_refs);
    QSqlQuery state_check(inspector);
    REQUIRE(state_check.exec(QStringLiteral("SELECT COUNT(*) FROM revisions WHERE state='sealing'")));
    REQUIRE(state_check.next() && state_check.value(0).toLongLong() == 0);
    state_check.finish();
    REQUIRE(hasReference(inspector, finalized->new_working_revision_uuid, point.uuid, alternative.uuid));
    REQUIRE(db.loadHydraulicNetwork(actor, finalized->new_working_revision_uuid,
                                    network.uuid, &error).has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    inspector.close();
    inspector = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("local-structure-inspector"));
    return 0;
}
