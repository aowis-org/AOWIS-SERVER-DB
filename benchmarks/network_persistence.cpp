#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#include <optional>

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
    bool has_count = false;
    int count = 1000;
    for (int index = 1; index < arguments.size(); ++index)
    {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--help"))
        {
            output << "Usage: aowis-db-network-benchmark [PIPE_COUNT] [--postgres]\n"
                      "Default PIPE_COUNT: 1000; allowed range: 1..100000.\n"
                      "SQLite uses a temporary file. PostgreSQL requires a dedicated\n"
                      "AOWIS_DB_TEST_PG_DB and libpq PG* connection settings;\n"
                      "a random temporary schema is created and deleted.\n";
            return 0;
        }
        if (argument == QStringLiteral("--postgres") && !postgres)
        {
            postgres = true;
            continue;
        }
        bool number_ok = false;
        const int parsed_count = argument.toInt(&number_ok);
        if (has_count || !number_ok || parsed_count < 1 || parsed_count > 100000)
        {
            errors << "Usage: aowis-db-network-benchmark [PIPE_COUNT] [--postgres]\n"
                      "PIPE_COUNT must be an integer between 1 and 100000.\n";
            return 2;
        }
        has_count = true;
        count = parsed_count;
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
           << "initial_save_ms=" << initial_save_ms << "\n"
           << "load_ms=" << load_ms << "\n"
           << "unchanged_save_ms=" << unchanged_save_ms << "\n"
           << "single_entity_edit_ms=" << single_edit_ms << "\n"
           << "finalize_ms=" << finalize_ms << "\n"
           << "create_branch_ms=" << branch_ms << "\n"
           << "verify_history_ms=" << verify_ms << "\n";
    if (postgres)
        output << "postgres_schema_bytes=" << *postgres_bytes << "\n";
    else
        output << "sqlite_file_bytes=" << QFileInfo(db_path).size() << "\n";
    output.flush();
    return 0;
}
