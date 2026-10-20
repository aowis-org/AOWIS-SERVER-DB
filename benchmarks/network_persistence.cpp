#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <QFileInfo>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <QStringList>
#include <QtGlobal>

#include <cstdio>
#include <optional>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace
{
// The environment variable deliberately identifies a dedicated *test*
// database, never AOWIS's configured operational database. Only the new,
// randomly named benchmark schema is created and dropped.
struct StorageCounts
{
    qint64 working_objects = -1;
    qint64 network_members = -1;
    qint64 network_references = -1;
    qint64 object_events = -1;
    qint64 revision_rows = -1;
    qint64 content_blobs = -1;
    qint64 content_tree_nodes = -1;
    qint64 content_payload_bytes = -1;
};

// Read-only measurements, deliberately outside timed model operations.
// Relation and column names are static; only the internally generated,
// validated temporary PostgreSQL schema name becomes part of a query.
std::optional<StorageCounts> collectStorageCounts(QSqlDatabase &connection,
    const QString &table_prefix, bool postgres, QString *error)
{
    StorageCounts result;
    const auto count = [&connection, &table_prefix, error](const QString &table,
                                                            qint64 *value) -> bool {
        QSqlQuery query(connection);
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1%2")
                            .arg(table_prefix, table)) || !query.next())
        {
            *error = query.lastError().text();
            return false;
        }
        *value = query.value(0).toLongLong();
        return true;
    };
    if (!count(QStringLiteral("revision_objects"), &result.working_objects) ||
        !count(QStringLiteral("hydraulic_network_members"), &result.network_members) ||
        !count(QStringLiteral("hydraulic_network_references"), &result.network_references) ||
        !count(QStringLiteral("object_events"), &result.object_events) ||
        !count(QStringLiteral("revisions"), &result.revision_rows))
        return std::nullopt;
    QSqlQuery content(connection);
    const QString length_function = postgres
        ? QStringLiteral("OCTET_LENGTH") : QStringLiteral("LENGTH");
    if (!content.exec(QStringLiteral(
            "SELECT kind, COUNT(*), COALESCE(SUM(%1(payload)), 0) "
            "FROM %2content_objects GROUP BY kind").arg(length_function, table_prefix)))
    {
        *error = content.lastError().text();
        return std::nullopt;
    }
    result.content_blobs = 0;
    result.content_tree_nodes = 0;
    result.content_payload_bytes = 0;
    while (content.next())
    {
        const QString kind = content.value(0).toString();
        if (kind == QStringLiteral("blob"))
            result.content_blobs = content.value(1).toLongLong();
        else if (kind == QStringLiteral("tree"))
            result.content_tree_nodes = content.value(1).toLongLong();
        else
        {
            *error = QStringLiteral("Unexpected content kind: %1").arg(kind);
            return std::nullopt;
        }
        result.content_payload_bytes += content.value(2).toLongLong();
    }
    if (content.lastError().isValid())
    {
        *error = content.lastError().text();
        return std::nullopt;
    }
    return result;
}

class TemporaryPostgresSchema final
{
public:
    ~TemporaryPostgresSchema()
    {
        if (this->created_ && this->connection_.isOpen())
        {
            // DROP ... CASCADE lists every schema object as a NOTICE.
            // Avoid hundreds of distracting lines in benchmark output.
            QSqlQuery quiet(this->connection_);
            quiet.exec(QStringLiteral("SET client_min_messages TO warning"));
            QSqlQuery drop(this->connection_);
            if (!drop.exec(QStringLiteral("DROP SCHEMA %1 CASCADE").arg(this->schema_name_)))
                qWarning().noquote() << "Cannot remove temporary benchmark schema:"
                                     << this->schema_name_ << drop.lastError().text();
        }
        if (this->connection_.isValid())
        {
            this->connection_.close();
            this->connection_ = QSqlDatabase();
            QSqlDatabase::removeDatabase(this->connection_name_);
        }
    }

