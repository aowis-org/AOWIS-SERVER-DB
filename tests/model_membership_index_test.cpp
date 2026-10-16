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

    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("index-check"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, first.uuid) == 3);
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, second.uuid) == 3);
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

    // Simulate a historical V5 database containing real finalized network
    // objects: drop only the derived V6 index, retaining every hashed byte.
    db.close();
    {
        QSqlDatabase older = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("v5-backfill"));
        older.setDatabaseName(path);
        REQUIRE(older.open());
        QSqlQuery sql(older);
        REQUIRE(sql.exec(QStringLiteral("DROP TABLE security_audit_events")));
        // Verify the synthetic legacy fixture no longer contains V9 objects;
        // otherwise re-running the V9 migration must reject this database.
        REQUIRE(sql.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
            "AND name='security_audit_events'")));
        REQUIRE(sql.next() && sql.value(0).toInt() == 0);
        sql.finish();
        REQUIRE(sql.exec(QStringLiteral("DROP TABLE user_sessions")));
        REQUIRE(sql.exec(QStringLiteral("DROP TABLE hydraulic_network_references")));
        REQUIRE(sql.exec(QStringLiteral("DROP TABLE hydraulic_network_members")));
        REQUIRE(sql.exec(QStringLiteral("PRAGMA user_version=5")));
        sql.finish();
        older.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("v5-backfill"));

    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.loadHydraulicNetwork(actor, sealed->new_working_revision_uuid, first.uuid, &error).has_value());
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("index-backfill"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        REQUIRE(membershipCount(inspection, project->working_revision_uuid, first.uuid) == 3);
        REQUIRE(membershipCount(inspection, sealed->new_working_revision_uuid, second.uuid) == 3);
        REQUIRE(membershipCount(inspection, branch->head_revision_uuid, second.uuid) == 3);
        QSqlQuery query(inspection);
        REQUIRE(query.prepare(QStringLiteral(
            "DELETE FROM hydraulic_network_members WHERE revision_uuid=?")));
        query.addBindValue(project->working_revision_uuid.toRfc4122());
        REQUIRE(!query.exec()); // Finalized revisions retain immutable membership.
        query.finish();
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("index-backfill"));
    return 0;
}
