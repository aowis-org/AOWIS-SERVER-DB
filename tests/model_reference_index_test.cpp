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
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("references-old-v6"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery query(inspector);
        REQUIRE(query.exec(QStringLiteral("DROP TABLE security_audit_events")));
        // Confirm the fixture has removed the V9 journal before migrating.
        REQUIRE(query.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
            "AND name='security_audit_events'")));
        REQUIRE(query.next() && query.value(0).toInt() == 0);
        query.finish();
        REQUIRE(query.exec(QStringLiteral("DROP TABLE user_sessions")));
        REQUIRE(query.exec(QStringLiteral("DROP TABLE hydraulic_network_references")));
        REQUIRE(query.exec(QStringLiteral("PRAGMA user_version=6")));
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("references-old-v6"));
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("references-migrated"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        REQUIRE(referenceCount(inspector, revision, alternative.uuid) == 2);
        REQUIRE(referenceCount(inspector, sealed->new_working_revision_uuid, alternative.uuid) == 2);
        QSqlQuery query(inspector);
        REQUIRE(query.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_references WHERE revision_uuid=?")));
        query.addBindValue(revision.toRfc4122());
        REQUIRE(!query.exec()); // finalized reference index cannot be rewritten
        query.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("references-migrated"));
    return 0;
}
