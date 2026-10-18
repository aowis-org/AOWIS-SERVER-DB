#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>

using aowis::db::Database;
using aowis::db::OperationContext;

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s FAILED (%s)\n", __FILE__, __LINE__, #condition, \
                error.toUtf8().constData()); \
        return 1; \
    } \
} while (false)

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QString error;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("history.sqlite"));

    Database db;
    REQUIRE(db.openSqlite(path, &error));
    const std::optional<aowis::db::UserInfo> admin = db.bootstrapAdministrator(QStringLiteral("admin"), &error);
    REQUIRE(admin.has_value());
    const OperationContext root{admin->uuid};
    const std::optional<aowis::db::UserInfo> outsider = db.createUser(root, QStringLiteral("outsider"), &error);
    REQUIRE(outsider.has_value());
    const OperationContext unauthorized{outsider->uuid};
    const std::optional<aowis::db::ProjectCreation> p = db.createProject(root, QStringLiteral("History network"), &error);
    REQUIRE(p.has_value());
    const QUuid branch_uuid = p->main_branch_uuid;
    const QUuid rev1 = p->working_revision_uuid;
    REQUIRE(db.history(root, branch_uuid, &error).isEmpty());
    REQUIRE(!db.undoLastChange(root, branch_uuid, 0, &error));
    REQUIRE(!db.replaceWorkingSnapshot(unauthorized, branch_uuid, 0, QByteArray("A"), &error));
    REQUIRE(db.replaceWorkingSnapshot(root, branch_uuid, 0, QByteArray("A"), &error));
    REQUIRE(db.replaceWorkingSnapshot(root, branch_uuid, 1, QByteArray("B"), &error));
    REQUIRE(!db.replaceWorkingSnapshot(root, branch_uuid, 2, QByteArray("B"), &error));
    const QVector<aowis::db::HistoryEvent> initial_events = db.history(root, branch_uuid, &error);
    REQUIRE(initial_events.size() == 2);
    REQUIRE(initial_events.at(0).actor_uuid == root.actor_uuid);
    REQUIRE(initial_events.at(0).target_event_uuid.isNull());
    REQUIRE(initial_events.at(1).actor_uuid == root.actor_uuid);
    REQUIRE(initial_events.at(1).target_event_uuid.isNull());
    REQUIRE(db.historyForRevision(root, rev1, &error).size() == 2);
    REQUIRE(db.historyForRevision(unauthorized, rev1, &error).isEmpty());
    REQUIRE(!db.undoLastChange(root, branch_uuid, 1, &error));
    REQUIRE(db.undoLastChange(root, branch_uuid, 2, &error).has_value());
    const QVector<aowis::db::HistoryEvent> after_undo = db.history(root, branch_uuid, &error);
    REQUIRE(after_undo.size() == 3);
    REQUIRE(after_undo.at(2).actor_uuid == root.actor_uuid);
    REQUIRE(after_undo.at(2).target_event_uuid == initial_events.at(1).uuid);
    REQUIRE(db.snapshot(root, rev1, &error).value() == QByteArray("A"));
    REQUIRE(db.undoLastChange(root, branch_uuid, 3, &error).has_value());
    REQUIRE(!db.snapshot(root, rev1, &error).has_value());
    REQUIRE(!db.undoLastChange(root, branch_uuid, 4, &error));
    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.history(root, branch_uuid, &error).size() == 4);
    REQUIRE(db.redoLastChange(root, branch_uuid, 4, &error).has_value());
    REQUIRE(db.snapshot(root, rev1, &error).value() == QByteArray("A"));
    REQUIRE(db.redoLastChange(root, branch_uuid, 5, &error).has_value());
    REQUIRE(db.snapshot(root, rev1, &error).value() == QByteArray("B"));
    REQUIRE(!db.redoLastChange(root, branch_uuid, 6, &error));
    REQUIRE(db.undoLastChange(root, branch_uuid, 6, &error).has_value());
    REQUIRE(db.replaceWorkingSnapshot(root, branch_uuid, 7, QByteArray("C"), &error));
    REQUIRE(!db.redoLastChange(root, branch_uuid, 8, &error));
    REQUIRE(db.history(root, branch_uuid, &error).size() == 8);

    const std::optional<aowis::db::Finalization> result = db.finalize(
        root, branch_uuid, 8, QStringLiteral("Baseline"), QStringLiteral("history sealed"), &error);
    REQUIRE(result.has_value());
    REQUIRE(db.verifyProject(root, p->project_uuid).valid);
    REQUIRE(db.historyForRevision(root, rev1, &error).size() == 8);
    REQUIRE(db.history(root, branch_uuid, &error).isEmpty());
    REQUIRE(db.snapshot(root, result->new_working_revision_uuid, &error).value() == QByteArray("C"));
    REQUIRE(!db.undoLastChange(root, branch_uuid, 0, &error));

    db.close();
    REQUIRE(db.openSqlite(path, &error));
    REQUIRE(db.historyForRevision(root, rev1, &error).size() == 8);
    REQUIRE(db.verifyProject(root, p->project_uuid).valid);

    // Direct SQL cannot mutate sealed history or append events to old revisions.
    {
        QSqlDatabase inspector = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("history-inspect"));
        inspector.setDatabaseName(path);
        REQUIRE(inspector.open());
        QSqlQuery q(inspector);
        REQUIRE(q.prepare(QStringLiteral("DELETE FROM operation_events WHERE revision_uuid=?")));
        q.addBindValue(rev1.toRfc4122());
        REQUIRE(!q.exec());
        REQUIRE(q.exec(QStringLiteral("DROP TRIGGER operation_events_update_guard")));
        REQUIRE(q.prepare(QStringLiteral("UPDATE operation_events SET occurred_at_ms=1234 WHERE revision_uuid=?")));
        q.addBindValue(rev1.toRfc4122());
        REQUIRE(q.exec());
        q.finish();
        inspector.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("history-inspect"));
    REQUIRE(!db.verifyProject(root, p->project_uuid).valid);
    return 0;
}