    bool create(QString *error)
    {
        const QByteArray selected_database = qgetenv("AOWIS_DB_TEST_PG_DB");
        if (selected_database.isEmpty())
        {
            *error = QStringLiteral("--postgres requires AOWIS_DB_TEST_PG_DB for a dedicated disposable test database");
            return false;
        }
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QPSQL")))
        {
            *error = QStringLiteral("Qt QPSQL driver is unavailable");
            return false;
        }
        this->database_name_ = QString::fromUtf8(selected_database);
        this->schema_name_ = QStringLiteral("aowis_benchmark_") +
            createUuidV7().toString(QUuid::Id128);
        this->connection_name_ = QStringLiteral("aowis-benchmark-schema-") +
            createUuidV7().toString(QUuid::Id128);
        this->connection_ = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), this->connection_name_);
        this->connection_.setDatabaseName(this->database_name_);
        // libpq reads PGHOST, PGPORT, PGUSER, PGPASSWORD and related settings.
        if (!this->connection_.open())
        {
            *error = this->connection_.lastError().text();
            return false;
        }
        QSqlQuery create(this->connection_);
        if (!create.exec(QStringLiteral("CREATE SCHEMA %1").arg(this->schema_name_)))
        {
            *error = create.lastError().text();
            return false;
        }
        this->created_ = true;
        return true;
    }

    QString databaseName() const { return this->database_name_; }
    QString schemaName() const { return this->schema_name_; }

    std::optional<StorageCounts> storageCounts(QString *error)
    {
        return collectStorageCounts(this->connection_, this->schema_name_ + QLatin1Char('.'),
                                    true, error);
    }

    std::optional<qint64> storageBytes(QString *error) const
    {
        QSqlQuery size(this->connection_);
        if (!size.prepare(QStringLiteral(
                "SELECT COALESCE(SUM(pg_total_relation_size(c.oid)), 0) "
                "FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid "
                "WHERE n.nspname = ? AND c.relkind IN ('r', 'm')")))
        {
            *error = size.lastError().text();
            return std::nullopt;
        }
        size.addBindValue(this->schema_name_);
        if (!size.exec() || !size.next())
        {
            *error = size.lastError().text();
            return std::nullopt;
        }
        return size.value(0).toLongLong();
    }

private:
    QString database_name_;
    QString schema_name_;
    QString connection_name_;
    QSqlDatabase connection_;
    bool created_ = false;
};

std::optional<StorageCounts> sqliteStorageCounts(const QString &path, QString *error)
{
    const QString connection_name = QStringLiteral("aowis-benchmark-count-") +
        createUuidV7().toString(QUuid::Id128);
    std::optional<StorageCounts> result;
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        connection.setDatabaseName(path);
        if (!connection.open())
            *error = connection.lastError().text();
        else
            result = collectStorageCounts(connection, QString(), false, error);
        connection.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return result;
}
}

namespace
{
struct ProcessMemory
{
    qint64 resident_kb = -1;
    qint64 high_water_kb = -1;
};

// Procfs files may report a length of zero even though they are readable.
// QFile::atEnd() before the first read can therefore suppress the *entire*
// scan on some Qt versions. Use stdio streaming rather than a size/EOF check.
// Linux ru_maxrss gives an independent, kernel-maintained peak in KiB.
ProcessMemory processMemory()
{
    ProcessMemory result;
#ifdef Q_OS_LINUX
    if (std::FILE *status = std::fopen("/proc/self/status", "r"))
    {
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), status) != nullptr)
        {
            long long value = -1;
            if (std::sscanf(buffer, "VmRSS: %lld kB", &value) == 1)
                result.resident_kb = value;
            else if (std::sscanf(buffer, "VmHWM: %lld kB", &value) == 1)
                result.high_water_kb = value;
        }
        std::fclose(status);
    }
    if (result.resident_kb < 0)
    {
        // /proc/self/statm reports pages. No Qt file-size or cached EOF
        // behavior is involved in the fallback either.
        if (std::FILE *statm = std::fopen("/proc/self/statm", "r"))
        {
            unsigned long long pages = 0;
            unsigned long long resident_pages = 0;
            if (std::fscanf(statm, "%llu %llu", &pages, &resident_pages) == 2)
            {
                const long page_bytes = sysconf(_SC_PAGESIZE);
                if (page_bytes > 0)
                    result.resident_kb = static_cast<qint64>(resident_pages) * page_bytes / 1024;
            }
            std::fclose(statm);
        }
    }
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        const qint64 peak = static_cast<qint64>(usage.ru_maxrss);
        if (result.high_water_kb < peak)
            result.high_water_kb = peak;
    }
