#include <aowis/db/database.h>

#include "../src/sqlite_backend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::Database;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());

    // Backend-specific mechanics, isolated from production schema migrations.
    const QString adapter_path = temp.filePath(QStringLiteral("adapter.sqlite"));
    const QString adapter_connection = QStringLiteral("sqlite-adapter-contract");
    {
        QSqlDatabase connection;
        REQUIRE(aowis::db::sqlite_backend::open(connection, adapter_connection, adapter_path, &error));
        REQUIRE(aowis::db::sqlite_backend::configure(connection, &error));
        {
            QSqlQuery settings(connection);
            REQUIRE(settings.exec(QStringLiteral("PRAGMA foreign_keys")));
            REQUIRE(settings.next() && settings.value(0).toInt() == 1);
            settings.finish();
            REQUIRE(settings.exec(QStringLiteral("PRAGMA journal_mode")));
            REQUIRE(settings.next() && settings.value(0).toString().compare(
                QStringLiteral("wal"), Qt::CaseInsensitive) == 0);
            settings.finish();
        }
        REQUIRE(aowis::db::sqlite_backend::schemaVersion(connection, &error) == 0);
        REQUIRE(!aowis::db::sqlite_backend::setSchemaVersion(connection, -1, &error));
        REQUIRE(!aowis::db::sqlite_backend::setSchemaVersion(connection, 14, &error));
        error.clear();

        REQUIRE(aowis::db::sqlite_backend::beginWrite(connection, &error));
        {
            QSqlQuery create(connection);
            REQUIRE(create.exec(QStringLiteral("CREATE TABLE rolled_back (value INTEGER)")));
        }
        REQUIRE(aowis::db::sqlite_backend::setSchemaVersion(connection, 6, &error));
        REQUIRE(aowis::db::sqlite_backend::schemaVersion(connection, &error) == 6);
        aowis::db::sqlite_backend::rollback(connection);
        REQUIRE(aowis::db::sqlite_backend::schemaVersion(connection, &error) == 0);
        {
            QSqlQuery missing(connection);
            REQUIRE(!missing.exec(QStringLiteral("SELECT value FROM rolled_back")));
        }

        REQUIRE(aowis::db::sqlite_backend::beginWrite(connection, &error));
        REQUIRE(!aowis::db::sqlite_backend::beginWrite(connection, &error));
        error.clear();
        {
            QSqlQuery create(connection);
            REQUIRE(create.exec(QStringLiteral(
                "CREATE TABLE parent (id INTEGER PRIMARY KEY)")));
            REQUIRE(create.exec(QStringLiteral(
                "CREATE TABLE child (parent_id INTEGER NOT NULL REFERENCES parent(id))")));
        }
        REQUIRE(aowis::db::sqlite_backend::setSchemaVersion(connection, 13, &error));
        REQUIRE(aowis::db::sqlite_backend::commit(connection, &error));
        REQUIRE(aowis::db::sqlite_backend::schemaVersion(connection, &error) == 13);
        {
            QSqlQuery foreign_key(connection);
            REQUIRE(!foreign_key.exec(QStringLiteral("INSERT INTO child(parent_id) VALUES(99)")));
        }
        connection.close();
        connection = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(adapter_connection);

    // Production lifecycle must still migrate a pristine SQLite database and
    // reject a forged schema version whose immutability guard is missing.
    const QString production_path = temp.filePath(QStringLiteral("production.sqlite"));
    {
        Database database;
        REQUIRE(database.openSqlite(production_path, &error));
        REQUIRE(database.isOpen());
        REQUIRE(!database.openSqlite(production_path, &error));
        database.close();
        REQUIRE(!database.isOpen());
        error.clear();
        REQUIRE(database.openSqlite(production_path, &error));
        database.close();
    }
    const QString inspection_name = QStringLiteral("sqlite-backend-inspection");
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), inspection_name);
        inspection.setDatabaseName(production_path);
        REQUIRE(inspection.open());
        {
            QSqlQuery query(inspection);
            REQUIRE(query.exec(QStringLiteral("PRAGMA user_version")));
            REQUIRE(query.next() && query.value(0).toInt() == 13);
            query.finish();
            REQUIRE(query.exec(QStringLiteral(
                "DROP TRIGGER user_sessions_immutable_identity_v13")));
            query.finish();
        }
        inspection.close();
        inspection = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(inspection_name);

    {
        Database database;
        error.clear();
        REQUIRE(!database.openSqlite(production_path, &error));
        REQUIRE(!database.isOpen());
        REQUIRE(error.contains(QStringLiteral("session-redaction")));
    }
    return 0;
}
