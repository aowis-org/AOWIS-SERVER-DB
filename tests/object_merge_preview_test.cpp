#include <aowis/db/database.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::AccessScope;
using aowis::db::Database;
using aowis::db::ObjectMergeChange;
using aowis::db::ObjectMergeKind;
using aowis::db::ObjectMergePreview;
using aowis::db::OperationContext;
using aowis::db::PrincipalKind;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

static const ObjectMergeChange *findChange(const ObjectMergePreview &preview, const QUuid &uuid)
{
    for (const ObjectMergeChange &change : preview.changes)
    {
        if (change.object_uuid == uuid)
            return &change;
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("merge-preview.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        root, QStringLiteral("Three way object preview"), &error);
    REQUIRE(project);

    const QUuid only_left = createUuidV7();
    const QUuid only_right = createUuidV7();
    const QUuid both_same = createUuidV7();
    const QUuid conflict = createUuidV7();
    const QUuid delete_modify = createUuidV7();
    const QUuid both_delete = createUuidV7();
    const QUuid unchanged = createUuidV7();
    const QUuid new_same = createUuidV7();
    const QUuid new_clash = createUuidV7();
    const QUuid new_left = createUuidV7();
    const QUuid new_right = createUuidV7();
    const QUuid only_delete = createUuidV7();
    const QString kind = QStringLiteral("hydraulic.label");
    const QVector<QUuid> starting{only_left, only_right, both_same, conflict,
                                 delete_modify, both_delete, unchanged, only_delete};
    for (const QUuid &uuid : starting)
        REQUIRE(db.putObject(root, project->main_branch_uuid, uuid, kind, QByteArray("base"), -1, &error));
    const std::optional<aowis::db::Finalization> finalized_base = db.finalize(
        root, project->main_branch_uuid, starting.size(), QStringLiteral("Base"), {}, &error);
    REQUIRE(finalized_base);
    const QUuid base_uuid = finalized_base->finalized_revision_uuid;

    const std::optional<aowis::db::BranchInfo> left_branch = db.createBranch(root,
        project->project_uuid, QStringLiteral("left"), base_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right_branch = db.createBranch(root,
        project->project_uuid, QStringLiteral("right"), base_uuid, &error);
    REQUIRE(left_branch && right_branch);
    const QUuid left_uuid = left_branch->head_revision_uuid;
    const QUuid right_uuid = right_branch->head_revision_uuid;

    REQUIRE(db.putObject(root, left_branch->uuid, only_left, kind, QByteArray("L"), 1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, only_right, kind, QByteArray("R"), 1, &error));
    REQUIRE(db.putObject(root, left_branch->uuid, both_same, kind, QByteArray("same"), 1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, both_same, kind, QByteArray("same"), 1, &error));
    REQUIRE(db.putObject(root, left_branch->uuid, conflict, kind, QByteArray("left"), 1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, conflict, kind, QByteArray("right"), 1, &error));
    REQUIRE(db.deleteObject(root, left_branch->uuid, delete_modify, 1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, delete_modify, kind, QByteArray("alive"), 1, &error));
    REQUIRE(db.deleteObject(root, left_branch->uuid, both_delete, 1, &error));
    REQUIRE(db.deleteObject(root, right_branch->uuid, both_delete, 1, &error));
    REQUIRE(db.deleteObject(root, left_branch->uuid, only_delete, 1, &error));
    REQUIRE(db.putObject(root, left_branch->uuid, new_same, kind, QByteArray("new"), -1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, new_same, kind, QByteArray("new"), -1, &error));
    REQUIRE(db.putObject(root, left_branch->uuid, new_clash, kind, QByteArray("one"), -1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, new_clash, kind, QByteArray("two"), -1, &error));
    REQUIRE(db.putObject(root, left_branch->uuid, new_left, kind, QByteArray("L"), -1, &error));
    REQUIRE(db.putObject(root, right_branch->uuid, new_right, kind, QByteArray("R"), -1, &error));

    // Read-only operation can inspect an editable tip without advancing its
    // generation, writing an audit event, or changing object versions.
    const qint64 left_generation = db.revision(root, left_uuid, &error)->generation;
    const qint64 right_generation = db.revision(root, right_uuid, &error)->generation;
    const std::optional<ObjectMergePreview> preview = db.previewObjectMerge(root, left_uuid, right_uuid, &error);
    REQUIRE(preview);
    REQUIRE(preview->project_uuid == project->project_uuid);
    REQUIRE(preview->base_revision_uuid == base_uuid);
    REQUIRE(preview->left_generation == left_generation);
    REQUIRE(preview->right_generation == right_generation);
    REQUIRE(preview->conflict_count == 3);
    REQUIRE(preview->changes.size() == 11);
    // Backend UUID conversion must not change the original SQLite physical
    // digest. Independently rebuild it from the stored BLOB values (including
    // tombstones and version counters), as the pre-portability code did.
    REQUIRE(preview->left_physical_sha256.size() == 32);
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("merge-preview-raw-digest"));
        raw.setDatabaseName(path);
        REQUIRE(raw.open());
        QSqlQuery rows(raw);
        REQUIRE(rows.prepare(QStringLiteral(
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects "
            "WHERE revision_uuid=? ORDER BY object_uuid")));
        rows.addBindValue(left_uuid.toRfc4122());
        REQUIRE(rows.exec());
        QCryptographicHash old_format(QCryptographicHash::Sha256);
        const auto addField = [&old_format](const QByteArray &value) {
            old_format.addData(QByteArray::number(value.size()));
            old_format.addData(QByteArrayView(":", 1));
            old_format.addData(value);
        };
        qsizetype rows_seen = 0;
        while (rows.next())
        {
            REQUIRE(rows.value(0).toByteArray().size() == 16);
            addField(rows.value(0).toByteArray());
            addField(rows.value(1).toString().toUtf8());
            addField(rows.value(2).toByteArray());
            addField(QByteArray::number(rows.value(3).toLongLong()));
            addField(QByteArray::number(rows.value(4).toInt()));
            ++rows_seen;
        }
        REQUIRE(!rows.lastError().isValid());
        REQUIRE(rows_seen > 0);
        REQUIRE(old_format.result() == preview->left_physical_sha256);
        rows.finish();
        raw.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge-preview-raw-digest"));
    // Bounded HTTP-style previews must report the same classifications and
    // physical digest, but abort on excessive changed rows or retained bytes.
    const std::optional<ObjectMergePreview> bounded = db.previewObjectMergeBounded(
        root, left_uuid, right_uuid, 11, 1024, &error);
    REQUIRE(bounded && bounded->changes.size() == 11);
    REQUIRE(bounded->conflict_count == preview->conflict_count);
    REQUIRE(bounded->left_physical_sha256 == preview->left_physical_sha256);
    REQUIRE(!db.previewObjectMergeBounded(root, left_uuid, right_uuid, 10, 1024, &error));
    REQUIRE(error == QStringLiteral("Merge preview exceeds changed-entity limit"));
    REQUIRE(!db.previewObjectMergeBounded(root, left_uuid, right_uuid, 11, 10, &error));
    REQUIRE(error == QStringLiteral("Merge preview exceeds changed-payload limit"));
    REQUIRE(!db.previewObjectMergeBounded(root, left_uuid, right_uuid, 0, 1024, &error));
    REQUIRE(error == QStringLiteral("Merge preview limits must be positive"));
    error.clear();
    for (const QUuid &uuid : QVector<QUuid>{only_left, only_right, both_same,
             conflict, delete_modify, both_delete, only_delete, new_same,
             new_clash, new_left, new_right})
        REQUIRE(findChange(*preview, uuid) != nullptr);
    REQUIRE(findChange(*preview, only_left)->kind == ObjectMergeKind::LeftOnly);
    REQUIRE(findChange(*preview, only_right)->kind == ObjectMergeKind::RightOnly);
    REQUIRE(findChange(*preview, both_same)->kind == ObjectMergeKind::BothSame);
    REQUIRE(findChange(*preview, both_same)->left->content == QByteArray("same"));
    REQUIRE(findChange(*preview, conflict)->kind == ObjectMergeKind::Conflict);
    REQUIRE(findChange(*preview, delete_modify)->kind == ObjectMergeKind::Conflict);
    REQUIRE(!findChange(*preview, delete_modify)->left);
    REQUIRE(findChange(*preview, delete_modify)->right->content == QByteArray("alive"));
    REQUIRE(findChange(*preview, both_delete)->kind == ObjectMergeKind::BothSame);
    REQUIRE(!findChange(*preview, both_delete)->left && !findChange(*preview, both_delete)->right);
    REQUIRE(findChange(*preview, only_delete)->kind == ObjectMergeKind::LeftOnly);
    REQUIRE(!findChange(*preview, only_delete)->left);
    REQUIRE(findChange(*preview, new_same)->kind == ObjectMergeKind::BothSame);
    REQUIRE(!findChange(*preview, new_same)->base);
    REQUIRE(findChange(*preview, new_clash)->kind == ObjectMergeKind::Conflict);
    REQUIRE(findChange(*preview, new_left)->kind == ObjectMergeKind::LeftOnly);
    REQUIRE(findChange(*preview, new_right)->kind == ObjectMergeKind::RightOnly);
    REQUIRE(findChange(*preview, unchanged) == nullptr);
    for (qsizetype i = 1; i < preview->changes.size(); ++i)
        REQUIRE(preview->changes.at(i-1).object_uuid.toRfc4122() < preview->changes.at(i).object_uuid.toRfc4122());
    REQUIRE(db.revision(root, left_uuid, &error)->generation == left_generation);
    REQUIRE(db.revision(root, right_uuid, &error)->generation == right_generation);

    const std::optional<ObjectMergePreview> reversed = db.previewObjectMerge(root, right_uuid, left_uuid, &error);
    REQUIRE(reversed && reversed->conflict_count == preview->conflict_count);
    REQUIRE(findChange(*reversed, only_left)->kind == ObjectMergeKind::RightOnly);
    REQUIRE(findChange(*reversed, only_right)->kind == ObjectMergeKind::LeftOnly);
    // Comparing a working head to itself should not mistake identical edits
    // for conflicts merely because both sides differ from the finalized base.
    const std::optional<ObjectMergePreview> self = db.previewObjectMerge(root, left_uuid, left_uuid, &error);
    REQUIRE(self && self->conflict_count == 0);
    REQUIRE(findChange(*self, conflict)->kind == ObjectMergeKind::BothSame);

    // Comparing two branch tips does not give implicit read access to a
    // third branch holding their finalized merge base.
    const std::optional<aowis::db::UserInfo> viewer = db.createUser(root, QStringLiteral("viewer"), &error);
    const std::optional<aowis::db::RoleInfo> role = db.createRole(root, QStringLiteral("merge.viewer"), &error);
    REQUIRE(viewer && role);
    const std::optional<aowis::db::PermissionInfo> read = db.permission(root, QStringLiteral("project.read"), &error);
    REQUIRE(read);
    REQUIRE(db.grantRolePermission(root, role->uuid, read->uuid, &error));
    REQUIRE(db.assignRole(root, PrincipalKind::User, viewer->uuid, role->uuid,
        AccessScope::branch(left_branch->uuid), &error));
    REQUIRE(db.assignRole(root, PrincipalKind::User, viewer->uuid, role->uuid,
        AccessScope::branch(right_branch->uuid), &error));
    const OperationContext restricted{viewer->uuid};
    REQUIRE(db.compareRevisions(restricted, left_uuid, right_uuid, &error));
    REQUIRE(!db.previewObjectMerge(restricted, left_uuid, right_uuid, &error));
    REQUIRE(!db.previewObjectMergeBounded(restricted, left_uuid, right_uuid, 11, 1024, &error));
    REQUIRE(db.assignRole(root, PrincipalKind::User, viewer->uuid, role->uuid,
        AccessScope::branch(project->main_branch_uuid), &error));
    REQUIRE(db.previewObjectMerge(restricted, left_uuid, right_uuid, &error));
    REQUIRE(!db.previewObjectMerge(restricted, left_uuid, createUuidV7(), &error));

    REQUIRE(db.finalize(root, left_branch->uuid, left_generation, QStringLiteral("L"), {}, &error));
    REQUIRE(db.finalize(root, right_branch->uuid, right_generation, QStringLiteral("R"), {}, &error));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<ObjectMergePreview> reopened = db.previewObjectMerge(root, left_uuid, right_uuid, &error);
    REQUIRE(reopened && reopened->changes.size() == preview->changes.size() &&
            reopened->conflict_count == preview->conflict_count);
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);

    // Legacy opaque snapshot histories are intentionally rejected. Mixing
    // raw snapshot data into object-diff results would silently lose data.
    const std::optional<aowis::db::ProjectCreation> legacy = db.createProject(
        root, QStringLiteral("legacy"), &error);
    REQUIRE(legacy);
    REQUIRE(db.replaceWorkingSnapshot(root, legacy->main_branch_uuid, 0, QByteArray("opaque"), &error));
    const std::optional<aowis::db::Finalization> legacy_base = db.finalize(
        root, legacy->main_branch_uuid, 1, QStringLiteral("legacy"), {}, &error);
    REQUIRE(legacy_base);
    const std::optional<aowis::db::BranchInfo> legacy_left = db.createBranch(
        root, legacy->project_uuid, QStringLiteral("A"), legacy_base->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> legacy_right = db.createBranch(
        root, legacy->project_uuid, QStringLiteral("B"), legacy_base->finalized_revision_uuid, &error);
    REQUIRE(legacy_left && legacy_right);
    REQUIRE(!db.previewObjectMerge(root, legacy_left->head_revision_uuid, legacy_right->head_revision_uuid, &error));
    REQUIRE(!db.previewObjectMerge(root, left_uuid, legacy_left->head_revision_uuid, &error));

    // Simulate already-supported criss-cross parent storage; the preview must
    // reject multiple merge bases rather than select an arbitrary one.
    const std::optional<aowis::db::Finalization> left_next = db.finalize(
        root, left_branch->uuid, 0, QStringLiteral("L2"), {}, &error);
    const std::optional<aowis::db::Finalization> right_next = db.finalize(
        root, right_branch->uuid, 0, QStringLiteral("R2"), {}, &error);
    REQUIRE(left_next && right_next);
    {
        QSqlDatabase fixture = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("merge-fixture"));
        fixture.setDatabaseName(path);
        REQUIRE(fixture.open());
        REQUIRE(fixture.transaction());
        QSqlQuery insert(fixture);
        REQUIRE(insert.prepare(QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) VALUES (?,1,?)")));
        insert.addBindValue(left_next->new_working_revision_uuid.toRfc4122());
        insert.addBindValue(right_next->finalized_revision_uuid.toRfc4122());
        REQUIRE(insert.exec());
        insert.finish();
        REQUIRE(insert.prepare(QStringLiteral(
            "INSERT INTO revision_parents(revision_uuid,parent_position,parent_revision_uuid) VALUES (?,1,?)")));
        insert.addBindValue(right_next->new_working_revision_uuid.toRfc4122());
        insert.addBindValue(left_next->finalized_revision_uuid.toRfc4122());
        REQUIRE(insert.exec());
        insert.finish();
        REQUIRE(fixture.commit());
        fixture.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge-fixture"));
    const std::optional<aowis::db::Finalization> left_merge = db.finalize(
        root, left_branch->uuid, 0, QStringLiteral("L3"), {}, &error);
    const std::optional<aowis::db::Finalization> right_merge = db.finalize(
        root, right_branch->uuid, 0, QStringLiteral("R3"), {}, &error);
    REQUIRE(left_merge && right_merge);
    const std::optional<aowis::db::RevisionComparison> two_bases = db.compareRevisions(
        root, left_merge->finalized_revision_uuid, right_merge->finalized_revision_uuid, &error);
    REQUIRE(two_bases && two_bases->merge_base_revision_uuids.size() == 2);
    REQUIRE(!db.previewObjectMerge(root, left_merge->finalized_revision_uuid,
        right_merge->finalized_revision_uuid, &error));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);
    return 0;
}
