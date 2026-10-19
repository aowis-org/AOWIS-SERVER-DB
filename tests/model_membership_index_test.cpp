#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
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
NetworkHydraulic makeNetwork(int pipe_count = 1)
{
    NetworkHydraulic network;
    network.uuid = createUuidV7();
    HydraulicNodeReservoir source;
    source.uuid = createUuidV7();
    network.nodes_reservoirs.append(source);
    QUuid previous = source.uuid;
    for (int index = 0; index < pipe_count; ++index)
    {
        HydraulicNodeJunction junction;
        junction.uuid = createUuidV7();
        junction.id = QStringLiteral("junction-%1").arg(index);
        network.nodes_junctions.append(junction);
        HydraulicLinkPipe pipe;
        pipe.uuid = createUuidV7();
        pipe.id = QStringLiteral("pipe-%1").arg(index);
        pipe.node_uuid_from = previous;
        pipe.node_uuid_to = junction.uuid;
        network.links_pipes.append(pipe);
        previous = junction.uuid;
    }
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

    // Finalized indexes are not materialized at all. Forks reconstruct
    // transient working indexes from the authoritative Merkle snapshot.
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("index-finalized-absence"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, first.uuid) == 0);
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, second.uuid) == 0);
        QSqlQuery count(inspection);
        REQUIRE(count.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_references WHERE revision_uuid=?")));
        count.addBindValue(project->working_revision_uuid.toRfc4122());
        REQUIRE(count.exec() && count.next() && count.value(0).toInt() == 0);
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("index-finalized-absence"));

    const std::optional<aowis::db::BranchInfo> reconstructed = db.createBranch(
        actor, project->project_uuid, QStringLiteral("merkle-index-rebuild"),
        project->working_revision_uuid, &error);
    REQUIRE(reconstructed.has_value());
    REQUIRE(db.loadHydraulicNetwork(actor, reconstructed->head_revision_uuid,
                                    first.uuid, &error).has_value());
    REQUIRE(db.loadHydraulicNetwork(actor, reconstructed->head_revision_uuid,
                                    second.uuid, &error).has_value());
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("index-rebuilt-check"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, reconstructed->head_revision_uuid, first.uuid) == 3);
        REQUIRE(membershipCount(inspection, reconstructed->head_revision_uuid, second.uuid) == 3);
        QSqlQuery count(inspection);
        REQUIRE(count.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM hydraulic_network_references WHERE revision_uuid=?")));
        count.addBindValue(reconstructed->head_revision_uuid.toRfc4122());
        REQUIRE(count.exec() && count.next() && count.value(0).toInt() > 0);
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("index-rebuilt-check"));

    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.loadHydraulicNetwork(actor, sealed->new_working_revision_uuid, first.uuid, &error).has_value());

    // A fresh fork has no hydraulic membership index while it reconstructs
    // the network. Exercise multiple 300-key fallback batches, then verify
    // that only the newly created editable head holds SQL index rows.
    const std::optional<aowis::db::ProjectCreation> large_project =
        db.createProject(actor, QStringLiteral("Large fork"), &error);
    REQUIRE(large_project.has_value());
    const NetworkHydraulic large_network = makeNetwork(330);
    REQUIRE(db.saveHydraulicNetwork(actor, large_project->main_branch_uuid,
                                    0, large_network, &error));
    const qint64 large_generation = db.revision(
        actor, large_project->working_revision_uuid, &error)->generation;
    const std::optional<aowis::db::Finalization> large_seal = db.finalize(
        actor, large_project->main_branch_uuid, large_generation,
        QStringLiteral("Large baseline"), {}, &error);
    REQUIRE(large_seal.has_value());
    const std::optional<aowis::db::BranchInfo> large_fork = db.createBranch(
        actor, large_project->project_uuid, QStringLiteral("large-fork"),
        large_seal->finalized_revision_uuid, &error);
    REQUIRE(large_fork.has_value());
    const std::optional<NetworkHydraulic> forked_model = db.loadHydraulicNetwork(
        actor, large_fork->head_revision_uuid, large_network.uuid, &error);
    REQUIRE(forked_model.has_value());
    REQUIRE(forked_model->nodes_junctions.size() == 330);
    REQUIRE(forked_model->links_pipes.size() == 330);
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("large-index-check"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, large_seal->finalized_revision_uuid,
                                large_network.uuid) == 0);
        REQUIRE(membershipCount(inspection, large_fork->head_revision_uuid,
                                large_network.uuid) == 661);
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("large-index-check"));
    REQUIRE(db.verifyProject(actor, large_project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
