#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        qCritical().noquote() << __FILE__ << __LINE__ << #condition << error; \
        return 1; \
    } \
} while (false)

namespace
{
NetworkHydraulic makeNetwork()
{
    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir source;
    source.uuid = createUuidV7();
    HydraulicNodeJunction destination;
    destination.uuid = createUuidV7();
    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.node_uuid_from = source.uuid;
    pipe.node_uuid_to = destination.uuid;
    network.nodes_reservoirs.append(source);
    network.nodes_junctions.append(destination);
    network.links_pipes.append(pipe);
    return network;
}

int membershipCount(QSqlDatabase &inspection, const QUuid &revision_uuid,
                    const QUuid &network_uuid)
{
    QSqlQuery query(inspection);
    if (!query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_members WHERE revision_uuid=? AND network_uuid=?")))
        return -1;
    query.addBindValue(revision_uuid.toRfc4122());
    query.addBindValue(network_uuid.toRfc4122());
    if (!query.exec() || !query.next())
        return -1;
    return query.value(0).toInt();
}
// The SQLite adapter must continue storing RFC-4122 BLOBs after the model
// persistence paths have been routed through StorageBackend::bindUuid().
bool membershipUuidStorage(QSqlDatabase &inspection, const QUuid &revision_uuid,
                           const QUuid &network_uuid, const QUuid &entity_uuid)
{
    QSqlQuery query(inspection);
    if (!query.prepare(QStringLiteral(
            "SELECT typeof(revision_uuid),revision_uuid,typeof(network_uuid),network_uuid,"
            "typeof(entity_uuid),entity_uuid FROM hydraulic_network_members "
            "WHERE revision_uuid=? AND network_uuid=? AND entity_uuid=?")))
        return false;
    query.addBindValue(revision_uuid.toRfc4122());
    query.addBindValue(network_uuid.toRfc4122());
    query.addBindValue(entity_uuid.toRfc4122());
    if (!query.exec() || !query.next())
        return false;
    return query.value(0).toString() == QStringLiteral("blob") &&
           query.value(1).toByteArray() == revision_uuid.toRfc4122() &&
           query.value(2).toString() == QStringLiteral("blob") &&
           query.value(3).toByteArray() == network_uuid.toRfc4122() &&
           query.value(4).toString() == QStringLiteral("blob") &&
           query.value(5).toByteArray() == entity_uuid.toRfc4122();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("membership.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(actor, QStringLiteral("Networks"), &error);
    REQUIRE(project.has_value());
    const NetworkHydraulic first = makeNetwork();
    const NetworkHydraulic second = makeNetwork();
    REQUIRE(db.saveHydraulicNetwork(actor, project->main_branch_uuid, 0, first, &error));
    const qint64 first_generation = db.revision(actor, project->working_revision_uuid, &error)->generation;
    REQUIRE(db.saveHydraulicNetwork(actor, project->main_branch_uuid, first_generation, second, &error));
    // The second wholly new network uses the same bulk insert path even when
    // the revision already has other networks and a nonzero journal sequence.
    const QVector<aowis::db::ObjectChange> initial_history =
        db.objectHistoryForRevision(actor, project->working_revision_uuid, &error);
    REQUIRE(initial_history.size() == 8);
    for (qsizetype position = 0; position < initial_history.size(); ++position)
    {
        REQUIRE(initial_history.at(position).sequence == position + 1);
        REQUIRE(initial_history.at(position).actor_uuid == actor.actor_uuid);
        REQUIRE(initial_history.at(position).before_version == -1);
        REQUIRE(initial_history.at(position).after_version == 1);
    }
    const QVector<aowis::db::ObjectGroup> initial_groups =
        db.objectGroupsForRevision(actor, project->working_revision_uuid, &error);
    REQUIRE(initial_groups.size() == 2);
    REQUIRE(initial_groups.at(0).object_event_uuids.size() == 4);
    REQUIRE(initial_groups.at(1).object_event_uuids.size() == 4);

    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("index-check"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, first.uuid) == 3);
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, second.uuid) == 3);
        REQUIRE(membershipUuidStorage(inspection, project->working_revision_uuid,
                                      first.uuid, first.links_pipes.first().uuid));
        REQUIRE(membershipUuidStorage(inspection, project->working_revision_uuid,
                                      second.uuid, second.nodes_junctions.first().uuid));
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("index-check"));

    // An edit to the first network must still be rejected; the second network
    // must be untouched even though both networks share the same revision.
    HydraulicLinkPipe invalid = first.links_pipes.first();
    invalid.node_uuid_to = createUuidV7();
    REQUIRE(!aowis::db::putHydraulicEntity(db, actor, project->main_branch_uuid,
                                            invalid, 1, &error));
    REQUIRE(db.loadHydraulicNetwork(actor, project->working_revision_uuid, second.uuid, &error).has_value());

    // Undo of an entire network creates tombstones and removes only its
    // membership rows. Redo reconstructs both the root and its ownership.
    const qint64 before_undo = db.revision(actor, project->working_revision_uuid, &error)->generation;
    const std::optional<QUuid> undone_group = db.undoObjectGroup(actor,
        project->main_branch_uuid, before_undo, &error);
    REQUIRE(undone_group.has_value());
    REQUIRE(!db.loadHydraulicNetwork(actor, project->working_revision_uuid, second.uuid, &error));
    REQUIRE(db.loadHydraulicNetwork(actor, project->working_revision_uuid, first.uuid, &error).has_value());
    REQUIRE(db.redoObjectGroup(actor, project->main_branch_uuid,
        db.revision(actor, project->working_revision_uuid, &error)->generation, &error) == undone_group);
    REQUIRE(db.loadHydraulicNetwork(actor, project->working_revision_uuid, second.uuid, &error).has_value());

    const qint64 before_finalize = db.revision(actor, project->working_revision_uuid, &error)->generation;
    const std::optional<aowis::db::Finalization> sealed = db.finalize(actor, project->main_branch_uuid,
        before_finalize, QStringLiteral("Two networks"), {}, &error);
    REQUIRE(sealed.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    const std::optional<aowis::db::BranchInfo> branch = db.createBranch(actor, project->project_uuid,
        QStringLiteral("design-2"), project->working_revision_uuid, &error);
    REQUIRE(branch.has_value());

    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("index-clone"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, sealed->new_working_revision_uuid, first.uuid) == 3);
        REQUIRE(membershipCount(inspection, branch->head_revision_uuid, second.uuid) == 3);
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("index-clone"));

    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.loadHydraulicNetwork(actor, sealed->new_working_revision_uuid, first.uuid, &error).has_value());
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