#endif
    return result;
}
}

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
    bool postgres = false;
    bool profile_phases = false;
    bool measure_memory = false;
    bool deep_verify = false;
    bool has_count = false;
    bool compare_branches = false;
    bool edits_option_seen = false;
    int changes_per_revision = 1;
    int count = 1000;
    int revision_count = 1;
    bool revisions_option_seen = false;
    for (int index = 1; index < arguments.size(); ++index)
    {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--help"))
        {
            output << "Usage: aowis-db-network-benchmark [PIPE_COUNT] [--revisions N] [--edits N] [--compare] [--postgres] [--profile] [--memory] [--deep-verify]\n"
                      "Default PIPE_COUNT: 1000; allowed range: 1..100000.\n"
                      "Default revisions: 1; allowed range: 1..100 finalized revisions.\n"
                      "--edits N changes N distinct junctions in each revision (default: 1).\n"
                      "--compare creates a divergent finalized branch, compares ancestry,\n"
                      "and measures a read-only three-way merge preview.\n"
                      "SQLite uses a temporary file. PostgreSQL requires a dedicated\n"
                      "AOWIS_DB_TEST_PG_DB and libpq PG* connection settings;\n"
                      "a random temporary schema is created and deleted.\n"
                      "--profile prints Merkle, network save and load timings to stderr.\n"
                      "--memory prints current and peak process RSS at workload checkpoints.\n"
                      "--deep-verify also replays object journals against Merkle revisions.\n";
            return 0;
        }
        if (argument == QStringLiteral("--compare") && !compare_branches)
        {
            compare_branches = true;
            continue;
        }
        if (argument == QStringLiteral("--edits") && !edits_option_seen)
        {
            edits_option_seen = true;
            bool edits_ok = false;
            if (index + 1 < arguments.size())
                changes_per_revision = arguments.at(++index).toInt(&edits_ok);
            if (!edits_ok || changes_per_revision < 1 || changes_per_revision > 100000)
            {
                errors << "--edits requires an integer between 1 and 100000.\n";
                return 2;
            }
            continue;
        }
        if (argument == QStringLiteral("--profile") && !profile_phases)
        {
            profile_phases = true;
            continue;
        }
        if (argument == QStringLiteral("--memory") && !measure_memory)
        {
            measure_memory = true;
            continue;
        }
        if (argument == QStringLiteral("--deep-verify") && !deep_verify)
        {
            deep_verify = true;
            continue;
        }
        if (argument == QStringLiteral("--postgres") && !postgres)
        {
            postgres = true;
            continue;
        }
        if (argument == QStringLiteral("--revisions") && !revisions_option_seen)
        {
            revisions_option_seen = true;
            bool revisions_ok = false;
            if (index + 1 < arguments.size())
                revision_count = arguments.at(++index).toInt(&revisions_ok);
            if (!revisions_ok || revision_count < 1 || revision_count > 100)
            {
                errors << "--revisions requires an integer between 1 and 100.\n";
                return 2;
            }
            continue;
        }
        bool number_ok = false;
        const int parsed_count = argument.toInt(&number_ok);
        if (has_count || !number_ok || parsed_count < 1 || parsed_count > 100000)
        {
            errors << "Usage: aowis-db-network-benchmark [PIPE_COUNT] [--revisions N] [--edits N] [--compare] [--postgres] [--profile] [--memory] [--deep-verify]\n"
                      "PIPE_COUNT must be an integer between 1 and 100000.\n";
            return 2;
        }
        has_count = true;
        count = parsed_count;
    }

    if (changes_per_revision > count)
    {
        errors << "--edits cannot exceed PIPE_COUNT.\n";
        return 2;
    }
    if (compare_branches && (revision_count < 2 || count < 2))
    {
        errors << "--compare requires --revisions 2 or greater and PIPE_COUNT 2 or greater.\n";
        return 2;
    }

    // This benchmark is single-process and uses disposable databases. Enable
    // the core's diagnostics before opening a database; normal test runs and
    // server processes keep profiling disabled by default.
    if (profile_phases)
    {
        qputenv("AOWIS_DB_PROFILE_PHASES", "1");
        qputenv("AOWIS_DB_PROFILE_SAVE", "1");
        qputenv("AOWIS_DB_PROFILE_LOAD", "1");
    }

#ifndef Q_OS_LINUX
    if (measure_memory)
    {
        errors << "--memory currently requires Linux /proc/self/status.\n";
        return 2;
    }
