#include "../src/sqlite_backend.h"
#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::SqliteBackend;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    QString error;
    REQUIRE(directory.isValid());
    SqliteBackend backend;

    const QStringList damage_statements{
        QStringLiteral("DROP TABLE revision_objects"),
        QStringLiteral("DROP INDEX revision_objects_by_kind"),
        QStringLiteral("DROP TRIGGER finalized_object_update"),
        QStringLiteral("DROP TRIGGER branch_working_storage_readonly"),
        QStringLiteral("DROP TRIGGER repository_identity_no_update"),
        QStringLiteral("DROP TABLE repository_identity"),
        QStringLiteral("ALTER TABLE snapshots ADD COLUMN unexpected INTEGER"),
        QStringLiteral("DROP TRIGGER security_audit_readonly_update"),
    };
    const QStringList expected_names{
        QStringLiteral("revision_objects"),
        QStringLiteral("revision_objects_by_kind"),
        QStringLiteral("finalized_object_update"),
        QStringLiteral("branch_working_storage_readonly"),
        QStringLiteral("repository_identity_no_update"),
        QStringLiteral("repository_identity"),
        QStringLiteral("snapshots"),
        QStringLiteral("security_audit_readonly_update"),
    };

    for (qsizetype i = 0; i < damage_statements.size(); ++i)
    {
        const QString name = QStringLiteral("schema-integrity-%1").arg(i);
        const QString path = directory.filePath(name + QStringLiteral(".sqlite"));
        {
            QSqlDatabase db;
            REQUIRE(backend.open(db, name, path, &error));
            REQUIRE(backend.configure(db, &error));
            REQUIRE(backend.beginWrite(db, &error));
            REQUIRE(backend.createSchema(db, &error));
            REQUIRE(backend.commit(db, &error));
            REQUIRE(backend.verifySchema(db, &error));

            QSqlQuery query(db);
            REQUIRE(query.exec(damage_statements.at(i)));
            query.finish();
            error.clear();
            REQUIRE(!backend.verifySchema(db, &error));
            REQUIRE(error.contains(expected_names.at(i)));
            query = QSqlQuery();
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
        // The public open path must reject the same damaged database.
        aowis::db::Database reopened;
        error.clear();
        REQUIRE(!reopened.openSqlite(path, &error));
        REQUIRE(error.contains(expected_names.at(i)));
    }

    // Replacing a trigger with a weaker definition under its original name
    // must fail verification as well; checking the name alone is insufficient.
    const QString name = QStringLiteral("weakened-trigger");
    {
        QSqlDatabase db;
        REQUIRE(backend.open(db, name, directory.filePath(name + QStringLiteral(".sqlite")), &error));
        REQUIRE(backend.configure(db, &error));
        REQUIRE(backend.beginWrite(db, &error));
        REQUIRE(backend.createSchema(db, &error));
        REQUIRE(backend.commit(db, &error));
        REQUIRE(backend.verifySchema(db, &error));
        QSqlQuery query(db);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER finalized_object_update")));
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER finalized_object_update BEFORE UPDATE ON revision_objects "
            "BEGIN SELECT 1; END")));
        query.finish();
        error.clear();
        REQUIRE(!backend.verifySchema(db, &error));
        REQUIRE(error.contains(QStringLiteral("finalized_object_update")));
        query = QSqlQuery();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return 0;
}
