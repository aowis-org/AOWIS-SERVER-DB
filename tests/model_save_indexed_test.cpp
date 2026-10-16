#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

namespace
{
NetworkHydraulic networkWithJunctions(int number)
{
    NetworkHydraulic network;
    network.uuid = createUuidV7();
    for (int i = 0; i < number; ++i)
    {
        HydraulicNodeJunction junction;
        junction.uuid = createUuidV7();
        junction.id = QStringLiteral("J%1").arg(i);
        network.nodes_junctions.append(junction);
    }
    return network;
}

// Ensure that the primary keys used by the bounded lookup queries actually
// support UUID lookups. Do not rely on timings for a correctness test.
bool indexedObjectLookups(QSqlDatabase &database)
{
    QSqlQuery objects(database);
    if (!objects.exec(QStringLiteral(
            "EXPLAIN QUERY PLAN SELECT object_uuid,kind,content,version,deleted "
            "FROM revision_objects WHERE revision_uuid=x'00' AND object_uuid IN (x'01',x'02')")))
        return false;
    bool object_indexed = false;
    while (objects.next())
    {
        const QString plan = objects.value(3).toString().toUpper();
        object_indexed |= plan.contains(QStringLiteral("SEARCH")) &&
                          plan.contains(QStringLiteral("INDEX"));
    }
    QSqlQuery members(database);
    if (!members.exec(QStringLiteral(
            "EXPLAIN QUERY PLAN SELECT network_uuid FROM hydraulic_network_members "
            "WHERE revision_uuid=x'00' AND entity_uuid IN (x'01',x'02')")))
        return false;
    bool member_indexed = false;
    while (members.next())
    {
        const QString plan = members.value(3).toString().toUpper();
        member_indexed |= plan.contains(QStringLiteral("SEARCH")) &&
                          (plan.contains(QStringLiteral("PRIMARY KEY")) ||
                           plan.contains(QStringLiteral("INDEX")));
    }
    return object_indexed && member_indexed;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("indexed-save.sqlite"));

    Database db;
    REQUIRE(db.openSqlite(file, &error));
    const std::optional<aowis::db::UserInfo> admin =
        db.bootstrapAdministrator(QStringLiteral("root"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        db.createProject(actor, QStringLiteral("Indexed saves"), &error);
    REQUIRE(project.has_value());
    const QUuid branch = project->main_branch_uuid;
    const QUuid revision = project->working_revision_uuid;

    // More than the 300-UUID query batch size. One revision can contain many
    // networks and non-network objects that an individual save must not load.
    NetworkHydraulic large = networkWithJunctions(345);
    NetworkHydraulic foreign = networkWithJunctions(9);
    REQUIRE(db.saveHydraulicNetwork(actor, branch, 0, large, &error));
    qint64 generation = db.revision(actor, revision, &error)->generation;
    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, foreign, &error));
    generation = db.revision(actor, revision, &error)->generation;
    const QUuid generic_uuid = createUuidV7();
    REQUIRE(db.putObject(actor, branch, generic_uuid,
                         QStringLiteral("generic.annotation"), QByteArray("keep"), -1, &error));
    generation = db.revision(actor, revision, &error)->generation;

    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, large, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == generation);

    // A scalar edit must not rewrite hundreds of unchanged ownership rows.
    // A temporary SQL-level guard turns accidental full membership rebuilds
    // into a test failure rather than depending on timing measurements.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("member-rewrite-check"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery sql(inspector);
        REQUIRE(sql.exec(QStringLiteral(
            "CREATE TRIGGER prevent_member_rewrite BEFORE DELETE "
            "ON hydraulic_network_members BEGIN "
            "SELECT RAISE(ABORT,'unchanged membership was rewritten'); END")));
        sql.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("member-rewrite-check"));
    large.nodes_junctions.first().elevation_m = 332.125;
    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, large, &error));
    generation = db.revision(actor, revision, &error)->generation;
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("member-rewrite-drop"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        QSqlQuery sql(inspector);
        REQUIRE(sql.exec(QStringLiteral("DROP TRIGGER prevent_member_rewrite")));
        sql.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("member-rewrite-drop"));

    // Removing a non-referenced junction changes the root index, and must
    // rewrite this network's membership but not any other network's members.
    const QUuid removed_uuid = large.nodes_junctions.last().uuid;
    large.nodes_junctions.removeLast();
    REQUIRE(db.saveHydraulicNetwork(actor, branch, generation, large, &error));
    const qint64 after = db.revision(actor, revision, &error)->generation;
    REQUIRE(after == generation + 2); // one root update and one tombstone
    REQUIRE(db.object(actor, revision, removed_uuid, &error)->deleted);
    REQUIRE(db.object(actor, revision, generic_uuid, &error)->content == QByteArray("keep"));
    const std::optional<NetworkHydraulic> reloaded =
        db.loadHydraulicNetwork(actor, revision, large.uuid, &error);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->nodes_junctions.size() == 344);
    REQUIRE(reloaded->nodes_junctions.first().elevation_m == 332.125);
    REQUIRE(db.loadHydraulicNetwork(actor, revision, foreign.uuid, &error)->nodes_junctions.size() == 9);

    // Indexed ownership must catch a cross-network entity collision, including
    // when a foreign entity UUID is reused as a proposed *network root*.
    NetworkHydraulic stolen = networkWithJunctions(1);
    stolen.nodes_junctions.first().uuid = foreign.nodes_junctions.first().uuid;
    REQUIRE(!db.saveHydraulicNetwork(actor, branch, after, stolen, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == after);
    stolen.nodes_junctions.first().uuid = createUuidV7();
    stolen.uuid = foreign.nodes_junctions.first().uuid;
    REQUIRE(!db.saveHydraulicNetwork(actor, branch, after, stolen, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == after);

    // An unrelated stored UUID is not available to be stolen by a network.
    stolen.uuid = generic_uuid;
    REQUIRE(!db.saveHydraulicNetwork(actor, branch, after, stolen, &error));
    REQUIRE(db.revision(actor, revision, &error)->generation == after);

    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("indexed-save-inspector"));
        inspector.setDatabaseName(file);
        REQUIRE(inspector.open());
        REQUIRE(indexedObjectLookups(inspector));
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("indexed-save-inspector"));

    const std::optional<aowis::db::Finalization> sealed = db.finalize(
        actor, branch, after, QStringLiteral("Indexed full save"), {}, &error);
    REQUIRE(sealed.has_value());
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.loadHydraulicNetwork(actor, sealed->new_working_revision_uuid,
                                    foreign.uuid, &error).has_value());
    db.close();
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
