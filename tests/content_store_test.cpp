#include "../src/content_store.h"
#include "../src/sqlite_backend.h"
#include "../src/postgres_backend.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

using aowis::db::content_store::Kind;
namespace content = aowis::db::content_store;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; \
        return false; \
    } \
} while (false)

// The exact same assertions run against SQLite and live PostgreSQL.
bool exercise(const QSqlDatabase &db, QString &error)
{
    const QByteArray bytes("pipe\0\xff", 6);
    const QByteArray blob_hash = content::digest(Kind::Blob, bytes);
    const QByteArray tree_hash = content::digest(Kind::Tree, bytes);
    REQUIRE(blob_hash.size() == 32);
    // Independent canonical test vectors; a future code change must not
    // silently change content identities or backend interoperability.
    REQUIRE(blob_hash.toHex() == QByteArray("66e0144940dbcf49299ce8f1e1b450ab9387299f828a5f5a32bdb1ad0376a99f"));
    REQUIRE(tree_hash.toHex() == QByteArray("5067d4b7b3a303e8ff889f2655894b3d33ccf8b40c0c2be3bd6d0a00165532af"));
    REQUIRE(blob_hash != tree_hash);
    REQUIRE(content::put(db, Kind::Blob, bytes, &error) == blob_hash);
    REQUIRE(content::put(db, Kind::Blob, bytes, &error) == blob_hash);
    REQUIRE(content::get(db, Kind::Blob, blob_hash, &error) == bytes);
    REQUIRE(content::put(db, Kind::Tree, bytes, &error) == tree_hash);
    REQUIRE(content::get(db, Kind::Tree, tree_hash, &error) == bytes);
    REQUIRE(!content::get(db, Kind::Blob, tree_hash, &error));
    REQUIRE(error.contains(QStringLiteral("kind")));
    error.clear();

    const std::optional<QHash<QByteArray, QByteArray>> repeated = content::getMany(
        db, Kind::Blob, {blob_hash, blob_hash}, &error);
    REQUIRE(repeated && repeated->size() == 1 && repeated->value(blob_hash) == bytes);
    REQUIRE(!content::getMany(db, Kind::Blob, {tree_hash}, &error));
    REQUIRE(error.contains(QStringLiteral("Invalid content object")));
    REQUIRE(!content::getMany(db, Kind::Blob, {QByteArray(31, 'a')}, &error));
    REQUIRE(error.contains(QStringLiteral("length")));
    REQUIRE(!content::getMany(db, Kind::Blob, {QByteArray(32, 'a')}, &error));
    REQUIRE(error.contains(QStringLiteral("not found")));
    error.clear();

    const QByteArray empty;
    const QByteArray empty_hash = content::digest(Kind::Blob, empty);
    REQUIRE(empty_hash.toHex() == QByteArray("51b2bdbcd8d961e7147b06e25fa7133801038376ef412a529832ac5e05bc429a"));
    REQUIRE(content::put(db, Kind::Blob, empty, &error) == empty_hash);
    const std::optional<QByteArray> read_empty = content::get(db, Kind::Blob, empty_hash, &error);
    REQUIRE(read_empty.has_value() && read_empty->isEmpty());
    REQUIRE(!content::get(db, Kind::Blob, QByteArray(31, 'a'), &error));
    REQUIRE(error.contains(QStringLiteral("length")));
    error.clear();
    REQUIRE(!content::get(db, Kind::Blob, QByteArray(32, 'a'), &error));
    REQUIRE(error.contains(QStringLiteral("not found")));
    error.clear();

    QSqlQuery sql(db);
    REQUIRE(sql.exec(QStringLiteral("SELECT COUNT(*) FROM content_objects")));
    REQUIRE(sql.next());
    REQUIRE(sql.value(0).toLongLong() == 3);
    sql.finish();
    // SQL-level protection, even before any revision starts referencing blobs.
    REQUIRE(sql.exec(QStringLiteral("SAVEPOINT content_mutation_check")));
    REQUIRE(sql.prepare(QStringLiteral("UPDATE content_objects SET payload=? WHERE sha256=?")));
    sql.addBindValue(QByteArray("tampered"));
    sql.addBindValue(blob_hash);
    REQUIRE(!sql.exec());
    sql.finish();
    REQUIRE(sql.exec(QStringLiteral("ROLLBACK TO SAVEPOINT content_mutation_check")));
    REQUIRE(sql.exec(QStringLiteral("RELEASE SAVEPOINT content_mutation_check")));
    REQUIRE(content::get(db, Kind::Blob, blob_hash, &error) == bytes);

    REQUIRE(sql.exec(QStringLiteral("SAVEPOINT content_deletion_check")));
    REQUIRE(sql.prepare(QStringLiteral("DELETE FROM content_objects WHERE sha256=?")));
    sql.addBindValue(blob_hash);
    REQUIRE(!sql.exec());
    sql.finish();
    REQUIRE(sql.exec(QStringLiteral("ROLLBACK TO SAVEPOINT content_deletion_check")));
    REQUIRE(sql.exec(QStringLiteral("RELEASE SAVEPOINT content_deletion_check")));
    REQUIRE(content::get(db, Kind::Blob, blob_hash, &error) == bytes);
    // New objects are transactional, just like their eventual tree roots.
    const QByteArray rolled_back_bytes("transaction rollback probe");
    const QByteArray rolled_back_hash = content::digest(Kind::Blob, rolled_back_bytes);
    REQUIRE(sql.exec(QStringLiteral("SAVEPOINT content_rollback_check")));
    REQUIRE(content::put(db, Kind::Blob, rolled_back_bytes, &error) == rolled_back_hash);
    REQUIRE(sql.exec(QStringLiteral("ROLLBACK TO SAVEPOINT content_rollback_check")));
    REQUIRE(sql.exec(QStringLiteral("RELEASE SAVEPOINT content_rollback_check")));
    error.clear();
    REQUIRE(!content::get(db, Kind::Blob, rolled_back_hash, &error));
    REQUIRE(error.contains(QStringLiteral("not found")));
    error.clear();
    REQUIRE(content::get(db, Kind::Blob, blob_hash, &error) == bytes);

    // More than one SQL batch, including a duplicate requested hash and
    // mixed payload sizes, must return every verified object exactly once.
    QVector<QByteArray> payloads;
    for (int index = 0; index < 260; ++index)
        payloads.append(QByteArray("batch-") + QByteArray::number(index));
    payloads.append(payloads.first()); // Duplicate in the same insert batch.
    const std::optional<QVector<QByteArray>> inserted = content::putMany(
        db, Kind::Blob, payloads, &error);
    REQUIRE(inserted && inserted->size() == payloads.size());
    QVector<QByteArray> all_hashes = *inserted;
    REQUIRE(all_hashes.last() == all_hashes.first());
    for (int index = 0; index < 260; ++index)
        REQUIRE(all_hashes.at(index) == content::digest(Kind::Blob, payloads.at(index)));
    // Re-inserting all values must reuse the same immutable records.
    REQUIRE(content::putMany(db, Kind::Blob, payloads, &error) == inserted);
    const std::optional<QVector<QByteArray>> no_values = content::putMany(
        db, Kind::Blob, {}, &error);
    REQUIRE(no_values && no_values->isEmpty());
    const std::optional<QVector<QByteArray>> empty_batch = content::putMany(
        db, Kind::Blob, {QByteArray("")}, &error);
    REQUIRE(empty_batch && empty_batch->size() == 1 && empty_batch->first() == empty_hash);
    const std::optional<QHash<QByteArray, QByteArray>> many = content::getMany(
        db, Kind::Blob, all_hashes, &error);
    REQUIRE(many && many->size() == 260);
    for (int index = 0; index < 260; ++index)
    {
        REQUIRE(many->value(all_hashes.at(index)) ==
                QByteArray("batch-") + QByteArray::number(index));
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temp;
    if (!temp.isValid())
        return 1;
    const QString file = temp.filePath(QStringLiteral("content.sqlite"));
    const QByteArray original("pipe\0\xff", 6);
    const QByteArray identity = content::digest(Kind::Blob, original);

    aowis::db::SqliteBackend sqlite;
    {
        QSqlDatabase connection;
        if (!sqlite.open(connection, QStringLiteral("content-sqlite"), file, &error) ||
            !sqlite.configure(connection, &error) ||
            !sqlite.beginWrite(connection, &error) ||
            !sqlite.createSchema(connection, &error) ||
            !sqlite.commit(connection, &error) ||
            !sqlite.verifySchema(connection, &error) ||
            !exercise(connection, error))
        {
            qCritical().noquote() << "SQLite content-store test failed:" << error;
            return 1;
        }
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("content-sqlite"));
    {
        QSqlDatabase connection;
        if (!sqlite.open(connection, QStringLiteral("content-reopen"), file, &error) ||
            !sqlite.configure(connection, &error) ||
            !sqlite.verifySchema(connection, &error) ||
            content::get(connection, Kind::Blob, identity, &error) != original)
        {
            qCritical().noquote() << "SQLite reopen failed:" << error;
            return 1;
        }
        // Simulate a privileged/manual corruption, then verify read-side hash
        // detection. Test database only; discard all changes afterwards.
        QSqlQuery sql(connection);
        if (!sql.exec(QStringLiteral("SAVEPOINT corrupt_content")) ||
            !sql.exec(QStringLiteral("DROP TRIGGER content_objects_no_update")) ||
            !sql.prepare(QStringLiteral("UPDATE content_objects SET payload=? WHERE sha256=?")))
            return 1;
        sql.addBindValue(QByteArray("modified"));
        sql.addBindValue(identity);
        if (!sql.exec())
            return 1;
        sql.finish();
        error.clear();
        if (content::get(connection, Kind::Blob, identity, &error) ||
            !error.contains(QStringLiteral("SHA-256 mismatch")))
            return 1;
        error.clear();
        if (content::getMany(connection, Kind::Blob, {identity}, &error) ||
            !error.contains(QStringLiteral("SHA-256 mismatch")))
            return 1;
        sql = QSqlQuery();
        QSqlQuery cleanup(connection);
        if (!cleanup.exec(QStringLiteral("ROLLBACK TO SAVEPOINT corrupt_content")) ||
            !cleanup.exec(QStringLiteral("RELEASE SAVEPOINT corrupt_content")))
            return 1;
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("content-reopen"));

    const QByteArray pg_database_name = qgetenv("AOWIS_DB_TEST_PG_DB");
    if (qgetenv("AOWIS_DB_TEST_PG_REQUIRED") == QByteArray("1") && pg_database_name.isEmpty())
        return 1;
    if (!pg_database_name.isEmpty())
    {
        aowis::db::PostgresBackend postgres;
        const QString name = QStringLiteral("content-postgres");
        {
            QSqlDatabase connection;
            if (!postgres.open(connection, name, QString::fromUtf8(pg_database_name), &error) ||
                !postgres.configure(connection, &error) ||
                !postgres.beginWrite(connection, &error))
            {
                qCritical().noquote() << "PostgreSQL content-store setup failed:" << error;
                return 1;
            }
            const QString schema = QStringLiteral("aowis_content_probe_") +
                QUuid::createUuid().toString(QUuid::Id128);
            QSqlQuery sql(connection);
            if (!sql.exec(QStringLiteral("CREATE SCHEMA %1").arg(schema)) ||
                !sql.exec(QStringLiteral("SET LOCAL search_path TO %1").arg(schema)) ||
                !postgres.createSchema(connection, &error) ||
                !postgres.verifySchema(connection, &error) ||
                !exercise(connection, error))
            {
                qCritical().noquote() << "PostgreSQL content-store test failed:" << error << sql.lastError().text();
                postgres.rollback(connection);
                return 1;
            }
            sql = QSqlQuery();
            postgres.rollback(connection); // dispose of the transaction-local schema
            connection.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    return 0;
}