#endif

    // One line per stage, immediately to stderr: if the OS terminates a
    // 100k-pipe run, its last successful checkpoint is still available.
    const auto checkpoint = [measure_memory](const char *stage) {
        if (!measure_memory)
            return;
        const ProcessMemory reading = processMemory();
        std::fprintf(stderr,
                     "aowis_db_memory stage=%s rss_kb=%lld peak_rss_kb=%lld\n",
                     stage, static_cast<long long>(reading.resident_kb),
                     static_cast<long long>(reading.high_water_kb));
        std::fflush(stderr);
    };
    if (measure_memory)
    {
        const ProcessMemory startup = processMemory();
        if (startup.resident_kb < 0 || startup.high_water_kb < 0)
        {
            errors << "Linux process memory counters are unavailable; cannot honor --memory.\n";
            return 1;
        }
    }
    checkpoint("startup");

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
    TemporaryPostgresSchema postgres_schema;
    aowis::db::Database database;
    if (postgres)
    {
        if (!postgres_schema.create(&error))
            return fail(QStringLiteral("Create isolated PostgreSQL schema"));
        if (!database.openPostgres(postgres_schema.databaseName(), postgres_schema.schemaName(), &error))
            return fail(QStringLiteral("Open PostgreSQL"));
    }
    else if (!database.openSqlite(db_path, &error))
        return fail(QStringLiteral("Open SQLite"));
    checkpoint("database_open");
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

    checkpoint("network_constructed");
    QElapsedTimer timer;
    timer.start();
    if (!database.saveHydraulicNetwork(actor, project->main_branch_uuid, 0, network, &error))
        return fail(QStringLiteral("Save network"));
    const qint64 initial_save_ms = timer.elapsed();
    checkpoint("initial_save");

    timer.restart();
    qint64 load_ms = -1;
    {
        const std::optional<NetworkHydraulic> loaded = database.loadHydraulicNetwork(
            actor, project->working_revision_uuid, network.uuid, &error);
        if (!loaded || loaded->links_pipes.size() != count ||
            loaded->nodes_junctions.size() != count)
            return fail(QStringLiteral("Load network"));
        load_ms = timer.elapsed();
        checkpoint("initial_load");
    }
    checkpoint("initial_load_released");

    const std::optional<aowis::db::RevisionInfo> before = database.revision(
        actor, project->working_revision_uuid, &error);
    if (!before)
        return fail(QStringLiteral("Read revision"));
    timer.restart();
    if (!database.saveHydraulicNetwork(actor, project->main_branch_uuid,
                                       before->generation, network, &error))
        return fail(QStringLiteral("Save unchanged network"));
    const qint64 unchanged_save_ms = timer.elapsed();
    checkpoint("unchanged_save");

    const double first_junction_initial_elevation = network.nodes_junctions.first().elevation_m;
    HydraulicNodeJunction updated_junction = network.nodes_junctions.first();
    updated_junction.elevation_m += 1.0;
    timer.restart();
    if (!aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid,
                                      updated_junction, 1, &error))
        return fail(QStringLiteral("Edit one junction"));
    const qint64 single_edit_ms = timer.elapsed();
    qint64 first_edit_total_ms = single_edit_ms;
    network.nodes_junctions[0].elevation_m = updated_junction.elevation_m;
    for (int index = 1; index < changes_per_revision; ++index)
    {
        HydraulicNodeJunction &junction = network.nodes_junctions[index];
        junction.elevation_m += 1.0;
        timer.restart();
        if (!aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid,
                                          junction, 1, &error))
            return fail(QStringLiteral("Edit junction in first batch"));
        first_edit_total_ms += timer.elapsed();
    }

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
    checkpoint("first_finalize");

    // Revision-growth workload: every successive working revision inherits all
    // existing objects. Change only the selected junctions before finalization so
    // the measurements expose the cost of copying largely unchanged models.
    // All data lives in a disposable database/schema; this is NOT a CTest.
    QUuid current_working_revision = finalization->new_working_revision_uuid;
    QUuid latest_finalized_revision = finalization->finalized_revision_uuid;
    qint64 total_repeated_edit_ms = 0;
    qint64 total_repeated_finalize_ms = 0;
    qint64 max_repeated_finalize_ms = 0;
    QStringList revision_samples;
    revision_samples.append(QStringLiteral("1:%1:%2").arg(first_edit_total_ms).arg(finalize_ms));
    for (int revision_number = 2; revision_number <= revision_count; ++revision_number)
    {
        qint64 edit_ms = 0;
        for (int index = 0; index < changes_per_revision; ++index)
        {
            HydraulicNodeJunction &junction = network.nodes_junctions[index];
            const std::optional<aowis::db::ModelObject> current_entity = database.object(
                actor, current_working_revision, junction.uuid, &error);
            if (!current_entity || current_entity->deleted)
                return fail(QStringLiteral("Find junction for repeated edit"));
            junction.elevation_m += 1.0;
            timer.restart();
            if (!aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid,
                                              junction, current_entity->version, &error))
                return fail(QStringLiteral("Edit junction in next revision"));
            edit_ms += timer.elapsed();
        }
        updated_junction = network.nodes_junctions.first();
        const std::optional<aowis::db::RevisionInfo> working = database.revision(
            actor, current_working_revision, &error);
        if (!working || working->finalized)
            return fail(QStringLiteral("Read next working revision"));
        timer.restart();
        const std::optional<aowis::db::Finalization> next = database.finalize(
            actor, project->main_branch_uuid, working->generation,
            QStringLiteral("Benchmark revision %1").arg(revision_number), {}, &error);
        if (!next)
            return fail(QStringLiteral("Finalize next revision"));
        const qint64 next_finalize_ms = timer.elapsed();
        current_working_revision = next->new_working_revision_uuid;
        latest_finalized_revision = next->finalized_revision_uuid;
        total_repeated_edit_ms += edit_ms;
        total_repeated_finalize_ms += next_finalize_ms;
        if (next_finalize_ms > max_repeated_finalize_ms)
            max_repeated_finalize_ms = next_finalize_ms;
        revision_samples.append(QStringLiteral("%1:%2:%3")
            .arg(revision_number).arg(edit_ms).arg(next_finalize_ms));
    }

    checkpoint("additional_revisions");

    // Confirm the newest working head actually inherited the last edit.
    // Release this complete network before branch creation; otherwise the
    // benchmark would artificially retain several full copies at once.
    {
        const std::optional<NetworkHydraulic> latest = database.loadHydraulicNetwork(
            actor, current_working_revision, network.uuid, &error);
        if (!latest)
            return fail(QStringLiteral("Reload newest working revision"));
        bool inherited = false;
        for (const HydraulicNodeJunction &junction : latest->nodes_junctions)
        {
            if (junction.uuid == updated_junction.uuid &&
                junction.elevation_m == updated_junction.elevation_m)
                inherited = true;
        }
        if (!inherited)
        {
            error = QStringLiteral("Latest working model did not inherit the final edit");
            return fail(QStringLiteral("Check revision inheritance"));
        }
        checkpoint("working_head_load");
    }
    checkpoint("working_head_load_released");

    timer.restart();
    const std::optional<aowis::db::BranchInfo> branch = database.createBranch(
        actor, project->project_uuid, QStringLiteral("alternative"),
        finalization->finalized_revision_uuid, &error);
    if (!branch)
        return fail(QStringLiteral("Create branch"));
    const qint64 branch_ms = timer.elapsed();
    checkpoint("branch_creation");

    // A branch from revision 1 must not accidentally inherit later edits
    // made to the main branch. Exercise a full load on the forked model.
    timer.restart();
    qint64 branch_load_ms = -1;
    {
        const std::optional<NetworkHydraulic> branch_model = database.loadHydraulicNetwork(
            actor, branch->head_revision_uuid, network.uuid, &error);
        if (!branch_model || branch_model->links_pipes.size() != count ||
            branch_model->nodes_junctions.size() != count)
            return fail(QStringLiteral("Verify forked network size"));
        bool preserved_baseline = false;
        for (const HydraulicNodeJunction &junction : branch_model->nodes_junctions)
        {
            if (junction.uuid == updated_junction.uuid &&
                junction.elevation_m == first_junction_initial_elevation + 1.0)
                preserved_baseline = true;
        }
        if (!preserved_baseline)
        {
            error = QStringLiteral("Forked revision did not preserve its finalized baseline");
            return fail(QStringLiteral("Verify branch independence"));
        }
        branch_load_ms = timer.elapsed();
        checkpoint("branch_load");
    }
    checkpoint("branch_load_released");

    qint64 ancestry_comparison_ms = -1;
    qint64 merge_preview_ms = -1;
    qint64 merge_preview_changes = -1;
    qint64 merge_preview_conflicts = -1;
    qint64 merge_preview_merkle_candidates = -1;
    qint64 merge_preview_merkle_nodes_loaded = -1;
    qint64 merge_preview_shared_subtrees_skipped = -1;
    bool merge_preview_merkle_used = false;
    qint64 divergent_branch_finalize_ms = -1;
    if (compare_branches)
    {
        // Edit a different junction on a branch forked from revision 1.
        // The main line has continued since revision 1; both tips have
        // the same finalized merge base and contain disjoint changes when
        // --edits is smaller than PIPE_COUNT.
        HydraulicNodeJunction branch_junction = network.nodes_junctions.at(count - 1);
        branch_junction.elevation_m =
            100.0 + static_cast<double>((count - 1) % 100) + 2.0;
        const std::optional<aowis::db::ModelObject> branch_entity = database.object(
            actor, branch->head_revision_uuid, branch_junction.uuid, &error);
        if (!branch_entity || branch_entity->deleted)
            return fail(QStringLiteral("Find divergent branch junction"));
        if (!aowis::db::putHydraulicEntity(database, actor, branch->uuid,
                                          branch_junction, branch_entity->version, &error))
            return fail(QStringLiteral("Edit divergent branch junction"));
        const std::optional<aowis::db::RevisionInfo> branch_working = database.revision(
            actor, branch->head_revision_uuid, &error);
        if (!branch_working)
            return fail(QStringLiteral("Read divergent branch working revision"));
        timer.restart();
        const std::optional<aowis::db::Finalization> branch_finalization = database.finalize(
            actor, branch->uuid, branch_working->generation,
            QStringLiteral("Benchmark divergent branch"), {}, &error);
        if (!branch_finalization)
            return fail(QStringLiteral("Finalize divergent branch"));
        divergent_branch_finalize_ms = timer.elapsed();
        checkpoint("divergent_branch_finalize");

        timer.restart();
        const std::optional<aowis::db::RevisionComparison> comparison = database.compareRevisions(
            actor, latest_finalized_revision, branch_finalization->finalized_revision_uuid, &error);
        if (!comparison || comparison->left_is_ancestor_of_right ||
            comparison->right_is_ancestor_of_left ||
            comparison->merge_base_revision_uuids.size() != 1 ||
            comparison->merge_base_revision_uuids.first() != finalization->finalized_revision_uuid)
        {
            if (error.isEmpty())
                error = QStringLiteral("Divergent branches do not have the expected common ancestor");
            return fail(QStringLiteral("Compare divergent revision ancestry"));
        }
        ancestry_comparison_ms = timer.elapsed();
        timer.restart();
        const std::optional<aowis::db::ObjectMergePreview> preview = database.previewObjectMerge(
            actor, latest_finalized_revision, branch_finalization->finalized_revision_uuid, &error);
        if (!preview || preview->changes.isEmpty())
        {
            if (error.isEmpty())
                error = QStringLiteral("Three-way preview produced no changes");
            return fail(QStringLiteral("Preview divergent branches"));
        }
        merge_preview_ms = timer.elapsed();
        merge_preview_changes = preview->changes.size();
        merge_preview_conflicts = preview->conflict_count;
        merge_preview_merkle_used = preview->merkle_candidate_scan_used;
        merge_preview_merkle_candidates = static_cast<qint64>(preview->merkle_candidate_keys);
        merge_preview_merkle_nodes_loaded = static_cast<qint64>(preview->merkle_tree_nodes_loaded);
        merge_preview_shared_subtrees_skipped =
            static_cast<qint64>(preview->merkle_shared_subtrees_skipped);
        checkpoint("merge_preview");
    }

    timer.restart();
    const aowis::db::IntegrityReport report = database.verifyProject(
        actor, project->project_uuid);
    if (!report.valid || report.checked_revisions != revision_count + (compare_branches ? 1 : 0))
    {
        error = report.error;
        return fail(QStringLiteral("Verify finalized history"));
    }
    const qint64 verify_ms = timer.elapsed();
    checkpoint("verify_history");

    qint64 deep_verify_ms = -1;
    if (deep_verify)
    {
        timer.restart();
        const aowis::db::IntegrityReport deep_report = database.verifyProjectJournal(
            actor, project->project_uuid);
        if (!deep_report.valid || deep_report.checked_revisions != revision_count + (compare_branches ? 1 : 0))
        {
            error = deep_report.error;
            return fail(QStringLiteral("Verify journal/Merkle consistency"));
        }
        deep_verify_ms = timer.elapsed();
        checkpoint("verify_journal");
    }

    database.close();
    checkpoint("database_closed");
    const std::optional<StorageCounts> counts = postgres
        ? postgres_schema.storageCounts(&error) : sqliteStorageCounts(db_path, &error);
    if (!counts)
        return fail(QStringLiteral("Measure SQL row and content storage"));
    std::optional<qint64> postgres_bytes;
    if (postgres)
    {
        postgres_bytes = postgres_schema.storageBytes(&error);
        if (!postgres_bytes)
            return fail(QStringLiteral("Measure PostgreSQL schema storage"));
    }
    output << (postgres ? "backend=postgres\n" : "backend=sqlite\n")
           << "pipes=" << count << "\n"
           << "junctions=" << count << "\n"
           << "finalized_revisions=" << revision_count << "\n"
           << "revision_object_rows=" << counts->working_objects << "\n"
           << "hydraulic_network_member_rows=" << counts->network_members << "\n"
           << "hydraulic_network_reference_rows=" << counts->network_references << "\n"
           << "object_event_rows=" << counts->object_events << "\n"
           << "revision_rows=" << counts->revision_rows << "\n"
           << "content_blob_rows=" << counts->content_blobs << "\n"
           << "content_tree_node_rows=" << counts->content_tree_nodes << "\n"
           << "content_payload_bytes=" << counts->content_payload_bytes << "\n"
           << "changes_per_revision=" << changes_per_revision << "\n"
           << "comparison_enabled=" << (compare_branches ? 1 : 0) << "\n"
           << "first_revision_edit_total_ms=" << first_edit_total_ms << "\n"
           << "initial_save_ms=" << initial_save_ms << "\n"
           << "load_ms=" << load_ms << "\n"
           << "unchanged_save_ms=" << unchanged_save_ms << "\n"
           << "single_entity_edit_ms=" << single_edit_ms << "\n"
           << "finalize_ms=" << finalize_ms << "\n"
           << "create_branch_ms=" << branch_ms << "\n"
           << "branch_load_ms=" << branch_load_ms << "\n"
           << "verify_history_ms=" << verify_ms << "\n"
           << "repeat_finalize_total_ms=" << total_repeated_finalize_ms << "\n"
           << "repeat_finalize_max_ms=" << max_repeated_finalize_ms << "\n"
           << "repeat_edit_total_ms=" << total_repeated_edit_ms << "\n"
           << "revision_samples_number_edit_ms_finalize_ms=" << revision_samples.join(QLatin1Char(';')) << "\n";
    if (compare_branches)
        output << "divergent_branch_finalize_ms=" << divergent_branch_finalize_ms << "\n"
               << "ancestry_comparison_ms=" << ancestry_comparison_ms << "\n"
               << "merge_preview_ms=" << merge_preview_ms << "\n"
               << "merge_preview_changes=" << merge_preview_changes << "\n"
               << "merge_preview_conflicts=" << merge_preview_conflicts << "\n"
               << "merge_preview_merkle_used=" << (merge_preview_merkle_used ? 1 : 0) << "\n"
               << "merge_preview_merkle_candidates=" << merge_preview_merkle_candidates << "\n"
               << "merge_preview_merkle_nodes_loaded=" << merge_preview_merkle_nodes_loaded << "\n"
               << "merge_preview_shared_subtrees_skipped=" << merge_preview_shared_subtrees_skipped << "\n";
    if (deep_verify)
        output << "verify_journal_ms=" << deep_verify_ms << "\n";
    if (measure_memory)
    {
        const ProcessMemory final_memory = processMemory();
        output << "process_rss_end_kb=" << final_memory.resident_kb << "\n"
               << "process_peak_rss_kb=" << final_memory.high_water_kb << "\n";
    }
    if (postgres)
        output << "postgres_schema_bytes=" << *postgres_bytes << "\n";
    else
        output << "sqlite_file_bytes=" << QFileInfo(db_path).size() << "\n";
    output.flush();
    return 0;
}
