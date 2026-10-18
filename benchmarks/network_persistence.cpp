#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#include <optional>

// An opt-in measurement tool, not a pass/fail CTest. Timing depends on the
// filesystem, host hardware, Qt build, network size and database contents.
// Keep this workload realistic but deterministic in structure: one reservoir,
// N junctions and N pipes. Every entity gets its normal UUIDv7 identity.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream output(stdout);
    QTextStream errors(stderr);
    const QStringList arguments = app.arguments();
    if (arguments.size() > 2 || (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--help")))
    {
        output << "Usage: aowis-db-network-benchmark [PIPE_COUNT]\n"
                  "Default PIPE_COUNT: 1000; allowed range: 1..100000.\n"
                  "Uses an isolated temporary SQLite database.\n";
        return arguments.size() == 2 && arguments.at(1) == QStringLiteral("--help") ? 0 : 2;
    }
    bool number_ok = true;
    const int count = arguments.size() == 2 ? arguments.at(1).toInt(&number_ok) : 1000;
    if (!number_ok || count < 1 || count > 100000)
    {
        errors << "PIPE_COUNT must be an integer between 1 and 100000.\n";
        return 2;
    }

    QString error;
    const auto fail = [&errors, &error](const QString &step) {
        errors << step << " failed: " << error << "\n";
        return 1;
    };

    QTemporaryDir directory;
    if (!directory.isValid())
    {
        errors << "Cannot create a temporary benchmark directory.\n";
        return 1;
    }
    const QString db_path = directory.filePath(QStringLiteral("network.sqlite"));
    aowis::db::Database database;
    if (!database.openSqlite(db_path, &error))
        return fail(QStringLiteral("Open SQLite"));
    const std::optional<aowis::db::UserInfo> administrator =
        database.bootstrapAdministrator(QStringLiteral("benchmark-admin"), &error);
    if (!administrator)
        return fail(QStringLiteral("Bootstrap administrator"));
    const aowis::db::OperationContext actor{administrator->uuid};
    const std::optional<aowis::db::ProjectCreation> project =
        database.createProject(actor, QStringLiteral("Network benchmark"), &error);
    if (!project)
        return fail(QStringLiteral("Create project"));

    NetworkHydraulic network;
    network.uuid = createUuidV7();
    network.id = QStringLiteral("benchmark-network");
    network.nodes_junctions.reserve(count);
    network.links_pipes.reserve(count);
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    reservoir.id = QStringLiteral("source");
    network.nodes_reservoirs.append(reservoir);
    QUuid previous_node = reservoir.uuid;
    for (int index = 0; index < count; ++index)
    {
        HydraulicNodeJunction junction;
        junction.uuid = createUuidV7();
        junction.id = QStringLiteral("junction-%1").arg(index);
        junction.elevation_m = 100.0 + static_cast<double>(index % 100);
        network.nodes_junctions.append(junction);

        HydraulicLinkPipe pipe;
        pipe.uuid = createUuidV7();
        pipe.id = QStringLiteral("pipe-%1").arg(index);
        pipe.node_uuid_from = previous_node;
        pipe.node_uuid_to = junction.uuid;
        pipe.length_measured_m = 150.0;
        network.links_pipes.append(pipe);
        previous_node = junction.uuid;
    }

    QElapsedTimer timer;
    timer.start();
    if (!database.saveHydraulicNetwork(actor, project->main_branch_uuid, 0, network, &error))
        return fail(QStringLiteral("Save network"));
    const qint64 initial_save_ms = timer.elapsed();

    timer.restart();
    const std::optional<NetworkHydraulic> loaded = database.loadHydraulicNetwork(
        actor, project->working_revision_uuid, network.uuid, &error);
    if (!loaded || loaded->links_pipes.size() != count ||
        loaded->nodes_junctions.size() != count)
        return fail(QStringLiteral("Load network"));
    const qint64 load_ms = timer.elapsed();

    const std::optional<aowis::db::RevisionInfo> before = database.revision(
        actor, project->working_revision_uuid, &error);
    if (!before)
        return fail(QStringLiteral("Read revision"));
    timer.restart();
    if (!database.saveHydraulicNetwork(actor, project->main_branch_uuid,
                                       before->generation, network, &error))
        return fail(QStringLiteral("Save unchanged network"));
    const qint64 unchanged_save_ms = timer.elapsed();

    HydraulicNodeJunction updated_junction = network.nodes_junctions.first();
    updated_junction.elevation_m += 1.0;
    timer.restart();
    if (!aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid,
                                      updated_junction, 1, &error))
        return fail(QStringLiteral("Edit one junction"));
    const qint64 single_edit_ms = timer.elapsed();

    const std::optional<aowis::db::RevisionInfo> edited = database.revision(
        actor, project->working_revision_uuid, &error);
    if (!edited)
        return fail(QStringLiteral("Read edited revision"));
    timer.restart();
    const std::optional<aowis::db::Finalization> finalization = database.finalize(
        actor, project->main_branch_uuid, edited->generation,
        QStringLiteral("Benchmark baseline"), QString(), &error);
    if (!finalization)
        return fail(QStringLiteral("Finalize revision"));
    const qint64 finalize_ms = timer.elapsed();

    timer.restart();
    const std::optional<aowis::db::BranchInfo> branch = database.createBranch(
        actor, project->project_uuid, QStringLiteral("alternative"),
        finalization->finalized_revision_uuid, &error);
    if (!branch)
        return fail(QStringLiteral("Create branch"));
    const qint64 branch_ms = timer.elapsed();

    timer.restart();
    const aowis::db::IntegrityReport report = database.verifyProject(
        actor, project->project_uuid);
    if (!report.valid || report.checked_revisions != 1)
    {
        error = report.error;
        return fail(QStringLiteral("Verify finalized history"));
    }
    const qint64 verify_ms = timer.elapsed();

    database.close();
    const qint64 file_bytes = QFileInfo(db_path).size();
    output << "backend=sqlite\n"
           << "pipes=" << count << "\n"
           << "junctions=" << count << "\n"
           << "initial_save_ms=" << initial_save_ms << "\n"
           << "load_ms=" << load_ms << "\n"
           << "unchanged_save_ms=" << unchanged_save_ms << "\n"
           << "single_entity_edit_ms=" << single_edit_ms << "\n"
           << "finalize_ms=" << finalize_ms << "\n"
           << "create_branch_ms=" << branch_ms << "\n"
           << "verify_history_ms=" << verify_ms << "\n"
           << "sqlite_file_bytes=" << file_bytes << "\n";
    output.flush();
    return 0;
}
