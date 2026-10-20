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
    REQUIRE(!preview->merkle_candidate_scan_used); // Working SQL tips.

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
            "SELECT object_uuid,kind,content,version,deleted FROM revision_objects WHERE storage_uuid=(SELECT "
            "b.working_storage_uuid FROM branches b JOIN revisions r ON r.branch_uuid=b.uuid AND "
            "r.project_uuid=b.project_uuid WHERE r.uuid=? AND r.state='working') ORDER BY object_uuid")));
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

    // Finalized merge sources are exclusively Merkle-backed: old SQL
    // object copies are absent, not merely ignored.
    {
        QSqlDatabase inspection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("merkle-merge-inspection"));
        inspection.setDatabaseName(path);
        REQUIRE(inspection.open());
        QSqlQuery query(inspection);
        REQUIRE(query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM revision_objects WHERE storage_uuid=(SELECT b.working_storage_uuid FROM branches " "b JOIN revisions r ON r.branch_uuid=b.uuid AND r.project_uuid=b.project_uuid WHERE r.uuid=? AND " "r.state='working')")));
        query.addBindValue(base_uuid.toRfc4122());
        REQUIRE(query.exec() && query.next() && query.value(0).toInt() == 0);
        query.finish();
        inspection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merkle-merge-inspection"));
    const std::optional<ObjectMergePreview> sealed_preview =
        db.previewObjectMerge(root, left_uuid, right_uuid, &error);
    REQUIRE(sealed_preview && sealed_preview->changes.size() == preview->changes.size());
    REQUIRE(sealed_preview->conflict_count == preview->conflict_count);
    REQUIRE(sealed_preview->merkle_candidate_scan_used);
    REQUIRE(sealed_preview->merkle_candidate_keys >= static_cast<quint64>(sealed_preview->changes.size()));
    REQUIRE(sealed_preview->merkle_tree_nodes_loaded > 0);
    // Finalized previews no longer decode the entire historical left model to
    // rebuild a SQL-format digest. The sealed root is already an immutable
    // commitment to every version and tombstone, so its transient fingerprint
    // deliberately differs from the working-state CAS digest.
    const std::optional<aowis::db::RevisionInfo> sealed_left = db.revision(root, left_uuid, &error);
    REQUIRE(sealed_left && sealed_left->object_root_sha256.size() == 32);
    QByteArray sealed_preimage("AOWIS-MERGE-SEALED-ROOT-V1");
    sealed_preimage.append('\0');
    sealed_preimage.append(sealed_left->object_root_sha256);
    REQUIRE(sealed_preview->left_physical_sha256 ==
        QCryptographicHash::hash(sealed_preimage, QCryptographicHash::Sha256));
    REQUIRE(sealed_preview->left_physical_sha256 != preview->left_physical_sha256);
    // The pre-finalization SQL scan is an independent full-model oracle. The
    // optimized finalized-tip scan must retain every classification, payload,
    // version, conflict field and auto-merged content in identical UUID order.
    const auto sameObject = [](const std::optional<aowis::db::ModelObject> &a,
                               const std::optional<aowis::db::ModelObject> &b) {
        if (!a || !b)
            return !a && !b;
        return a->uuid == b->uuid && a->kind == b->kind &&
               a->content == b->content && a->version == b->version &&
               a->deleted == b->deleted;
    };
    for (qsizetype i = 0; i < preview->changes.size(); ++i)
    {
        const ObjectMergeChange &old_change = preview->changes.at(i);
        const ObjectMergeChange &new_change = sealed_preview->changes.at(i);
        REQUIRE(new_change.object_uuid == old_change.object_uuid);
        REQUIRE(new_change.kind == old_change.kind);
        REQUIRE(new_change.conflicting_fields == old_change.conflicting_fields);
        REQUIRE(sameObject(new_change.base, old_change.base));
        REQUIRE(sameObject(new_change.left, old_change.left));
        REQUIRE(sameObject(new_change.right, old_change.right));
        REQUIRE(sameObject(new_change.automatically_merged, old_change.automatically_merged));
    }
    const std::optional<ObjectMergePreview> sealed_reversed =
        db.previewObjectMerge(root, right_uuid, left_uuid, &error);
    REQUIRE(sealed_reversed && sealed_reversed->merkle_candidate_scan_used);
    REQUIRE(sealed_reversed->changes.size() == reversed->changes.size());
    REQUIRE(sealed_reversed->conflict_count == reversed->conflict_count);
    for (qsizetype i = 0; i < reversed->changes.size(); ++i)
    {
        REQUIRE(sealed_reversed->changes.at(i).object_uuid == reversed->changes.at(i).object_uuid);
        REQUIRE(sealed_reversed->changes.at(i).kind == reversed->changes.at(i).kind);
    }
    const std::optional<ObjectMergePreview> sealed_self =
        db.previewObjectMerge(root, left_uuid, left_uuid, &error);
    REQUIRE(sealed_self && sealed_self->merkle_candidate_scan_used);
    REQUIRE(sealed_self->changes.isEmpty() && sealed_self->merkle_candidate_keys == 0);
    const std::optional<ObjectMergePreview> sealed_bounded =
        db.previewObjectMergeBounded(root, left_uuid, right_uuid, 11, 1024, &error);
    REQUIRE(sealed_bounded && sealed_bounded->changes.size() == 11);
    REQUIRE(sealed_bounded->merkle_candidate_scan_used);
    REQUIRE(sealed_bounded->merkle_candidate_keys <= 11); // Bounded UUID set.
    REQUIRE(sealed_bounded->merkle_tree_nodes_loaded > 0);
    REQUIRE(sealed_bounded->left_physical_sha256 == sealed_preview->left_physical_sha256);
    REQUIRE(sealed_bounded->conflict_count == sealed_preview->conflict_count);
    for (qsizetype i = 0; i < sealed_bounded->changes.size(); ++i)
    {
        const ObjectMergeChange &bounded_change = sealed_bounded->changes.at(i);
        const ObjectMergeChange &unbounded_change = sealed_preview->changes.at(i);
        REQUIRE(bounded_change.object_uuid == unbounded_change.object_uuid);
        REQUIRE(bounded_change.kind == unbounded_change.kind);
        REQUIRE(bounded_change.conflicting_fields == unbounded_change.conflicting_fields);
        REQUIRE(sameObject(bounded_change.base, unbounded_change.base));
        REQUIRE(sameObject(bounded_change.left, unbounded_change.left));
        REQUIRE(sameObject(bounded_change.right, unbounded_change.right));
        REQUIRE(sameObject(bounded_change.automatically_merged,
                           unbounded_change.automatically_merged));
    }
    // With a smaller candidate budget, the traversal falls back to the full
    // streaming cursors and preserves the original changed-entity error.
    REQUIRE(!db.previewObjectMergeBounded(root, left_uuid, right_uuid, 10, 1024, &error));
    REQUIRE(error == QStringLiteral("Merge preview exceeds changed-entity limit"));
    error.clear();
    REQUIRE(!db.previewObjectMergeBounded(root, left_uuid, right_uuid, 11, 10, &error));
    REQUIRE(error == QStringLiteral("Merge preview exceeds changed-payload limit"));
    error.clear();
    REQUIRE(findChange(*sealed_preview, only_left)->base->content == QByteArray("base"));
    REQUIRE(db.verifyProject(root, project->project_uuid).valid);

    // A self-preview has no candidate objects to fetch. It must still check
    // that the finalized left root exists and matches its own content hash.
    // Full project verification independently catches the same corruption.
    {
        QSqlDatabase tampered = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("sealed-root-tamper"));
        tampered.setDatabaseName(path);
        REQUIRE(tampered.open());
        QSqlQuery update(tampered);
        REQUIRE(update.exec(QStringLiteral(
            "SELECT sql FROM sqlite_master WHERE type='trigger' "
            "AND name='content_objects_no_update'")));
        REQUIRE(update.next());
        const QString original_guard = update.value(0).toString();
        REQUIRE(!original_guard.isEmpty());
        update.finish();

        const QString corrupt_sql = QStringLiteral(
            "UPDATE content_objects SET payload=? WHERE sha256=? AND kind='tree'");
        REQUIRE(update.prepare(corrupt_sql));
        update.addBindValue(QByteArray("corrupt root payload"));
        update.addBindValue(sealed_left->object_root_sha256);
        REQUIRE(!update.exec()); // Ordinary updates must remain prohibited.
        update.finish();

        // Simulate privileged/on-disk corruption in this disposable test DB.
        // Restore the immutable-content guard before exercising read-side
        // verification, so the test cannot silently weaken the schema.
        REQUIRE(update.exec(QStringLiteral("DROP TRIGGER content_objects_no_update")));
        update.finish();
        REQUIRE(update.prepare(corrupt_sql));
        update.addBindValue(QByteArray("corrupt root payload"));
        update.addBindValue(sealed_left->object_root_sha256);
        REQUIRE(update.exec() && update.numRowsAffected() == 1);
        update.finish();
        REQUIRE(update.exec(original_guard));
        update.finish();
        tampered.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("sealed-root-tamper"));
    error.clear();
    REQUIRE(!db.previewObjectMerge(root, left_uuid, left_uuid, &error));
    REQUIRE(!db.verifyProject(root, project->project_uuid).valid);
    return 0;
}
