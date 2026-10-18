#include <aowis/db/model_persistence.h>
#include <aowis/model/uuid.h>

#include "../src/sqlite_backend.h"
#include "../src/postgres_backend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QMetaType>
#include <QSqlError>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::Database;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

// Report the SQL driver's actual error, not only the backend's error string.
#define REQUIRE_SQL(query, condition) do { \
    if (!(condition)) { \
        qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition \
                              << "SQL:" << (query).lastQuery() \
                              << "SQL error:" << (query).lastError().text(); \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString file = temp.filePath(QStringLiteral("development.sqlite"));
    const QString name = QStringLiteral("backend-contract");
    aowis::db::SqliteBackend adapter;
    aowis::db::StorageBackend &contract = adapter;
    QSqlDatabase connection;
    REQUIRE(contract.open(connection, name, file, &error));
    REQUIRE(contract.configure(connection, &error));
    REQUIRE(contract.hasSchema(connection, &error) == false);
    REQUIRE(!contract.hasColumn(connection, QStringLiteral("uncreated"),
                                QStringLiteral("uuid"), &error).value());
    REQUIRE(!contract.hasColumn(connection, QStringLiteral("bad); DROP TABLE users;--"),
                                QStringLiteral("uuid"), &error).has_value());
    error.clear();

    // A failed creation does not leave behind half a database.
    REQUIRE(contract.beginWrite(connection, &error));
    REQUIRE(contract.createSchema(connection, &error));
    REQUIRE(contract.hasSchema(connection, &error) == true);
    contract.rollback(connection);
    REQUIRE(contract.hasSchema(connection, &error) == false);

    REQUIRE(contract.beginWrite(connection, &error));
    REQUIRE(contract.createSchema(connection, &error));
    REQUIRE(contract.commit(connection, &error));
    REQUIRE(contract.hasSchema(connection, &error) == true);
    REQUIRE(contract.verifySchema(connection, &error));
    REQUIRE(contract.hasColumn(connection, QStringLiteral("revisions"),
                               QStringLiteral("hash_format_version"), &error) == true);
    REQUIRE(contract.hasColumn(connection, QStringLiteral("role_assignments"),
                               QStringLiteral("revoked_at_ms"), &error) == true);
    REQUIRE(contract.hasColumn(connection, QStringLiteral("password_recovery_tokens"),
                               QStringLiteral("token_sha256"), &error) == true);

    // Still verify UUID value representation independently of schema DDL.
    const QUuid id(QStringLiteral("{018e7df0-6f80-7c1e-bca5-80e2f23b0622}"));
    REQUIRE(contract.bindUuid(id).toByteArray() == id.toRfc4122());
    REQUIRE(contract.readUuid(contract.bindUuid(id)) == id);
    REQUIRE(contract.readUuid(QVariant()).isNull());
    REQUIRE(contract.readUuid(QVariant(QByteArray(15, '\0'))).isNull());
    const QVariant sql_null(QMetaType::fromType<QByteArray>());
    QSqlQuery query(connection);
    REQUIRE_SQL(query, query.prepare(QStringLiteral(
        "INSERT INTO users(uuid,username,created_at_ms) VALUES (?,?,?)")));
    query.addBindValue(contract.bindUuid(id));
    query.addBindValue(QStringLiteral("first"));
    query.addBindValue(1);
    REQUIRE_SQL(query, query.exec());
    query.finish();
    REQUIRE_SQL(query, query.exec(QStringLiteral("SELECT uuid FROM users")));
    REQUIRE_SQL(query, query.next());
    REQUIRE(contract.readUuid(query.value(0)) == id);
    query.finish();
    REQUIRE(sql_null.isNull());
    // All current tables exist from the first creation, with no upgrades.
    query = QSqlQuery();
    connection.close();
    connection = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);

    Database db;
    REQUIRE(db.openSqlite(file, &error));
    REQUIRE(db.isOpen());
    REQUIRE(db.bootstrapAdministrator(QStringLiteral("second"), &error).has_value() == false);
    db.close();

    // Existing but incomplete databases fail rather than being silently reset.
    {
        QSqlDatabase damaged = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("damaged"));
        damaged.setDatabaseName(file);
        REQUIRE(damaged.open());
        QSqlQuery sql(damaged);
        REQUIRE_SQL(sql, sql.exec(QStringLiteral("DROP TRIGGER recovery_tokens_no_delete")));
        sql.finish();
        damaged.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("damaged"));
    error.clear();
    REQUIRE(!db.openSqlite(file, &error));
    REQUIRE(error.contains(QStringLiteral("recovery token immutability")));

    // Fresh empty-file creation is independent of previously damaged files.
    const QString fresh_file = temp.filePath(QStringLiteral("new.sqlite"));
    REQUIRE(db.openSqlite(fresh_file, &error));
    const std::optional<aowis::db::UserInfo> administrator =
        db.bootstrapAdministrator(QStringLiteral("operator"), &error);
    REQUIRE(administrator.has_value());
    REQUIRE(db.projects({administrator->uuid}, &error).isEmpty());
    db.close();

    // The PostgreSQL adapter must use native UUID values, never SQLite BLOBs.
    // These representation checks need no QPSQL driver or PostgreSQL server.
    aowis::db::PostgresBackend postgres_adapter;
    aowis::db::StorageBackend &postgres_contract = postgres_adapter;
    REQUIRE(postgres_contract.bindUuid(id).metaType().id() == QMetaType::QUuid);
    // Qt's SQL result layer treats a null QUuid as SQL NULL. The all-zero
    // UUID must instead bind as its non-NULL text representation.
    REQUIRE(postgres_contract.bindUuid(QUuid()).metaType().id() == QMetaType::QString);
    REQUIRE(!postgres_contract.bindUuid(QUuid()).isNull());
    REQUIRE(postgres_contract.bindUuid(QUuid()).toString() ==
            QStringLiteral("00000000-0000-0000-0000-000000000000"));
    REQUIRE(postgres_contract.readUuid(postgres_contract.bindUuid(id)) == id);
    REQUIRE(postgres_contract.readUuid(postgres_contract.bindUuid(QUuid())).isNull());
    REQUIRE(postgres_contract.readUuid(QVariant()).isNull());
    REQUIRE(postgres_contract.readUuid(QVariant(QStringLiteral("not-a-uuid"))).isNull());
    REQUIRE(postgres_contract.readUuid(QVariant(id.toRfc4122())).isNull());
    REQUIRE(postgres_contract.readUuid(QVariant(id.toString(QUuid::WithoutBraces))) == id);
    REQUIRE(!postgres_contract.hasColumn(QSqlDatabase(), QStringLiteral("bad); DROP TABLE users;--"),
                                         QStringLiteral("uuid"), &error).has_value());
    error.clear();
    // PostgreSQL schema creation and verification are exercised on an
    // isolated transaction if a live test database is provided below.

    // Live integration: compile_linux_tests.sh supplies a disposable Docker
    // PostgreSQL database automatically, or uses a dedicated external test
    // database. If AOWIS_DB_TEST_PG_REQUIRED=1, a missing database is a test
    // failure rather than an accidental skip. PostgreSQL schemas are isolated.
    const QByteArray pg_database_name = qgetenv("AOWIS_DB_TEST_PG_DB");
    // Test-runner/CI guarantee: never report green if live PostgreSQL was
    // required but its connection settings were accidentally not passed on.
    if (qgetenv("AOWIS_DB_TEST_PG_REQUIRED") == QByteArray("1"))
        REQUIRE(!pg_database_name.isEmpty());
    if (!pg_database_name.isEmpty())
    {
        const QString pg_connection_name = QStringLiteral("backend-contract-postgres");
        {
            QSqlDatabase pg_connection;
            REQUIRE(postgres_contract.open(pg_connection, pg_connection_name,
                                           QString::fromUtf8(pg_database_name), &error));
            REQUIRE(postgres_contract.configure(pg_connection, &error));
            REQUIRE(postgres_contract.hasSchema(pg_connection, &error).has_value());
            REQUIRE(postgres_contract.hasColumn(pg_connection, QStringLiteral("nonexistent_aowis_table"),
                                                QStringLiteral("uuid"), &error) == false);
            REQUIRE(postgres_contract.beginWrite(pg_connection, &error));
            {
                QSqlQuery pg_query(pg_connection);
                REQUIRE_SQL(pg_query, pg_query.exec(QStringLiteral(
                    "CREATE TEMP TABLE aowis_uuid_probe "
                    "(ordinal INTEGER PRIMARY KEY, object_uuid UUID)")));
                REQUIRE_SQL(pg_query, pg_query.prepare(QStringLiteral(
                    "INSERT INTO aowis_uuid_probe (ordinal, object_uuid) VALUES (?, ?)")));
                pg_query.addBindValue(1);
                pg_query.addBindValue(postgres_contract.bindUuid(id));
                REQUIRE_SQL(pg_query, pg_query.exec());
                pg_query.finish();
                REQUIRE_SQL(pg_query, pg_query.prepare(QStringLiteral(
                    "INSERT INTO aowis_uuid_probe (ordinal, object_uuid) VALUES (?, ?)")));
                pg_query.addBindValue(2);
                pg_query.addBindValue(postgres_contract.bindUuid(QUuid()));
                REQUIRE_SQL(pg_query, pg_query.exec());
                pg_query.finish();
                // SQL NULL must remain distinct from the all-zero UUID.
                REQUIRE_SQL(pg_query, pg_query.exec(QStringLiteral(
                    "INSERT INTO aowis_uuid_probe (ordinal, object_uuid) VALUES (3, NULL)")));
                pg_query.finish();
                REQUIRE_SQL(pg_query, pg_query.exec(QStringLiteral(
                    "SELECT object_uuid::text, object_uuid IS NULL, "
                    "pg_typeof(object_uuid)::text "
                    "FROM aowis_uuid_probe ORDER BY ordinal")));
                REQUIRE_SQL(pg_query, pg_query.next());
                REQUIRE(pg_query.value(0).toString() == id.toString(QUuid::WithoutBraces));
                REQUIRE(!pg_query.value(1).toBool());
                REQUIRE(pg_query.value(2).toString() == QStringLiteral("uuid"));
                REQUIRE_SQL(pg_query, pg_query.next());
                REQUIRE(pg_query.value(0).toString() ==
                        QStringLiteral("00000000-0000-0000-0000-000000000000"));
                REQUIRE(!pg_query.value(1).toBool());
                REQUIRE(pg_query.value(2).toString() == QStringLiteral("uuid"));
                REQUIRE_SQL(pg_query, pg_query.next());
                REQUIRE(pg_query.value(0).isNull());
                REQUIRE(pg_query.value(1).toBool());
                REQUIRE_SQL(pg_query, !pg_query.next());
                pg_query.finish();
                // The sentinel must also work as a bound UUID predicate.
                REQUIRE_SQL(pg_query, pg_query.prepare(QStringLiteral(
                    "SELECT COUNT(*) FROM aowis_uuid_probe WHERE object_uuid = ?")));
                pg_query.addBindValue(postgres_contract.bindUuid(QUuid()));
                REQUIRE_SQL(pg_query, pg_query.exec());
                REQUIRE_SQL(pg_query, pg_query.next());
                REQUIRE(pg_query.value(0).toInt() == 1);
                pg_query.finish();
            }
            postgres_contract.rollback(pg_connection);
            {
                QSqlQuery check(pg_connection);
                REQUIRE_SQL(check, check.exec(QStringLiteral(
                    "SELECT to_regclass('pg_temp.aowis_uuid_probe') IS NULL")));
                REQUIRE_SQL(check, check.next());
                REQUIRE(check.value(0).toBool());
                check.finish();
            }
            // Build the entire current schema inside a newly-created
            // transaction-local schema, then roll it back. This never
            // modifies the caller's existing tables or schema.
            REQUIRE(postgres_contract.beginWrite(pg_connection, &error));
            const QString probe_schema = QStringLiteral("aowis_schema_probe_") +
                QUuid::createUuid().toString(QUuid::Id128);
            QSqlQuery ddl(pg_connection);
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("CREATE SCHEMA %1").arg(probe_schema)));
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("SET LOCAL search_path TO %1").arg(probe_schema)));
            REQUIRE(postgres_contract.hasSchema(pg_connection, &error) == false);
            REQUIRE(postgres_contract.createSchema(pg_connection, &error));
            REQUIRE(postgres_contract.hasSchema(pg_connection, &error) == true);
            REQUIRE(postgres_contract.verifySchema(pg_connection, &error));
            REQUIRE(postgres_contract.hasColumn(pg_connection, QStringLiteral("revisions"),
                                                QStringLiteral("hash_format_version"), &error) == true);
            REQUIRE(postgres_contract.hasColumn(pg_connection, QStringLiteral("users"),
                                                QStringLiteral("uuid"), &error) == true);
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral(
                "SELECT data_type FROM information_schema.columns "
                "WHERE table_schema=current_schema() AND table_name='users' AND column_name='uuid'")));
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toString() == QStringLiteral("uuid"));
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral(
                "SELECT data_type FROM information_schema.columns "
                "WHERE table_schema=current_schema() AND table_name='users' AND column_name='created_at_ms'")));
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toString() == QStringLiteral("bigint"));
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral(
                "SELECT data_type FROM information_schema.columns "
                "WHERE table_schema=current_schema() AND table_name='user_password_credentials' "
                "AND column_name='verifier'")));
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toString() == QStringLiteral("bytea"));
            ddl.finish();

            // The creator guard must reject unattributed project creation.
            // Savepoints are necessary: PostgreSQL marks a transaction failed
            // after a constraint error until it is rolled back to a savepoint.
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("SAVEPOINT schema_guard_check")));
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO projects(uuid,name,created_at_ms) VALUES (?,?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(QUuid::createUuid()));
            ddl.addBindValue(QStringLiteral("forbidden-project"));
            ddl.addBindValue(QDateTime::currentMSecsSinceEpoch());
            REQUIRE_SQL(ddl, !ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("ROLLBACK TO SAVEPOINT schema_guard_check")));
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("RELEASE SAVEPOINT schema_guard_check")));
            ddl.finish();

            // Exercise a real project/branch/revision lifecycle with the
            // PostgreSQL schema, native UUID foreign keys and immutable history.
            // All rows live in this transaction-local schema and are rolled
            // back below. No application schema upgrades or fixture cleanup.
            const QUuid actor_uuid = QUuid::createUuid();
            const QUuid project_uuid = QUuid::createUuid();
            const QUuid branch_uuid = QUuid::createUuid();
            const QUuid revision_uuid = QUuid::createUuid();
            const QUuid successor_uuid = QUuid::createUuid();
            const QUuid role_uuid = QUuid::createUuid();
            const QUuid system_grant_uuid = QUuid::createUuid();
            const QUuid project_grant_uuid = QUuid::createUuid();
            const QByteArray hash(32, '\x5a');
            const QByteArray snapshot_bytes("postgres-schema-smoke-snapshot");
            const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO users(uuid,username,created_at_ms) VALUES (?,?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            ddl.addBindValue(QStringLiteral("schema-test-user"));
            ddl.addBindValue(now_ms);
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();

            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO projects(uuid,name,created_at_ms,created_by_uuid) VALUES (?,?,?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(project_uuid));
            ddl.addBindValue(QStringLiteral("Schema constraint project"));
            ddl.addBindValue(now_ms);
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();

            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO branches(uuid,project_uuid,name,created_by_uuid) VALUES (?,?,?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(branch_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(project_uuid));
            ddl.addBindValue(QStringLiteral("main"));
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();

            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,"
                "hash_format_version) VALUES (?,?,?,1,'working',?,?,2)")));
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(project_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(branch_uuid));
            ddl.addBindValue(now_ms);
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();

            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "UPDATE branches SET head_revision_uuid=? WHERE uuid=?")));
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(branch_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO snapshots(revision_uuid,content) VALUES (?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            ddl.addBindValue(snapshot_bytes);
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();

            // Test native UUID scope values and the system-scope SQL NULL.
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO roles(uuid,key,builtin) VALUES (?,'schema_test.reader',0)")));
            ddl.addBindValue(postgres_contract.bindUuid(role_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,"
                "scope_kind,scope_uuid,created_by_uuid,created_at_ms) "
                "VALUES (?,'user',?,?,'system',NULL,?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(system_grant_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(role_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            ddl.addBindValue(now_ms);
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO role_assignments(uuid,principal_kind,principal_uuid,role_uuid,"
                "scope_kind,scope_uuid,created_by_uuid,created_at_ms) "
                "VALUES (?,'user',?,?,'project',?,?,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(project_grant_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(role_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(project_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            ddl.addBindValue(now_ms);
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral(
                "SELECT scope_kind,scope_uuid FROM role_assignments "
                "ORDER BY scope_kind DESC")));
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toString() == QStringLiteral("system"));
            REQUIRE(ddl.value(1).isNull());
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toString() == QStringLiteral("project"));
            REQUIRE(postgres_contract.readUuid(ddl.value(1)) == project_uuid);
            REQUIRE_SQL(ddl, !ddl.next());
            ddl.finish();

            // Finalized revisions require actor attribution and a history seal.
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("SAVEPOINT missing_finalize_actor")));
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "UPDATE revisions SET state='finalized',finalized_at_ms=? WHERE uuid=?")));
            ddl.addBindValue(now_ms);
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            REQUIRE_SQL(ddl, !ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("ROLLBACK TO SAVEPOINT missing_finalize_actor")));
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("RELEASE SAVEPOINT missing_finalize_actor")));
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "UPDATE revisions SET state='finalized',finalized_at_ms=?,finalized_by_uuid=?,"
                "operation_history_sha256=?,snapshot_sha256=?,chain_sha256=? WHERE uuid=?")));
            ddl.addBindValue(now_ms);
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            ddl.addBindValue(hash);
            ddl.addBindValue(hash);
            ddl.addBindValue(hash);
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            REQUIRE(ddl.numRowsAffected() == 1);
            ddl.finish();

            // The finalized snapshot is protected at the SQL layer.
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("SAVEPOINT immutable_finalized_snapshot")));
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "UPDATE snapshots SET content=? WHERE revision_uuid=?")));
            ddl.addBindValue(QByteArray("tampering"));
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            REQUIRE_SQL(ddl, !ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("ROLLBACK TO SAVEPOINT immutable_finalized_snapshot")));
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("RELEASE SAVEPOINT immutable_finalized_snapshot")));

            // Advance the branch atomically and retain the finalized parent.
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,"
                "hash_format_version) VALUES (?,?,?,2,'working',?,?,2)")));
            ddl.addBindValue(postgres_contract.bindUuid(successor_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(project_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(branch_uuid));
            ddl.addBindValue(now_ms);
            ddl.addBindValue(postgres_contract.bindUuid(actor_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) "
                "VALUES (?,0,?)")));
            ddl.addBindValue(postgres_contract.bindUuid(successor_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(revision_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "UPDATE branches SET head_revision_uuid=? WHERE uuid=?")));
            ddl.addBindValue(postgres_contract.bindUuid(successor_uuid));
            ddl.addBindValue(postgres_contract.bindUuid(branch_uuid));
            REQUIRE_SQL(ddl, ddl.exec());
            ddl.finish();
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral(
                "SELECT count(*) FROM revisions WHERE branch_uuid=(SELECT uuid FROM branches WHERE name='main')")));
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toInt() == 2);
            ddl.finish();

            postgres_contract.rollback(pg_connection);
            REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                "SELECT to_regnamespace(?) IS NULL")));
            ddl.addBindValue(probe_schema);
            REQUIRE_SQL(ddl, ddl.exec());
            REQUIRE_SQL(ddl, ddl.next());
            REQUIRE(ddl.value(0).toBool());
            ddl.finish();
            // Exercise the actual Database facade, not only QPSQL DDL.
            // Give it its own disposable schema in the same dedicated test
            // database. Do not touch an existing application schema.
            const QString core_schema = QStringLiteral("aowis_core_api_") +
                QUuid::createUuid().toString(QUuid::Id128);
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("CREATE SCHEMA %1").arg(core_schema)));
            QUuid saved_admin_uuid;
            QUuid saved_project_uuid;
            {
                Database postgres_database;
                REQUIRE(postgres_database.openPostgres(
                    QString::fromUtf8(pg_database_name), core_schema, &error));
                REQUIRE(postgres_database.isOpen());
                // Opening twice is forbidden and must not discard the first connection.
                REQUIRE(!postgres_database.openPostgres(
                    QString::fromUtf8(pg_database_name), core_schema, &error));
                REQUIRE(postgres_database.isOpen());
                error.clear();

                const std::optional<aowis::db::UserInfo> pg_admin =
                    postgres_database.bootstrapAdministrator(QStringLiteral("postgres-operator"), &error);
                REQUIRE(pg_admin.has_value());
                saved_admin_uuid = pg_admin->uuid;
                const aowis::db::OperationContext actor{pg_admin->uuid};
                REQUIRE(postgres_database.hasPermission(actor, QStringLiteral("project.create"),
                    aowis::db::AccessScope::system(), &error));
                const std::optional<aowis::db::ProjectCreation> pg_project =
                    postgres_database.createProject(actor, QStringLiteral("PostgreSQL core integration"), &error);
                REQUIRE(pg_project.has_value());
                saved_project_uuid = pg_project->project_uuid;
                REQUIRE(postgres_database.projects(actor, &error).size() == 1);
                REQUIRE(postgres_database.branches(actor, pg_project->project_uuid, &error).size() == 1);
                const std::optional<aowis::db::RevisionInfo> pg_first =
                    postgres_database.revision(actor, pg_project->working_revision_uuid, &error);
                REQUIRE(pg_first.has_value() && !pg_first->finalized && pg_first->number == 1);
                const QByteArray pg_snapshot("postgresql-core-snapshot");
                REQUIRE(postgres_database.replaceWorkingSnapshot(
                    actor, pg_project->main_branch_uuid, 0, pg_snapshot, &error));
                REQUIRE(postgres_database.snapshot(actor, pg_project->working_revision_uuid, &error) == pg_snapshot);
                REQUIRE(!postgres_database.replaceWorkingSnapshot(
                    actor, pg_project->main_branch_uuid, 0, QByteArray("stale"), &error));
                const std::optional<aowis::db::Finalization> pg_finalized =
                    postgres_database.finalize(actor, pg_project->main_branch_uuid, 1,
                                               QStringLiteral("baseline"), QString(), &error);
                REQUIRE(pg_finalized.has_value());
                REQUIRE(postgres_database.revision(actor, pg_project->working_revision_uuid, &error)->finalized);
                REQUIRE(postgres_database.revision(actor, pg_finalized->new_working_revision_uuid, &error)->number == 2);
                REQUIRE(postgres_database.verifyProject(actor, pg_project->project_uuid).valid);
                REQUIRE(postgres_database.verifySecurityAudit(actor).valid);
                postgres_database.close();
            }
            // Reopening reads the very same persisted PostgreSQL project.
            {
                Database reopened;
                REQUIRE(reopened.openPostgres(QString::fromUtf8(pg_database_name), core_schema, &error));
                REQUIRE(reopened.isOpen());
                const aowis::db::OperationContext actor{saved_admin_uuid};
                const std::optional<aowis::db::ProjectInfo> persisted =
                    reopened.project(actor, saved_project_uuid, &error);
                REQUIRE(persisted.has_value());
                REQUIRE(persisted->created_by_uuid == saved_admin_uuid);
                REQUIRE(reopened.verifyProject(actor, saved_project_uuid).valid);

                // Exercise the granular-object and grouped-history
                // paths on PostgreSQL, using a separate object-mode project.
                // This must not share the legacy snapshot-mode revision above.
                const std::optional<aowis::db::ProjectCreation> object_project =
                    reopened.createProject(actor, QStringLiteral("PostgreSQL object history"), &error);
                REQUIRE(object_project.has_value());
                const QUuid working = object_project->working_revision_uuid;
                const QUuid branch = object_project->main_branch_uuid;
                const QUuid pipe = QUuid::createUuid();
                const QUuid junction = QUuid::createUuid();
                const QString pipe_kind = QStringLiteral("hydraulic.pipe");
                const QString junction_kind = QStringLiteral("hydraulic.junction");
                const QByteArray original_pipe("\0\x01\xff", 3);
                const QByteArray changed_pipe("PG edited pipe");

                const std::optional<QUuid> first_group = reopened.commitObjectGroup(actor, branch, 0, {
                    {pipe, pipe_kind, original_pipe, -1},
                    {junction, junction_kind, QByteArray("PG junction"), -1}
                }, &error);
                REQUIRE(first_group.has_value());
                REQUIRE(reopened.revision(actor, working, &error)->generation == 2);
                REQUIRE(reopened.object(actor, working, pipe, &error)->content == original_pipe);
                REQUIRE(reopened.objectHistoryForRevision(actor, working, &error).size() == 2);
                REQUIRE(reopened.objectGroupsForRevision(actor, working, &error).size() == 1);

                // A failed multi-object mutation must roll back ALL changes.
                REQUIRE(!reopened.commitObjectGroup(actor, branch, 2, {
                    {pipe, pipe_kind, changed_pipe, 1},
                    {junction, junction_kind, QByteArray("bad stale version"), 0}
                }, &error));
                REQUIRE(reopened.revision(actor, working, &error)->generation == 2);
                REQUIRE(reopened.object(actor, working, pipe, &error)->content == original_pipe);
                REQUIRE(reopened.objectHistoryForRevision(actor, working, &error).size() == 2);
                error.clear();

                const std::optional<QUuid> second_group = reopened.commitObjectGroup(actor, branch, 2, {
                    {pipe, pipe_kind, changed_pipe, 1},
                    {junction, junction_kind, std::nullopt, 1}
                }, &error);
                REQUIRE(second_group.has_value());
                REQUIRE(!reopened.commitObjectGroup(actor, branch, 2, {
                    {pipe, pipe_kind, QByteArray("stale"), 2}
                }, &error));
                error.clear();
                REQUIRE(reopened.undoObjectGroup(actor, branch, 4, &error) == second_group);
                REQUIRE(reopened.object(actor, working, pipe, &error)->content == original_pipe);
                REQUIRE(!reopened.object(actor, working, junction, &error)->deleted);
                REQUIRE(reopened.redoObjectGroup(actor, branch, 6, &error) == second_group);
                REQUIRE(reopened.object(actor, working, pipe, &error)->content == changed_pipe);
                REQUIRE(reopened.object(actor, working, junction, &error)->deleted);
                REQUIRE(reopened.objectHistoryForRevision(actor, working, &error).size() == 8);
                REQUIRE(reopened.objectGroupsForRevision(actor, working, &error).size() == 4);

                const std::optional<QByteArray> manifest = reopened.snapshot(actor, working, &error);
                REQUIRE(manifest.has_value());
                REQUIRE(manifest->startsWith("AOWIS-OBJECT-MANIFEST-V1"));
                const std::optional<aowis::db::Finalization> object_finalized =
                    reopened.finalize(actor, branch, 8, QStringLiteral("Grouped objects"),
                                      QStringLiteral("PostgreSQL history parity"), &error);
                REQUIRE(object_finalized.has_value());
                REQUIRE(object_finalized->snapshot_sha256 == QCryptographicHash::hash(
                    *manifest, QCryptographicHash::Sha256));
                REQUIRE(reopened.verifyProject(actor, object_project->project_uuid).valid);
                REQUIRE(reopened.snapshot(actor, working, &error) == manifest);
                REQUIRE(reopened.object(actor, object_finalized->new_working_revision_uuid,
                                        pipe, &error)->content == changed_pipe);
                REQUIRE(reopened.objectGroupsForRevision(actor,
                    object_finalized->new_working_revision_uuid, &error).isEmpty());

                // Check that the finalized journal survives a full close and
                // reopening, including its UUIDs and SHA-256 ancestry.
                const QUuid finalized_project_uuid = object_project->project_uuid;
                const QUuid finalized_revision_uuid = working;
                const QUuid finalized_pipe_uuid = pipe;
                reopened.close();
                REQUIRE(reopened.openPostgres(QString::fromUtf8(pg_database_name), core_schema, &error));
                REQUIRE(reopened.verifyProject(actor, finalized_project_uuid).valid);
                REQUIRE(reopened.object(actor, finalized_revision_uuid,
                                        finalized_pipe_uuid, &error)->content == changed_pipe);
                REQUIRE(reopened.objectHistoryForRevision(actor, finalized_revision_uuid, &error).size() == 8);
                REQUIRE(reopened.objectGroupsForRevision(actor, finalized_revision_uuid, &error).size() == 4);
                // Verify real hydraulic Model persistence against PostgreSQL,
                // including native-UUID graph indexes and finalized ancestry.
                const std::optional<aowis::db::ProjectCreation> hydraulic_project =
                    reopened.createProject(actor, QStringLiteral("PostgreSQL hydraulic model"), &error);
                REQUIRE(hydraulic_project.has_value());
                const QUuid hydraulic_branch = hydraulic_project->main_branch_uuid;
                const QUuid hydraulic_revision = hydraulic_project->working_revision_uuid;
                NetworkHydraulic network;
                network.uuid = createUuidV7();
                network.id = QStringLiteral("PG-NET");
                network.title_line_1 = QStringLiteral("PostgreSQL hydraulic é 水");
                network.options_hydraulic.maximum_trials = 37;
                HydraulicNodeReservoir source;
                source.uuid = createUuidV7();
                source.id = QStringLiteral("SOURCE");
                HydraulicNodeJunction hydraulic_junction;
                hydraulic_junction.uuid = createUuidV7();
                hydraulic_junction.id = QStringLiteral("J1");
                hydraulic_junction.elevation_m = 123.456789;
                hydraulic_junction.coordinate_wgs84.longitude_deg = 18.123456789012345;
                hydraulic_junction.coordinate_wgs84.latitude_deg = 11.987654321012345;
                HydraulicLinkPipe hydraulic_pipe;
                hydraulic_pipe.uuid = createUuidV7();
                hydraulic_pipe.node_uuid_from = source.uuid;
                hydraulic_pipe.node_uuid_to = hydraulic_junction.uuid;
                hydraulic_pipe.diameter_mm = 160.25;
                hydraulic_pipe.length_measured_m = 230.75;
                HydraulicDemandPoint demand_point;
                demand_point.uuid = createUuidV7();
                demand_point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
                demand_point.attachment.pipe_uuid = hydraulic_pipe.uuid;
                demand_point.attachment.pipe_position = 0.375;
                demand_point.attachment.pipe_allocation_mode =
                    HydraulicDemandPointPipeAllocationMode::AssignedJunction;
                demand_point.attachment.pipe_assigned_junction_uuid = hydraulic_junction.uuid;
                network.nodes_reservoirs.append(source);
                network.nodes_junctions.append(hydraulic_junction);
                network.links_pipes.append(hydraulic_pipe);
                network.demand_points.append(demand_point);

                const QByteArray model_bytes = aowis::db::ModelCodec<NetworkHydraulic>::encode(network);
                REQUIRE(!model_bytes.isEmpty());
                REQUIRE(reopened.saveHydraulicNetwork(actor, hydraulic_branch, 0, network, &error));
                const std::optional<NetworkHydraulic> loaded = reopened.loadHydraulicNetwork(
                    actor, hydraulic_revision, network.uuid, &error);
                REQUIRE(loaded.has_value());
                REQUIRE(aowis::db::ModelCodec<NetworkHydraulic>::encode(*loaded) == model_bytes);
                const std::optional<aowis::db::RevisionInfo> initial_model_revision =
                    reopened.revision(actor, hydraulic_revision, &error);
                REQUIRE(initial_model_revision.has_value());
                REQUIRE(initial_model_revision->generation == 5);
                REQUIRE(reopened.objects(actor, hydraulic_revision, &error).size() == 5);
                REQUIRE(reopened.objectGroupsForRevision(actor, hydraulic_revision, &error).size() == 1);

                // Query the physical PostgreSQL graph indexes using the
                // independent QPSQL connection to verify native UUID columns.
                REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("SET search_path TO %1").arg(core_schema)));
                REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                    "SELECT pg_typeof(entity_uuid)::text FROM hydraulic_network_members "
                    "WHERE revision_uuid=? AND network_uuid=? LIMIT 1")));
                ddl.addBindValue(postgres_contract.bindUuid(hydraulic_revision));
                ddl.addBindValue(postgres_contract.bindUuid(network.uuid));
                REQUIRE_SQL(ddl, ddl.exec());
                REQUIRE_SQL(ddl, ddl.next());
                REQUIRE(ddl.value(0).toString() == QStringLiteral("uuid"));
                ddl.finish();
                REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                    "SELECT COUNT(*) FROM hydraulic_network_members WHERE revision_uuid=? AND network_uuid=?")));
                ddl.addBindValue(postgres_contract.bindUuid(hydraulic_revision));
                ddl.addBindValue(postgres_contract.bindUuid(network.uuid));
                REQUIRE_SQL(ddl, ddl.exec());
                REQUIRE_SQL(ddl, ddl.next());
                REQUIRE(ddl.value(0).toInt() == 4);
                ddl.finish();
                REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                    "SELECT COUNT(*) FROM hydraulic_network_references WHERE revision_uuid=? AND network_uuid=?")));
                ddl.addBindValue(postgres_contract.bindUuid(hydraulic_revision));
                ddl.addBindValue(postgres_contract.bindUuid(network.uuid));
                REQUIRE_SQL(ddl, ddl.exec());
                REQUIRE_SQL(ddl, ddl.next());
                REQUIRE(ddl.value(0).toInt() > 0);
                ddl.finish();

                REQUIRE(!reopened.saveHydraulicNetwork(actor, hydraulic_branch, 0, network, &error));
                const std::optional<aowis::db::RevisionInfo> after_stale =
                    reopened.revision(actor, hydraulic_revision, &error);
                REQUIRE(after_stale.has_value() && after_stale->generation == 5);
                error.clear();
                HydraulicLinkPipe invalid_pipe = hydraulic_pipe;
                invalid_pipe.node_uuid_to = createUuidV7();
                REQUIRE(!aowis::db::putHydraulicEntity(reopened, actor, hydraulic_branch,
                                                       invalid_pipe, 1, &error));
                const std::optional<aowis::db::RevisionInfo> after_invalid =
                    reopened.revision(actor, hydraulic_revision, &error);
                REQUIRE(after_invalid.has_value() && after_invalid->generation == 5);
                error.clear();
                HydraulicNodeJunction updated_junction = hydraulic_junction;
                updated_junction.elevation_m = 201.125;
                REQUIRE(aowis::db::putHydraulicEntity(reopened, actor, hydraulic_branch,
                                                      updated_junction, 1, &error));
                const std::optional<NetworkHydraulic> after_edit = reopened.loadHydraulicNetwork(
                    actor, hydraulic_revision, network.uuid, &error);
                REQUIRE(after_edit.has_value());
                REQUIRE(after_edit->nodes_junctions.first().elevation_m == updated_junction.elevation_m);
                const std::optional<aowis::db::RevisionInfo> after_edit_revision =
                    reopened.revision(actor, hydraulic_revision, &error);
                REQUIRE(after_edit_revision.has_value() && after_edit_revision->generation == 6);
                const std::optional<QUuid> undone =
                    reopened.undoObjectGroup(actor, hydraulic_branch, 6, &error);
                REQUIRE(undone.has_value());
                const std::optional<NetworkHydraulic> after_undo = reopened.loadHydraulicNetwork(
                    actor, hydraulic_revision, network.uuid, &error);
                REQUIRE(after_undo.has_value());
                REQUIRE(after_undo->nodes_junctions.first().elevation_m == hydraulic_junction.elevation_m);
                REQUIRE(reopened.redoObjectGroup(actor, hydraulic_branch, 7, &error) == undone);
                const std::optional<NetworkHydraulic> after_redo = reopened.loadHydraulicNetwork(
                    actor, hydraulic_revision, network.uuid, &error);
                REQUIRE(after_redo.has_value());
                REQUIRE(after_redo->nodes_junctions.first().elevation_m == updated_junction.elevation_m);
                REQUIRE(reopened.objectGroupsForRevision(actor, hydraulic_revision, &error).size() == 4);

                const std::optional<QByteArray> model_manifest =
                    reopened.snapshot(actor, hydraulic_revision, &error);
                REQUIRE(model_manifest.has_value());
                const std::optional<aowis::db::Finalization> model_seal = reopened.finalize(
                    actor, hydraulic_branch, 8, QStringLiteral("Hydraulic PostgreSQL"),
                    QStringLiteral("Model and reference integrity"), &error);
                REQUIRE(model_seal.has_value());
                REQUIRE(model_seal->snapshot_sha256 == QCryptographicHash::hash(
                    *model_manifest, QCryptographicHash::Sha256));
                REQUIRE(reopened.verifyProject(actor, hydraulic_project->project_uuid).valid);
                REQUIRE(reopened.loadHydraulicNetwork(actor, model_seal->new_working_revision_uuid,
                                                     network.uuid, &error).has_value());
                const std::optional<aowis::db::BranchInfo> design_branch = reopened.createBranch(
                    actor, hydraulic_project->project_uuid, QStringLiteral("pg-alternative"),
                    hydraulic_revision, &error);
                REQUIRE(design_branch.has_value());
                REQUIRE(reopened.loadHydraulicNetwork(actor, design_branch->head_revision_uuid,
                                                     network.uuid, &error).has_value());

                const QUuid persisted_hydraulic_project = hydraulic_project->project_uuid;
                const QUuid persisted_hydraulic_network = network.uuid;
                reopened.close();
                REQUIRE(reopened.openPostgres(QString::fromUtf8(pg_database_name), core_schema, &error));
                REQUIRE(reopened.verifyProject(actor, persisted_hydraulic_project).valid);
                const std::optional<NetworkHydraulic> recovered = reopened.loadHydraulicNetwork(
                    actor, hydraulic_revision, persisted_hydraulic_network, &error);
                REQUIRE(recovered.has_value());
                REQUIRE(recovered->nodes_junctions.first().elevation_m == updated_junction.elevation_m);
                REQUIRE(reopened.loadHydraulicNetwork(actor, design_branch->head_revision_uuid,
                                                     persisted_hydraulic_network, &error).has_value());

                // Cover multiple 256-reference CTE batches and a partial last
                // batch on QPSQL. Bound UUIDs inside VALUES must remain native
                // PostgreSQL UUIDs; SQLite's BLOB representation is different.
                const std::optional<aowis::db::ProjectCreation> batch_project =
                    reopened.createProject(actor, QStringLiteral("PostgreSQL reference batches"), &error);
                REQUIRE(batch_project.has_value());
                NetworkHydraulic batch_network;
                batch_network.uuid = createUuidV7();
                batch_network.id = QStringLiteral("PG-REFERENCE-BATCH");
                HydraulicNodeReservoir batch_source;
                batch_source.uuid = createUuidV7();
                batch_source.id = QStringLiteral("BATCH-SOURCE");
                batch_network.nodes_reservoirs.append(batch_source);
                for (int i = 0; i < 260; ++i)
                {
                    HydraulicNodeJunction node;
                    node.uuid = createUuidV7();
                    node.id = QStringLiteral("BATCH-J-%1").arg(i);
                    batch_network.nodes_junctions.append(node);
                    HydraulicLinkPipe link;
                    link.uuid = createUuidV7();
                    link.id = QStringLiteral("BATCH-P-%1").arg(i);
                    link.node_uuid_from = batch_source.uuid;
                    link.node_uuid_to = node.uuid;
                    link.length_measured_m = 100.0;
                    link.diameter_mm = 150.0;
                    batch_network.links_pipes.append(link);
                }
                REQUIRE(reopened.saveHydraulicNetwork(actor, batch_project->main_branch_uuid,
                                                      0, batch_network, &error));
                const std::optional<NetworkHydraulic> reloaded_batch =
                    reopened.loadHydraulicNetwork(actor, batch_project->working_revision_uuid,
                                                  batch_network.uuid, &error);
                REQUIRE(reloaded_batch.has_value());
                REQUIRE(reloaded_batch->links_pipes.size() == 260);
                REQUIRE(reloaded_batch->nodes_junctions.size() == 260);
                REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                    "SELECT COUNT(*) FROM hydraulic_network_references "
                    "WHERE revision_uuid=? AND network_uuid=?")));
                ddl.addBindValue(postgres_contract.bindUuid(batch_project->working_revision_uuid));
                ddl.addBindValue(postgres_contract.bindUuid(batch_network.uuid));
                REQUIRE_SQL(ddl, ddl.exec());
                REQUIRE_SQL(ddl, ddl.next());
                REQUIRE(ddl.value(0).toInt() == 520);
                ddl.finish();
                // The server-side fresh-import batches still produce exactly
                // one immutable event per object, strictly ordered across
                // all 128-object batch boundaries, with one edit group.
                const std::optional<aowis::db::RevisionInfo> batch_revision =
                    reopened.revision(actor, batch_project->working_revision_uuid, &error);
                REQUIRE(batch_revision.has_value() && batch_revision->generation == 522);
                REQUIRE(reopened.objectGroupsForRevision(
                    actor, batch_project->working_revision_uuid, &error).size() == 1);
                REQUIRE_SQL(ddl, ddl.prepare(QStringLiteral(
                    "SELECT COUNT(*),MIN(e.sequence),MAX(e.sequence),"
                    "COUNT(DISTINCT ge.position) "
                    "FROM object_group_events ge "
                    "JOIN object_events e ON e.uuid=ge.object_event_uuid "
                    "JOIN object_groups g ON g.uuid=ge.group_uuid "
                    "WHERE g.revision_uuid=?")));
                ddl.addBindValue(postgres_contract.bindUuid(batch_project->working_revision_uuid));
                REQUIRE_SQL(ddl, ddl.exec());
                REQUIRE_SQL(ddl, ddl.next());
                REQUIRE(ddl.value(0).toLongLong() == 522);
                REQUIRE(ddl.value(1).toLongLong() == 1);
                REQUIRE(ddl.value(2).toLongLong() == 522);
                REQUIRE(ddl.value(3).toLongLong() == 522);
                ddl.finish();
                reopened.close();
            }
            REQUIRE_SQL(ddl, ddl.exec(QStringLiteral("DROP SCHEMA %1 CASCADE").arg(core_schema)));

            pg_connection.close();
            pg_connection = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(pg_connection_name);
    }
    return 0;
}
