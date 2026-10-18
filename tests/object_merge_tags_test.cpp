#include <aowis/db/database.h>
#include <aowis/db/model_codec.h>
#include <aowis/model/uuid.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVector>

#include <cstdio>

using aowis::db::Database;
using aowis::db::ModelCodec;
using aowis::db::ObjectMergeChange;
using aowis::db::ObjectMergeKind;
using aowis::db::ObjectMergePreview;
using aowis::db::ObjectMergeResolution;
using aowis::db::ObjectMergeSide;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

namespace
{
struct Scenario
{
    QUuid uuid;
    HydraulicNodeJunction base;
    HydraulicNodeJunction left;
    HydraulicNodeJunction right;
};

HydraulicNodeJunction makeJunction(const QUuid &uuid, const QString &id,
                                  const QStringList &tags)
{
    HydraulicNodeJunction value;
    value.uuid = uuid;
    value.id = id;
    value.metadata.tags = tags;
    return value;
}

const ObjectMergeChange *findChange(const ObjectMergePreview &preview, const QUuid &uuid)
{
    for (const ObjectMergeChange &change : preview.changes)
        if (change.object_uuid == uuid)
            return &change;
    return nullptr;
}

ObjectMergeResolution tagsChoice(const QUuid &uuid, const ObjectMergeSide side)
{
    ObjectMergeResolution choice;
    choice.object_uuid = uuid;
    choice.field_resolutions = {{QStringLiteral("metadata.tags"), side}};
    return choice;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString error;
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("append-only-tags.sqlite"));
    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(
        QStringLiteral("admin"), &error);
    REQUIRE(admin);
    const OperationContext actor{admin->uuid};
    const std::optional<aowis::db::ProjectCreation> project = db.createProject(
        actor, QStringLiteral("append-only tag merge"), &error);
    REQUIRE(project);
    const QString kind = QStringLiteral("hydraulic.junction");

    QVector<Scenario> scenarios;
    const auto addScenario = [&scenarios](const QString &id, const QStringList &base_tags,
                                           const QStringList &left_tags,
                                           const QStringList &right_tags) {
        const QUuid uuid = createUuidV7();
        const HydraulicNodeJunction baseline = makeJunction(uuid, id, base_tags);
        HydraulicNodeJunction left = baseline;
        HydraulicNodeJunction right = baseline;
        left.metadata.tags = left_tags;
        right.metadata.tags = right_tags;
        scenarios.append(Scenario{uuid, baseline, left, right});
    };
    // Distinct suffixes combine in left-then-right order, retaining both
    // branches' order within their respective suffixes.
    addScenario(QStringLiteral("add"), {QStringLiteral("common")},
        {QStringLiteral("common"), QStringLiteral("left-1"), QStringLiteral("left-2")},
        {QStringLiteral("common"), QStringLiteral("right-1"), QStringLiteral("right-2")});
    // Same complete edit is BothSame, not an artificial conflict.
    addScenario(QStringLiteral("same"), {QStringLiteral("common")},
        {QStringLiteral("common"), QStringLiteral("shared")},
        {QStringLiteral("common"), QStringLiteral("shared")});
    // Shared new tags, reordering, deletion, and re-appending an existing
    // base tag must all fail closed to the explicit metadata.tags choice.
    addScenario(QStringLiteral("overlap"), {QStringLiteral("common")},
        {QStringLiteral("common"), QStringLiteral("left"), QStringLiteral("shared")},
        {QStringLiteral("common"), QStringLiteral("shared"), QStringLiteral("right")});
    addScenario(QStringLiteral("reorder"), {QStringLiteral("common"), QStringLiteral("fixed")},
        {QStringLiteral("fixed"), QStringLiteral("common"), QStringLiteral("left")},
        {QStringLiteral("common"), QStringLiteral("fixed"), QStringLiteral("right")});
    addScenario(QStringLiteral("remove"), {QStringLiteral("common"), QStringLiteral("fixed")},
        {QStringLiteral("common")},
        {QStringLiteral("common"), QStringLiteral("fixed"), QStringLiteral("right")});
    addScenario(QStringLiteral("duplicate"), {QStringLiteral("common"), QStringLiteral("fixed")},
        {QStringLiteral("common"), QStringLiteral("fixed"), QStringLiteral("common")},
        {QStringLiteral("common"), QStringLiteral("fixed"), QStringLiteral("right")});

    // The tags addition must combine with unrelated changes to metadata.
    scenarios[0].left.metadata.description = QStringLiteral("local description");
    scenarios[0].right.metadata.comment = QStringLiteral("remote comment");

