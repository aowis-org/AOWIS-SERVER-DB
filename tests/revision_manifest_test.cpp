#include <aowis/db/database.h>
#include <aowis/db/revision_manifest.h>

#include <algorithm>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

using aowis::db::Database;
using aowis::db::OperationContext;
using aowis::db::RevisionManifest;

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ \
        << #condition << error; return false; } \
} while (false)

namespace
{
bool exercise(Database &db, QString &error)
{
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(
        QStringLiteral("manifest-admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext actor{admin->uuid};
    const OperationContext stranger{QUuid::createUuid()};

    // Opaque snapshots must use a canonical ZERO root, not a fake Merkle root.
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        actor, QStringLiteral("History exchange"), &error);
    REQUIRE(project.has_value());
    REQUIRE(!db.exportRevisionManifest(actor, project->working_revision_uuid, &error));
    error.clear();
    REQUIRE(!db.exportRevisionContentInventory(actor, project->working_revision_uuid, &error));
    error.clear();
    REQUIRE(db.replaceWorkingSnapshot(actor, project->main_branch_uuid, 0,
                                      QByteArray("first snapshot"), &error));
    const std::optional<aowis::db::Finalization> first = db.finalize(actor,
        project->main_branch_uuid, 1, QStringLiteral("Initial Ü"),
        QStringLiteral("A non-ASCII message €"), &error);
    REQUIRE(first.has_value());
    REQUIRE(!db.exportRevisionManifest(stranger, first->finalized_revision_uuid, &error));
    error.clear();
    const std::optional<QByteArray> first_bytes = db.exportRevisionManifest(
        actor, first->finalized_revision_uuid, &error);
    REQUIRE(first_bytes.has_value());
    REQUIRE(db.exportRevisionManifest(actor, first->finalized_revision_uuid, &error) == first_bytes);
    const std::optional<RevisionManifest> first_manifest =
        aowis::db::decodeRevisionManifest(*first_bytes, &error);
    REQUIRE(first_manifest.has_value());
    REQUIRE(first_manifest->revision_uuid == first->finalized_revision_uuid);
    REQUIRE(first_manifest->branch_uuid == project->main_branch_uuid);
    REQUIRE(first_manifest->project_uuid == project->project_uuid);
    REQUIRE(!first_manifest->object_mode && first_manifest->object_root_sha256.isEmpty());
    REQUIRE(first_manifest->parents.isEmpty());
    REQUIRE(first_manifest->chain_sha256 == first->chain_sha256);
    // Export EXACT hash preimage: it includes the original before/after
    // snapshot payloads and is independently gated by history.read.
    REQUIRE(!db.exportRevisionOperationJournal(stranger,
        first->finalized_revision_uuid, &error));
    error.clear();
    const std::optional<QByteArray> first_journal =
        db.exportRevisionOperationJournal(actor, first->finalized_revision_uuid, &error);
    REQUIRE(first_journal.has_value());
    REQUIRE(first_journal->startsWith(QByteArray("AOWIS-OPERATIONS-V1\0", 20)));
    REQUIRE(first_journal->contains(QByteArray("first snapshot")));
    REQUIRE(QCryptographicHash::hash(*first_journal, QCryptographicHash::Sha256) ==
            first_manifest->operation_history_sha256);
    REQUIRE(aowis::db::verifyRevisionOperationJournal(*first_journal,
        first->finalized_revision_uuid, first_manifest->operation_history_sha256, &error));
    // Finalization seals the original working revision and creates a new one.
    // Only the new successor is still editable and must be rejected.
    REQUIRE(first->new_working_revision_uuid != project->working_revision_uuid);
    REQUIRE(!db.exportRevisionOperationJournal(actor,
        first->new_working_revision_uuid, &error));
    error.clear();
    QByteArray invalid_journal = *first_journal;
    invalid_journal[invalid_journal.size()-1] =
        static_cast<char>(invalid_journal.back() ^ 1);
    REQUIRE(!aowis::db::verifyRevisionOperationJournal(invalid_journal,
        first->finalized_revision_uuid, first_manifest->operation_history_sha256, &error));
    error.clear();
    REQUIRE(!aowis::db::verifyRevisionOperationJournal(*first_journal,
        QUuid::createUuid(), first_manifest->operation_history_sha256, &error));
    error.clear();
    REQUIRE(!aowis::db::verifyRevisionOperationJournal(first_journal->left(16),
        first->finalized_revision_uuid, first_manifest->operation_history_sha256, &error));
    error.clear();
    // The decoder must reject malformed bytes even if the caller supplies
    // their newly calculated checksum (a valid hash is not valid framing).
    QByteArray malformed_journal = *first_journal + QByteArray("junk");
    REQUIRE(!aowis::db::verifyRevisionOperationJournal(malformed_journal,
        first->finalized_revision_uuid,
        QCryptographicHash::hash(malformed_journal, QCryptographicHash::Sha256), &error));
    error.clear();
    QByteArray bad_order = *first_journal;
    // Magic (20) + event count (8) + three UUIDs (48) = sequence offset 76.
    bad_order[83] = '\x00'; // First event sequence must be 1, not 0.
    REQUIRE(!aowis::db::verifyRevisionOperationJournal(bad_order,
        first->finalized_revision_uuid,
        QCryptographicHash::hash(bad_order, QCryptographicHash::Sha256), &error));
    error.clear();
    const std::optional<QVector<QByteArray>> first_parents =
        db.exportRevisionParentManifests(actor, first->finalized_revision_uuid, &error);
    REQUIRE(first_parents.has_value() && first_parents->isEmpty());

    error.clear();
    REQUIRE(!db.exportRevisionContentInventory(actor, first->finalized_revision_uuid, &error));
    REQUIRE(aowis::db::encodeRevisionManifest(*first_manifest, &error) == first_bytes);

    REQUIRE(db.replaceWorkingSnapshot(actor, project->main_branch_uuid, 0,
                                      QByteArray("second snapshot"), &error));
    const std::optional<aowis::db::Finalization> second = db.finalize(actor,
        project->main_branch_uuid, 1, QStringLiteral("Successor"), {}, &error);
    REQUIRE(second.has_value());
    const std::optional<QByteArray> second_bytes = db.exportRevisionManifest(
        actor, second->finalized_revision_uuid, &error);
    REQUIRE(second_bytes.has_value());
    const std::optional<RevisionManifest> second_manifest =
        aowis::db::decodeRevisionManifest(*second_bytes, &error);
    REQUIRE(second_manifest && second_manifest->parents.size() == 1);
    REQUIRE(second_manifest->parents.at(0).first == first->finalized_revision_uuid);
    REQUIRE(second_manifest->parents.at(0).second == first->chain_sha256);
    REQUIRE(second_manifest->chain_sha256 == second->chain_sha256);
    REQUIRE(!db.exportRevisionParentManifests(stranger,
        second->finalized_revision_uuid, &error));
    error.clear();
    const std::optional<QVector<QByteArray>> second_parents =
        db.exportRevisionParentManifests(actor, second->finalized_revision_uuid, &error);
    REQUIRE(second_parents.has_value() && second_parents->size() == 1);
    REQUIRE(second_parents->at(0) == *first_bytes);
    const std::optional<QByteArray> second_journal =
        db.exportRevisionOperationJournal(actor, second->finalized_revision_uuid, &error);
    REQUIRE(second_journal.has_value());
    REQUIRE(second_journal->contains(QByteArray("second snapshot")));
    REQUIRE(aowis::db::verifyRevisionOperationJournal(*second_journal,
        second->finalized_revision_uuid, second_manifest->operation_history_sha256, &error));


    // An independent branch retains ordered ancestry without rewriting UUIDs.
    const std::optional<aowis::db::BranchInfo> alternate = db.createBranch(actor,
        project->project_uuid, QStringLiteral("alternate"), first->finalized_revision_uuid, &error);
    REQUIRE(alternate.has_value());
    const std::optional<aowis::db::Finalization> other = db.finalize(actor,
        alternate->uuid, 0, QStringLiteral("Fork"), {}, &error);
    REQUIRE(other.has_value());
    const std::optional<QByteArray> other_bytes = db.exportRevisionManifest(
        actor, other->finalized_revision_uuid, &error);
    REQUIRE(other_bytes.has_value());
    const std::optional<RevisionManifest> other_manifest =
        aowis::db::decodeRevisionManifest(*other_bytes, &error);
    REQUIRE(other_manifest && other_manifest->parents.size() == 1);
    REQUIRE(other_manifest->parents.first().first == first->finalized_revision_uuid);
    REQUIRE(other_manifest->branch_uuid == alternate->uuid);
    const std::optional<QVector<QByteArray>> other_parents =
        db.exportRevisionParentManifests(actor, other->finalized_revision_uuid, &error);
    REQUIRE(other_parents.has_value() && other_parents->size() == 1);
    REQUIRE(other_parents->first() == *first_bytes);


    // Object-mode metadata must carry the actual Merkle root hash.
    const std::optional<aowis::db::ProjectCreation> object_project = db.createProject(
        actor, QStringLiteral("Objects"), &error);
    REQUIRE(object_project.has_value());
    REQUIRE(db.putObject(actor, object_project->main_branch_uuid, QUuid::createUuid(),
                         QStringLiteral("test.item"), QByteArray("payload"), -1, &error));
    const std::optional<aowis::db::Finalization> object_revision = db.finalize(actor,
        object_project->main_branch_uuid, 1, QStringLiteral("Object root"), {}, &error);
    REQUIRE(object_revision.has_value());
    const std::optional<QByteArray> object_bytes = db.exportRevisionManifest(
        actor, object_revision->finalized_revision_uuid, &error);
    REQUIRE(object_bytes.has_value());
    const std::optional<RevisionManifest> object_manifest =
        aowis::db::decodeRevisionManifest(*object_bytes, &error);
    REQUIRE(object_manifest && object_manifest->object_mode);
    REQUIRE(object_manifest->object_root_sha256.size() == 32);
    REQUIRE(object_manifest->chain_sha256 == object_revision->chain_sha256);
    REQUIRE(aowis::db::encodeRevisionManifest(*object_manifest, &error) == object_bytes);
    // Object-mode revisions have a separately sealed object/group journal;
    // their operation journal may legitimately contain zero snapshot edits.
    const std::optional<QByteArray> object_journal =
        db.exportRevisionOperationJournal(actor, object_revision->finalized_revision_uuid, &error);
    REQUIRE(object_journal.has_value());
    REQUIRE(object_journal->size() == 28); // Magic (20) + zero event count (8).
    REQUIRE(aowis::db::verifyRevisionOperationJournal(*object_journal,
        object_revision->finalized_revision_uuid, object_manifest->operation_history_sha256,
        &error));


    // A finalized Merkle revision exposes verified, sorted, unique hashes
    // without exporting mutable SQL state. For one model object, the tree
    // is a single compressed leaf and one separately addressed model blob.
    error.clear();
    REQUIRE(!db.exportRevisionContentInventory(stranger,
        object_revision->finalized_revision_uuid, &error));
    error.clear();
    const std::optional<aowis::db::RevisionContentInventory> inventory =
        db.exportRevisionContentInventory(actor,
            object_revision->finalized_revision_uuid, &error);
    REQUIRE(inventory.has_value());
    REQUIRE(inventory->revision_uuid == object_revision->finalized_revision_uuid);
    REQUIRE(inventory->object_root_sha256 == object_manifest->object_root_sha256);
    REQUIRE(inventory->tree_sha256.size() == 1);
    REQUIRE(inventory->tree_sha256.first() == inventory->object_root_sha256);
    REQUIRE(inventory->blob_sha256.size() == 1);
    REQUIRE(inventory->blob_sha256.first().size() == 32);
    const std::optional<aowis::db::RevisionContentInventory> repeated_inventory =
        db.exportRevisionContentInventory(actor,
            object_revision->finalized_revision_uuid, &error);
    REQUIRE(repeated_inventory.has_value());
    REQUIRE(repeated_inventory->blob_sha256 == inventory->blob_sha256);

    // A synthetic two-parent merge proves that order affects the existing
    // chain hash and round-trips byte-for-byte through the transport codec.
    RevisionManifest synthetic = *second_manifest;
    synthetic.parents.append(qMakePair(other->finalized_revision_uuid, other->chain_sha256));
    synthetic.chain_sha256 = aowis::db::revisionManifestChainHash(
        synthetic.project_uuid, synthetic.branch_uuid, synthetic.revision_uuid,
        synthetic.parents, synthetic.number, synthetic.created_at_ms,
        synthetic.finalized_at_ms, synthetic.title, synthetic.message,
        synthetic.snapshot_sha256, synthetic.created_by_uuid,
        synthetic.finalized_by_uuid, synthetic.operation_history_sha256,
        QByteArray(32, '\0'));
    const std::optional<QByteArray> merge_bytes = aowis::db::encodeRevisionManifest(synthetic, &error);
    REQUIRE(merge_bytes.has_value());
    REQUIRE(aowis::db::decodeRevisionManifest(*merge_bytes, &error)->parents == synthetic.parents);
    std::swap(synthetic.parents[0], synthetic.parents[1]);
    REQUIRE(!aowis::db::encodeRevisionManifest(synthetic, &error));
    error.clear();
    synthetic.parents[1] = synthetic.parents[0];
    REQUIRE(!aowis::db::encodeRevisionManifest(synthetic, &error));
    error.clear();

    // Corruption, truncated/trailing framing, invalid UTF-8, invalid mode,
    // and a changed sealed parent hash must all fail closed.
    QByteArray tampered = *second_bytes;
    tampered[tampered.size() - 1] = static_cast<char>(tampered.back() ^ 1);
    REQUIRE(!aowis::db::decodeRevisionManifest(tampered, &error));
    error.clear();
    REQUIRE(!aowis::db::decodeRevisionManifest(second_bytes->left(second_bytes->size()-1), &error));
    error.clear();
    REQUIRE(!aowis::db::decodeRevisionManifest(*second_bytes + QByteArray("x"), &error));
    error.clear();
    REQUIRE(!aowis::db::decodeRevisionManifest(QByteArray(16*1024*1024+1, '\0'), &error));
    error.clear();
    QByteArray invalid_utf8 = *first_bytes;
    const qsizetype umlaut = invalid_utf8.indexOf(QByteArray("\xC3\x9C", 2));
    REQUIRE(umlaut >= 0);
    invalid_utf8[umlaut] = '\xFF';
    REQUIRE(!aowis::db::decodeRevisionManifest(invalid_utf8, &error));
    error.clear();
    QByteArray invalid_mode = *second_bytes;
    invalid_mode[static_cast<qsizetype>(sizeof("AOWIS-REVISION-MANIFEST-V1"))] = '\x03';
    REQUIRE(!aowis::db::decodeRevisionManifest(invalid_mode, &error));
    error.clear();
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    REQUIRE(db.verifyProject(actor, object_project->project_uuid).valid);
    return true;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    {
        Database db;
        if (!db.openSqlite(directory.filePath(QStringLiteral("portable.sqlite")), &error) ||
            !exercise(db, error))
        {
            qCritical().noquote() << "SQLite revision manifest test:" << error;
            return 1;
        }
        db.close();
    }

    const QByteArray postgres_database = qgetenv("AOWIS_DB_TEST_PG_DB");
    if (qgetenv("AOWIS_DB_TEST_PG_REQUIRED") == QByteArray("1") && postgres_database.isEmpty())
        return 1;
    if (!postgres_database.isEmpty())
    {
        const QString schema = QStringLiteral("aowis_manifest_") +
            QUuid::createUuid().toString(QUuid::Id128);
        const QString name = QStringLiteral("revision-manifest-schema-setup");
        {
            QSqlDatabase setup = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), name);
            setup.setDatabaseName(QString::fromUtf8(postgres_database));
            if (!setup.open())
            {
                qCritical().noquote() << "PostgreSQL manifest test connection:" << setup.lastError().text();
                return 1;
            }
            QSqlQuery query(setup);
            if (!query.exec(QStringLiteral("CREATE SCHEMA %1").arg(schema)))
            {
                qCritical().noquote() << query.lastError().text();
                return 1;
            }
            query = QSqlQuery();
            setup.close();
        }
        QSqlDatabase::removeDatabase(name);
        {
            Database db;
            if (!db.openPostgres(QString::fromUtf8(postgres_database), schema, &error) ||
                !exercise(db, error))
            {
                qCritical().noquote() << "PostgreSQL revision manifest test:" << error;
                return 1;
            }
            db.close();
        }
        {
            QSqlDatabase cleanup = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), name);
            cleanup.setDatabaseName(QString::fromUtf8(postgres_database));
            if (!cleanup.open())
                return 1;
            QSqlQuery query(cleanup);
            if (!query.exec(QStringLiteral("DROP SCHEMA %1 CASCADE").arg(schema)))
            {
                qCritical().noquote() << "PostgreSQL cleanup:" << query.lastError().text();
                return 1;
            }
            query = QSqlQuery();
            cleanup.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    return 0;
}
