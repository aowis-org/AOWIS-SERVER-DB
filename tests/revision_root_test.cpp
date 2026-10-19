#include "../src/content_store.h"
#include "../src/merkle_tree.h"
#include "../src/revision_root.h"
#include "../src/sqlite_backend.h"
#include "../src/postgres_backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::StorageBackend;
namespace content = aowis::db::content_store;
namespace tree = aowis::db::merkle_tree;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; \
        return false; \
    } \
} while (false)

bool execute(QSqlQuery &query, QString &error)
{
    if (query.exec())
        return true;
    error = query.lastError().text();
    return false;
}

bool exercise(const QSqlDatabase &database, const StorageBackend &backend, QString &error)
{
    const QUuid actor = QUuid::createUuid();
    const QUuid project = QUuid::createUuid();
    const QUuid branch = QUuid::createUuid();
    const QUuid revision = QUuid::createUuid();
    const QUuid pipe = QUuid::createUuid();
    const QUuid junction = QUuid::createUuid();
    QSqlQuery query(database);
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO users(uuid,username,created_at_ms) VALUES (?,?,?)")));
    query.addBindValue(backend.bindUuid(actor));
    query.addBindValue(QStringLiteral("root-test-actor"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO projects(uuid,name,created_at_ms,created_by_uuid) VALUES (?,?,?,?)")));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(QStringLiteral("Root test project"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO branches(uuid,project_uuid,name,created_by_uuid) VALUES (?,?,?,?)")));
    query.addBindValue(backend.bindUuid(branch));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(QStringLiteral("main"));
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,object_mode) "
        "VALUES (?,?,?,1,'working',?,?,1)")));
    query.addBindValue(backend.bindUuid(revision));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(backend.bindUuid(branch));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
        "VALUES (?,?,?,?,?,?)")));
    for (const QUuid &key : {pipe, junction})
    {
        query.bindValue(0, backend.bindUuid(revision));
        query.bindValue(1, backend.bindUuid(key));
        query.bindValue(2, QStringLiteral("hydraulic.pipe"));
        query.bindValue(3, key == pipe ? QByteArray("old pipe") : QByteArray("junction"));
        query.bindValue(4, 1);
        query.bindValue(5, 0);
        REQUIRE(execute(query, error));
    }
    query.finish();

    const std::optional<QByteArray> root1 = aowis::db::buildRevisionRoot(database, backend, revision, {}, &error);
    REQUIRE(root1 && root1->size() == 32);
    REQUIRE(aowis::db::verifyWorkingObjectsAgainstRoot(database, backend, revision, *root1, &error));
    REQUIRE(aowis::db::buildRevisionRoot(database, backend, revision, {}, &error) == root1);
    const std::optional<QByteArray> old_pipe = tree::lookup(database, *root1, pipe, &error);
    REQUIRE(old_pipe && content::get(database, content::Kind::Blob, *old_pipe, &error));
    const std::optional<aowis::db::MerkleObjectRecord> historic_pipe =
        aowis::db::readRevisionObject(database, *root1, pipe, &error);
    REQUIRE(historic_pipe && historic_pipe->uuid == pipe &&
            historic_pipe->kind == QStringLiteral("hydraulic.pipe") &&
            historic_pipe->content == QByteArray("old pipe") &&
            historic_pipe->version == 1 && !historic_pipe->deleted);
    const std::optional<QVector<aowis::db::MerkleObjectRecord>> historic_objects =
        aowis::db::readRevisionObjects(database, *root1, &error);
    REQUIRE(historic_objects && historic_objects->size() == 2);
    REQUIRE(historic_objects->at(0).uuid.toRfc4122() < historic_objects->at(1).uuid.toRfc4122());
    // The merge cursor uses batched decoding of already-validated Merkle
    // entries. It must preserve ordered UUID identity and the sealed bytes.
    const std::optional<QVector<tree::Entry>> listed = tree::entries(database, *root1, &error);
    REQUIRE(listed && listed->size() == 2);
    const std::optional<QVector<aowis::db::MerkleObjectRecord>> page =
        aowis::db::readRevisionObjectEntries(database, *listed, &error);
    REQUIRE(page && page->size() == 2);
    REQUIRE(page->at(0).uuid == historic_objects->at(0).uuid);
    REQUIRE(page->at(0).content == historic_objects->at(0).content);
    REQUIRE(page->at(1).uuid == historic_objects->at(1).uuid);
    REQUIRE(page->at(1).content == historic_objects->at(1).content);
    REQUIRE(aowis::db::readRevisionObjectEntries(database, {}, &error)->isEmpty());

    const std::optional<QVector<aowis::db::MerkleObjectRecord>> selected =
        aowis::db::readRevisionObjectsSelected(database, *root1, QSet<QUuid>{pipe}, &error);
    REQUIRE(selected && selected->size() == 1 && selected->first().uuid == pipe &&
            selected->first().content == QByteArray("old pipe"));
    const std::optional<QVector<aowis::db::MerkleObjectRecord>> selected_pair =
        aowis::db::readRevisionObjectsSelected(database, *root1,
                                               QSet<QUuid>{pipe, junction}, &error);
    REQUIRE(selected_pair && selected_pair->size() == 2);
    REQUIRE(selected_pair->at(0).uuid == historic_objects->at(0).uuid &&
            selected_pair->at(0).content == historic_objects->at(0).content &&
            selected_pair->at(1).uuid == historic_objects->at(1).uuid &&
            selected_pair->at(1).content == historic_objects->at(1).content);
    REQUIRE(!aowis::db::readRevisionObjectsSelected(database, *root1,
        QSet<QUuid>{pipe, QUuid::createUuid()}, &error));
    REQUIRE(error.contains(QStringLiteral("missing")));
    error.clear();
    const std::optional<QVector<aowis::db::MerkleObjectRecord>> empty_selection =
        aowis::db::readRevisionObjectsSelected(database, *root1, {}, &error);
    REQUIRE(empty_selection && empty_selection->isEmpty());
    REQUIRE(!aowis::db::readRevisionObjectsSelected(database, *root1,
        QSet<QUuid>{QUuid::createUuid()}, &error));
    REQUIRE(error.contains(QStringLiteral("missing")));
    error.clear();
    REQUIRE(!aowis::db::readRevisionObject(database, *root1, QUuid::createUuid(), &error));
    REQUIRE(error == QStringLiteral("Object not found"));
    error.clear();

    // Valid content-addressed bytes can still be an invalid model record.
    const std::optional<QByteArray> malformed_blob = content::put(
        database, content::Kind::Blob, QByteArray("invalid record"), &error);
    REQUIRE(malformed_blob);
    const std::optional<QByteArray> malformed_root = tree::update(
        database, *root1, tree::Change{pipe, *malformed_blob}, &error);
    REQUIRE(malformed_root);
    REQUIRE(!aowis::db::readRevisionObject(database, *malformed_root, pipe, &error));
    REQUIRE(error.contains(QStringLiteral("magic")));
    error.clear();
    REQUIRE(!aowis::db::readRevisionObjects(database, *malformed_root, &error));
    REQUIRE(error.contains(QStringLiteral("magic")));
    error.clear();

    REQUIRE(query.prepare(QStringLiteral(
        "UPDATE revision_objects SET content=?,version=2 WHERE revision_uuid=? AND object_uuid=?")));
    query.addBindValue(QByteArray("new pipe"));
    query.addBindValue(backend.bindUuid(revision));
    query.addBindValue(backend.bindUuid(pipe));
    REQUIRE(execute(query, error));
    query.finish();
    // An ordinary edit leaves an append-only object event. A successor can
    // reuse root1 and update only the changed object, even if the project
    // contains many other, unchanged objects.
    REQUIRE(query.prepare(QStringLiteral(
        "UPDATE revisions SET generation=1 WHERE uuid=?")));
    query.addBindValue(backend.bindUuid(revision));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO object_events(uuid,revision_uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,"
        "kind,action,before_version,after_version,before_content,after_content,before_deleted) "
        "VALUES (?,?,?,?,1,?,'hydraulic.pipe','put',1,2,?,?,0)")));
    query.addBindValue(backend.bindUuid(QUuid::createUuid()));
    query.addBindValue(backend.bindUuid(revision));
    query.addBindValue(backend.bindUuid(pipe));
    query.addBindValue(backend.bindUuid(actor));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(QByteArray("old pipe"));
    query.addBindValue(QByteArray("new pipe"));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(aowis::db::verifyRevisionRoot(database, *root1, &error));
    REQUIRE(!aowis::db::verifyWorkingObjectsAgainstRoot(database, backend, revision, *root1, &error));
    REQUIRE(error.contains(QStringLiteral("Revision Merkle object contents differ from SQL state")));
    error.clear();
    const std::optional<QByteArray> root2 = aowis::db::buildRevisionRoot(database, backend, revision, {}, &error);
    REQUIRE(root2 && *root1 != *root2);
    REQUIRE(aowis::db::buildRevisionRoot(database, backend, revision, *root1, &error) == root2);
    REQUIRE(aowis::db::verifyWorkingObjectsAgainstRoot(database, backend, revision, *root2, &error));
    // Independent semantic replay: the one pipe event transforms root1 into
    // root2. The untouched junction is inherited from the first-parent root.
    REQUIRE(aowis::db::verifyRevisionJournalReplay(
        database, backend, revision, *root1, *root2, &error));
    // The same event cannot explain the initial two-object snapshot, nor can
    // it produce the old state again. This detects omitted journal changes.
    REQUIRE(!aowis::db::verifyRevisionJournalReplay(
        database, backend, revision, QByteArray(), *root1, &error));
    REQUIRE(error.contains(QStringLiteral("before-state")) ||
            error.contains(QStringLiteral("count")));
    error.clear();
    REQUIRE(tree::lookup(database, *root1, pipe, &error) == old_pipe);
    REQUIRE(aowis::db::readRevisionObject(database, *root1, pipe, &error)->content == QByteArray("old pipe"));
    REQUIRE(aowis::db::readRevisionObject(database, *root2, pipe, &error)->content == QByteArray("new pipe"));
    REQUIRE(tree::lookup(database, *root1, junction, &error) ==
            tree::lookup(database, *root2, junction, &error));

    // Tombstones must be committed to the root: a delete changes content
    // without deleting the identity or throwing away its object version.
    REQUIRE(query.prepare(QStringLiteral(
        "UPDATE revision_objects SET content=NULL,version=3,deleted=1 "
        "WHERE revision_uuid=? AND object_uuid=?")));
    query.addBindValue(backend.bindUuid(revision));
    query.addBindValue(backend.bindUuid(pipe));
    REQUIRE(execute(query, error));
    query.finish();
    const std::optional<QByteArray> root3 = aowis::db::buildRevisionRoot(database, backend, revision, {}, &error);
    REQUIRE(root3 && *root3 != *root2);
    REQUIRE(aowis::db::verifyWorkingObjectsAgainstRoot(database, backend, revision, *root3, &error));
    const std::optional<aowis::db::MerkleObjectRecord> tombstone =
        aowis::db::readRevisionObject(database, *root3, pipe, &error);
    REQUIRE(tombstone && tombstone->deleted && tombstone->version == 3 &&
            tombstone->content.isEmpty());
    // The second state change is not yet journaled: a fast SQL-only update
    // must not pass the independent historical replay verifier.
    REQUIRE(!aowis::db::verifyRevisionJournalReplay(
        database, backend, revision, *root1, *root3, &error));
    REQUIRE(error.contains(QStringLiteral("differs")));
    error.clear();
    REQUIRE(query.prepare(QStringLiteral(
        "UPDATE revisions SET generation=2 WHERE uuid=?")));
    query.addBindValue(backend.bindUuid(revision));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO object_events(uuid,revision_uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,"
        "kind,action,before_version,after_version,before_content,after_content,before_deleted) "
        "VALUES (?,?,?,?,2,?,'hydraulic.pipe','delete',2,3,?,NULL,0)")));
    query.addBindValue(backend.bindUuid(QUuid::createUuid()));
    query.addBindValue(backend.bindUuid(revision));
    query.addBindValue(backend.bindUuid(pipe));
    query.addBindValue(backend.bindUuid(actor));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(QByteArray("new pipe"));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(aowis::db::verifyRevisionJournalReplay(
        database, backend, revision, *root1, *root3, &error));
    const std::optional<QVector<aowis::db::MerkleObjectRecord>> selected_tombstone =
        aowis::db::readRevisionObjectsSelected(database, *root3,
                                               QSet<QUuid>{pipe, junction}, &error);
    REQUIRE(selected_tombstone && selected_tombstone->size() == 2);
    bool saw_deleted_pipe = false;
    for (const aowis::db::MerkleObjectRecord &record : *selected_tombstone)
    {
        if (record.uuid == pipe)
        {
            REQUIRE(record.deleted && record.version == 3 && record.content.isEmpty());
            saw_deleted_pipe = true;
        }
    }
    REQUIRE(saw_deleted_pipe);

    // Missing and extra immutable entries both invalidate a revision root.
    REQUIRE(query.prepare(QStringLiteral(
        "DELETE FROM revision_objects WHERE revision_uuid=? AND object_uuid=?")));
    query.addBindValue(backend.bindUuid(revision));
    query.addBindValue(backend.bindUuid(pipe));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(!aowis::db::verifyWorkingObjectsAgainstRoot(database, backend, revision, *root3, &error));
    error.clear();

    // The source SQL materialization no longer contains the deleted pipe.
    // New editable heads must be reconstructed from the immutable root, not
    // by copying whatever rows happen to remain on the source revision.
    const QUuid fork_branch = QUuid::createUuid();
    const QUuid fork_revision = QUuid::createUuid();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO branches(uuid,project_uuid,name,created_by_uuid) VALUES (?,?,?,?)")));
    query.addBindValue(backend.bindUuid(fork_branch));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(QStringLiteral("materialized-fork"));
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,object_mode) "
        "VALUES (?,?,?,1,'working',?,?,1)")));
    query.addBindValue(backend.bindUuid(fork_revision));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(backend.bindUuid(fork_branch));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();

    REQUIRE(aowis::db::materializeWorkingObjectsFromRoot(
        database, backend, fork_revision, *root3, &error));
    REQUIRE(aowis::db::verifyWorkingObjectsAgainstRoot(database, backend, fork_revision, *root3, &error));
    // Deliberately forge a journaled resurrection with no corresponding
    // final model update in a separate working revision.
    // Object-event rows themselves are immutable by trigger; corruption is
    // injected on initial INSERT, not by updating already-journaled events.
    REQUIRE(query.prepare(QStringLiteral(
        "UPDATE revisions SET generation=1 WHERE uuid=?")));
    query.addBindValue(backend.bindUuid(fork_revision));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO object_events(uuid,revision_uuid,object_uuid,actor_uuid,sequence,occurred_at_ms,"
        "kind,action,before_version,after_version,before_content,after_content,before_deleted) "
        "VALUES (?,?,?,?,1,?,'hydraulic.pipe','put',3,4,?,?,1)")));
    query.addBindValue(backend.bindUuid(QUuid::createUuid()));
    query.addBindValue(backend.bindUuid(fork_revision));
    query.addBindValue(backend.bindUuid(pipe));
    query.addBindValue(backend.bindUuid(actor));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(QByteArray("forged-before"));
    query.addBindValue(QByteArray("resurrected"));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(!aowis::db::verifyRevisionJournalReplay(
        database, backend, fork_revision, *root3, *root3, &error));
    REQUIRE(error.contains(QStringLiteral("before-state")));
    error.clear();
    REQUIRE(!aowis::db::materializeWorkingObjectsFromRoot(
        database, backend, fork_revision, *root3, &error));
    REQUIRE(error.contains(QStringLiteral("not empty")));
    error.clear();
    REQUIRE(query.prepare(QStringLiteral(
        "SELECT kind,content,version,deleted FROM revision_objects "
        "WHERE revision_uuid=? AND object_uuid=?")));
    query.addBindValue(backend.bindUuid(fork_revision));
    query.addBindValue(backend.bindUuid(pipe));
    REQUIRE(execute(query, error));
    REQUIRE(query.next());
    REQUIRE(query.value(0).toString() == QStringLiteral("hydraulic.pipe"));
    REQUIRE(query.value(1).isNull());
    REQUIRE(query.value(2).toLongLong() == 3);
    REQUIRE(query.value(3).toInt() == 1);
    query.finish();

    // A project-wide verification may check many revisions with shared tree
    // nodes and blobs. Reusing the request-local caches must preserve root
    // validation and reject previously unseen malformed payloads.
    QHash<QByteArray, QByteArray> verified_nodes;
    QSet<QByteArray> verified_blobs;
    REQUIRE(aowis::db::verifyRevisionRootCached(
        database, *root1, verified_nodes, verified_blobs, &error));
    REQUIRE(aowis::db::verifyRevisionRootCached(
        database, *root2, verified_nodes, verified_blobs, &error));
    REQUIRE(aowis::db::verifyRevisionRootCached(
        database, *root3, verified_nodes, verified_blobs, &error));
    REQUIRE(!verified_nodes.isEmpty() && !verified_blobs.isEmpty());
    REQUIRE(!aowis::db::verifyRevisionRootCached(
        database, *malformed_root, verified_nodes, verified_blobs, &error));
    REQUIRE(error.contains(QStringLiteral("magic")));
    error.clear();
    // A new verification must not inherit an old invocation's cache.
    REQUIRE(aowis::db::verifyRevisionRoot(database, *root3, &error));

    // Exercise a working-head reconstruction spanning multiple 64-row SQLite
    // INSERT batches and the 256-object Merkle content fetch boundaries.
    // Keep tombstones and exact versions in the reconstructed state.
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO revision_objects(revision_uuid,object_uuid,kind,content,version,deleted) "
        "VALUES (?,?,?,?,?,?)")));
    QVector<QUuid> added;
    for (int index = 0; index < 261; ++index)
    {
        const QUuid id = QUuid::createUuid();
        const bool deleted = index % 17 == 0;
        added.append(id);
        query.bindValue(0, backend.bindUuid(fork_revision));
        query.bindValue(1, backend.bindUuid(id));
        query.bindValue(2, QStringLiteral("test.item"));
        query.bindValue(3, deleted ? QVariant() : QVariant(QByteArray("payload-") + QByteArray::number(index)));
        query.bindValue(4, index + 1);
        query.bindValue(5, deleted ? 1 : 0);
        REQUIRE(execute(query, error));
    }
    query.finish();
    const std::optional<QByteArray> large_root = aowis::db::buildRevisionRoot(
        database, backend, fork_revision, {}, &error);
    REQUIRE(large_root && aowis::db::verifyRevisionRoot(database, *large_root, &error));
    const QUuid large_branch = QUuid::createUuid();
    const QUuid large_revision = QUuid::createUuid();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO branches(uuid,project_uuid,name,created_by_uuid) VALUES (?,?,?,?)")));
    query.addBindValue(backend.bindUuid(large_branch));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(QStringLiteral("large-materialized-fork"));
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(query.prepare(QStringLiteral(
        "INSERT INTO revisions(uuid,project_uuid,branch_uuid,number,state,created_at_ms,created_by_uuid,object_mode) "
        "VALUES (?,?,?,1,'working',?,?,1)")));
    query.addBindValue(backend.bindUuid(large_revision));
    query.addBindValue(backend.bindUuid(project));
    query.addBindValue(backend.bindUuid(large_branch));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(backend.bindUuid(actor));
    REQUIRE(execute(query, error));
    query.finish();
    REQUIRE(aowis::db::materializeWorkingObjectsFromRoot(
        database, backend, large_revision, *large_root, &error));
    REQUIRE(aowis::db::verifyWorkingObjectsAgainstRoot(
        database, backend, large_revision, *large_root, &error));
    REQUIRE(query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM revision_objects WHERE revision_uuid=?")));
    query.addBindValue(backend.bindUuid(large_revision));
    REQUIRE(execute(query, error));
    REQUIRE(query.next() && query.value(0).toInt() == 263);
    query.finish();
    const std::optional<aowis::db::MerkleObjectRecord> first_added =
        aowis::db::readRevisionObject(database, *large_root, added.first(), &error);
    REQUIRE(first_added && first_added->deleted && first_added->version == 1);
    const std::optional<aowis::db::MerkleObjectRecord> last_added =
        aowis::db::readRevisionObject(database, *large_root, added.last(), &error);
    REQUIRE(last_added && !last_added->deleted && last_added->version == 261 &&
            last_added->content == QByteArray("payload-260"));
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    aowis::db::SqliteBackend sqlite;
    {
        QSqlDatabase db;
        if (!sqlite.open(db, QStringLiteral("revision-root-sqlite"),
                         temporary.filePath(QStringLiteral("root.sqlite")), &error) ||
            !sqlite.configure(db, &error) || !sqlite.createSchema(db, &error) ||
            !sqlite.verifySchema(db, &error) || !exercise(db, sqlite, error))
        {
            qCritical().noquote() << "SQLite revision Merkle root test:" << error;
            return 1;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("revision-root-sqlite"));

    const QByteArray pg_name = qgetenv("AOWIS_DB_TEST_PG_DB");
    if (qgetenv("AOWIS_DB_TEST_PG_REQUIRED") == QByteArray("1") && pg_name.isEmpty())
        return 1;
    if (!pg_name.isEmpty())
    {
        aowis::db::PostgresBackend postgres;
        QSqlDatabase db;
        const QString connection_name = QStringLiteral("revision-root-postgres");
        {
            if (!postgres.open(db, connection_name, QString::fromUtf8(pg_name), &error) ||
                !postgres.configure(db, &error) || !postgres.beginWrite(db, &error))
            {
                qCritical().noquote() << "PostgreSQL revision root setup:" << error;
                return 1;
            }
            QSqlQuery query(db);
            const QString schema = QStringLiteral("aowis_revroot_probe_") +
                QUuid::createUuid().toString(QUuid::Id128);
            if (!query.exec(QStringLiteral("CREATE SCHEMA %1").arg(schema)) ||
                !query.exec(QStringLiteral("SET LOCAL search_path TO %1").arg(schema)) ||
                !postgres.createSchema(db, &error) || !postgres.verifySchema(db, &error) ||
                !exercise(db, postgres, error))
            {
                qCritical().noquote() << "PostgreSQL revision Merkle root test:" << error;
                postgres.rollback(db);
                return 1;
            }
            query = QSqlQuery();
            postgres.rollback(db); // Drop the transaction-local test schema.
            db.close();
        }
        QSqlDatabase::removeDatabase(connection_name);
    }
    return 0;
}