    for (const Scenario &scenario : scenarios)
        REQUIRE(db.putObject(actor, project->main_branch_uuid, scenario.uuid, kind,
            ModelCodec<HydraulicNodeJunction>::encode(scenario.base), -1, &error));
    const std::optional<aowis::db::Finalization> baseline = db.finalize(
        actor, project->main_branch_uuid, scenarios.size(), QStringLiteral("baseline"), {}, &error);
    REQUIRE(baseline);
    const std::optional<aowis::db::BranchInfo> left_branch = db.createBranch(
        actor, project->project_uuid, QStringLiteral("left"), baseline->finalized_revision_uuid, &error);
    const std::optional<aowis::db::BranchInfo> right_branch = db.createBranch(
        actor, project->project_uuid, QStringLiteral("right"), baseline->finalized_revision_uuid, &error);
    REQUIRE(left_branch && right_branch);
    for (const Scenario &scenario : scenarios)
    {
        REQUIRE(db.putObject(actor, left_branch->uuid, scenario.uuid, kind,
            ModelCodec<HydraulicNodeJunction>::encode(scenario.left), 1, &error));
        REQUIRE(db.putObject(actor, right_branch->uuid, scenario.uuid, kind,
            ModelCodec<HydraulicNodeJunction>::encode(scenario.right), 1, &error));
    }
    const std::optional<aowis::db::Finalization> left_final = db.finalize(
        actor, left_branch->uuid, scenarios.size(), QStringLiteral("left"), {}, &error);
    const std::optional<aowis::db::Finalization> right_final = db.finalize(
        actor, right_branch->uuid, scenarios.size(), QStringLiteral("right"), {}, &error);
    REQUIRE(left_final && right_final);
    const QUuid working = left_final->new_working_revision_uuid;
    const std::optional<ObjectMergePreview> preview = db.previewObjectMerge(
        actor, working, right_final->finalized_revision_uuid, &error);
    REQUIRE(preview);
    REQUIRE(preview->conflict_count == 4);
    REQUIRE(preview->changes.size() == scenarios.size());
    const ObjectMergeChange *added = findChange(*preview, scenarios[0].uuid);
    REQUIRE(added && added->kind == ObjectMergeKind::FieldsMerged && added->automatically_merged);
    const std::optional<HydraulicNodeJunction> auto_merged = ModelCodec<HydraulicNodeJunction>::decode(
        added->automatically_merged->content, &error);
    REQUIRE(auto_merged);
    const QStringList expected_tags = {QStringLiteral("common"), QStringLiteral("left-1"),
        QStringLiteral("left-2"), QStringLiteral("right-1"), QStringLiteral("right-2")};
    REQUIRE(auto_merged->metadata.tags == expected_tags);
    REQUIRE(auto_merged->metadata.description == QStringLiteral("local description"));
    REQUIRE(auto_merged->metadata.comment == QStringLiteral("remote comment"));
    const ObjectMergeChange *same = findChange(*preview, scenarios[1].uuid);
    REQUIRE(same && same->kind == ObjectMergeKind::BothSame);
    for (qsizetype i = 2; i < scenarios.size(); ++i)
    {
        const ObjectMergeChange *change = findChange(*preview, scenarios[i].uuid);
        REQUIRE(change && change->kind == ObjectMergeKind::Conflict);
        const QStringList expected_conflicts = {QStringLiteral("metadata.tags")};
        REQUIRE(change->conflicting_fields == expected_conflicts);
    }

    const QVector<ObjectMergeResolution> resolutions = {
        tagsChoice(scenarios[2].uuid, ObjectMergeSide::Left),
        tagsChoice(scenarios[3].uuid, ObjectMergeSide::Right),
        tagsChoice(scenarios[4].uuid, ObjectMergeSide::Left),
        tagsChoice(scenarios[5].uuid, ObjectMergeSide::Right)
    };
    REQUIRE(!db.mergeObjectRevisions(actor, left_branch->uuid, working, 0,
        right_final->finalized_revision_uuid, {resolutions.first()},
        QStringLiteral("missing choices"), {}, &error));

    // Reject late parent insertion: automatic tag synthesis, journal writes,
    // and finalized revision metadata must roll back completely.
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("tags-merge-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_tags_merge BEFORE INSERT ON revision_parents "
            "WHEN NEW.parent_position=1 BEGIN SELECT RAISE(ABORT,'rollback'); END")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("tags-merge-fixture"));
    REQUIRE(!db.mergeObjectRevisions(actor, left_branch->uuid, working, 0,
        right_final->finalized_revision_uuid, resolutions, QStringLiteral("rollback"), {}, &error));
    const std::optional<aowis::db::RevisionInfo> after_failure = db.revision(actor, working, &error);
    REQUIRE(after_failure && !after_failure->finalized && after_failure->generation == 0);
    REQUIRE(db.objectGroupsForRevision(actor, working, &error).isEmpty());
    REQUIRE(db.object(actor, working, scenarios[0].uuid, &error)->content ==
        ModelCodec<HydraulicNodeJunction>::encode(scenarios[0].left));
    {
        QSqlDatabase connection = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("tags-merge-fixture"));
        connection.setDatabaseName(path);
        REQUIRE(connection.open());
        QSqlQuery query(connection);
        REQUIRE(query.exec(QStringLiteral("DROP TRIGGER reject_tags_merge")));
        connection.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("tags-merge-fixture"));

    const std::optional<aowis::db::Finalization> merged = db.mergeObjectRevisions(
        actor, left_branch->uuid, working, 0, right_final->finalized_revision_uuid,
        resolutions, QStringLiteral("append-only tag merge"), {}, &error);
    REQUIRE(merged && merged->finalized_revision_uuid == working);
    const std::optional<HydraulicNodeJunction> committed = ModelCodec<HydraulicNodeJunction>::decode(
        db.object(actor, working, scenarios[0].uuid, &error)->content, &error);
    REQUIRE(committed && committed->metadata.tags == expected_tags);
    REQUIRE(committed->metadata.description == QStringLiteral("local description"));
    REQUIRE(committed->metadata.comment == QStringLiteral("remote comment"));
    for (qsizetype i = 2; i < scenarios.size(); ++i)
    {
        const bool chose_right = i == 3 || i == 5;
        const HydraulicNodeJunction &expected = chose_right ? scenarios[i].right : scenarios[i].left;
        REQUIRE(db.object(actor, working, scenarios[i].uuid, &error)->content ==
            ModelCodec<HydraulicNodeJunction>::encode(expected));
    }
    REQUIRE(db.revision(actor, working, &error)->parent_revision_uuids.size() == 2);
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.verifyProject(actor, project->project_uuid).valid);
    return 0;
}
