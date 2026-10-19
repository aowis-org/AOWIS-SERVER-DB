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

    std::optional<qint64> objectRows(QString *error) const
    {
        QSqlQuery rows(this->connection_);
        if (!rows.exec(QStringLiteral("SELECT COUNT(*) FROM %1.revision_objects").arg(this->schema_name_)) ||
            !rows.next())
        {
            *error = rows.lastError().text();
            return std::nullopt;
        }
        return rows.value(0).toLongLong();
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

std::optional<qint64> sqliteObjectRows(const QString &path, QString *error)
{
    const QString connection_name = QStringLiteral("aowis-benchmark-count-") +
        createUuidV7().toString(QUuid::Id128);
    qint64 result = -1;
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        connection.setDatabaseName(path);
        if (!connection.open())
            *error = connection.lastError().text();
        else
        {
            QSqlQuery rows(connection);
            if (rows.exec(QStringLiteral("SELECT COUNT(*) FROM revision_objects")) && rows.next())
                result = rows.value(0).toLongLong();
            else
                *error = rows.lastError().text();
        }
        connection.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    if (result < 0)
        return std::nullopt;
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
    int count = 1000;
    int revision_count = 1;
    bool revisions_option_seen = false;
    for (int index = 1; index < arguments.size(); ++index)
    {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--help"))
        {
            output << "Usage: aowis-db-network-benchmark [PIPE_COUNT] [--revisions N] [--postgres] [--profile] [--memory] [--deep-verify]\n"
                      "Default PIPE_COUNT: 1000; allowed range: 1..100000.\n"
                      "Default revisions: 1; allowed range: 1..100 finalized revisions.\n"
                      "Each additional revision changes one junction and finalizes.\n"
                      "SQLite uses a temporary file. PostgreSQL requires a dedicated\n"
                      "AOWIS_DB_TEST_PG_DB and libpq PG* connection settings;\n"
                      "a random temporary schema is created and deleted.\n"
                      "--profile prints Merkle, network save and load timings to stderr.\n"
                      "--memory prints current and peak process RSS at workload checkpoints.\n"
                      "--deep-verify also replays object journals against Merkle revisions.\n";
            return 0;
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
            errors << "Usage: aowis-db-network-benchmark [PIPE_COUNT] [--revisions N] [--postgres] [--profile] [--memory] [--deep-verify]\n"
                      "PIPE_COUNT must be an integer between 1 and 100000.\n";
            return 2;
        }
        has_count = true;
        count = parsed_count;
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
    checkpoint("first_finalize");

    // Revision-growth workload: every successive working revision inherits all
    // existing objects. Change only one junction before each finalization so
    // the measurements expose the cost of copying largely unchanged models.
    // All data lives in a disposable database/schema; this is NOT a CTest.
    QUuid current_working_revision = finalization->new_working_revision_uuid;
    qint64 total_repeated_edit_ms = 0;
    qint64 total_repeated_finalize_ms = 0;
    qint64 max_repeated_finalize_ms = 0;
    QStringList revision_samples;
    revision_samples.append(QStringLiteral("1:%1:%2").arg(single_edit_ms).arg(finalize_ms));
    for (int revision_number = 2; revision_number <= revision_count; ++revision_number)
    {
        const std::optional<aowis::db::ModelObject> current_entity = database.object(
            actor, current_working_revision, updated_junction.uuid, &error);
        if (!current_entity || current_entity->deleted)
            return fail(QStringLiteral("Find junction for repeated edit"));
        updated_junction.elevation_m += 1.0;
        timer.restart();
        if (!aowis::db::putHydraulicEntity(database, actor, project->main_branch_uuid,
                                          updated_junction, current_entity->version, &error))
            return fail(QStringLiteral("Edit junction in next revision"));
        const qint64 edit_ms = timer.elapsed();
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
                junction.elevation_m == network.nodes_junctions.first().elevation_m + 1.0)
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

    timer.restart();
    const aowis::db::IntegrityReport report = database.verifyProject(
        actor, project->project_uuid);
    if (!report.valid || report.checked_revisions != revision_count)
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
        if (!deep_report.valid || deep_report.checked_revisions != revision_count)
        {
            error = deep_report.error;
            return fail(QStringLiteral("Verify journal/Merkle consistency"));
        }
        deep_verify_ms = timer.elapsed();
        checkpoint("verify_journal");
    }

    database.close();
    checkpoint("database_closed");
    std::optional<qint64> object_rows;
    if (postgres)
        object_rows = postgres_schema.objectRows(&error);
    else
        object_rows = sqliteObjectRows(db_path, &error);
    if (!object_rows)
        return fail(QStringLiteral("Count stored revision objects"));
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
           << "revision_object_rows=" << *object_rows << "\n"
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
