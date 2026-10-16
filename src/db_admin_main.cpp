#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream output(stdout);
    QTextStream errors(stderr);
    const QStringList args = app.arguments();
    const QString usage = QStringLiteral(
        "Usage:\n"
        "  aowis-db-admin bootstrap DB_FILE USERNAME\n"
        "  aowis-db-admin issue DB_FILE ADMIN_UUID USER_UUID [LIFETIME_SECONDS]\n"
        "  aowis-db-admin set-password DB_FILE ADMIN_UUID USER_UUID < PASSWORD_STDIN\n"
        "  aowis-db-admin recover-password DB_FILE ADMIN_UUID USER_UUID [LIFETIME_SECONDS]\n"
        "  aowis-db-admin revoke DB_FILE ADMIN_UUID SESSION_UUID\n"
        "  aowis-db-admin disable-user DB_FILE ADMIN_UUID USER_UUID\n"
        "  aowis-db-admin enable-user DB_FILE ADMIN_UUID USER_UUID\n"
        "  aowis-db-admin remove-member DB_FILE ADMIN_UUID MEMBERSHIP_UUID\n"
        "  aowis-db-admin revoke-role-permission DB_FILE ADMIN_UUID GRANT_UUID\n"
        "  aowis-db-admin revoke-role-assignment DB_FILE ADMIN_UUID ASSIGNMENT_UUID\n"
        "  aowis-db-admin audit DB_FILE ADMIN_UUID [AFTER_SEQUENCE [LIMIT]]\n"
        "  aowis-db-admin audit-verify DB_FILE ADMIN_UUID\n"
        "Local operator only: file-level SQLite access is trusted.\n"
        "Capture issued tokens securely (umask 077); never publish or commit them.\n");
    if (args.size() < 4)
    {
        errors << usage;
        return 2;
    }
    const QString command = args.at(1);
    const bool bootstrap = command == QStringLiteral("bootstrap") && args.size() == 4;
    const bool issue = command == QStringLiteral("issue") && (args.size() == 5 || args.size() == 6);
    const bool set_password = command == QStringLiteral("set-password") && args.size() == 5;
    const bool recovery = command == QStringLiteral("recover-password") && (args.size() == 5 || args.size() == 6);
    const bool revoke = command == QStringLiteral("revoke") && args.size() == 5;
    const bool disable = command == QStringLiteral("disable-user") && args.size() == 5;
    const bool enable = command == QStringLiteral("enable-user") && args.size() == 5;
    const bool remove_member = command == QStringLiteral("remove-member") && args.size() == 5;
    const bool revoke_permission = command == QStringLiteral("revoke-role-permission") && args.size() == 5;
    const bool revoke_assignment = command == QStringLiteral("revoke-role-assignment") && args.size() == 5;
    const bool audit = command == QStringLiteral("audit") &&
                       (args.size() == 4 || args.size() == 5 || args.size() == 6);
    const bool audit_verify = command == QStringLiteral("audit-verify") && args.size() == 4;
    if (!bootstrap && !issue && !set_password && !recovery && !revoke && !disable && !enable && !remove_member &&
        !revoke_permission && !revoke_assignment && !audit && !audit_verify)
    {
        errors << usage;
        return 2;
    }
    const QString file = args.at(2);
    if (!bootstrap && !QFileInfo::exists(file))
    {
        errors << "Database file does not exist: " << file << "\n";
        return 2;
    }
    aowis::db::Database database;
    QString error;
    if (!database.openSqlite(file, &error))
    {
        errors << "Unable to open database: " << error << "\n";
        return 1;
    }
    if (bootstrap)
    {
        const std::optional<aowis::db::UserInfo> user = database.bootstrapAdministrator(args.at(3), &error);
        if (!user)
        {
            errors << error << "\n";
            return 1;
        }
        output << user->uuid.toString(QUuid::WithoutBraces) << "\n";
        return 0;
    }
    const QUuid administrator(args.at(3));
    if (administrator.isNull())
    {
        errors << "Invalid administrator UUID.\n";
        return 2;
    }
    const aowis::db::OperationContext context{administrator};
    if (audit_verify)
    {
        const aowis::db::SecurityAuditReport report = database.verifySecurityAudit(context);
        if (!report.valid)
        {
            errors << "Security audit verification failed at sequence " << report.failed_sequence
                   << ": " << report.error << "\n";
            return 1;
        }
        output << "Valid security audit chain (" << report.checked_events << " events)\n";
        return 0;
    }
    if (audit)
    {
        bool valid = true;
        qint64 after = 0;
        int limit = 100;
        if (args.size() >= 5)
            after = args.at(4).toLongLong(&valid);
        if (!valid || after < 0)
        {
            errors << "Invalid audit sequence.\n";
            return 2;
        }
        if (args.size() == 6)
            limit = args.at(5).toInt(&valid);
        if (!valid || limit < 1 || limit > 1000)
        {
            errors << "Invalid audit limit (1..1000).\n";
            return 2;
        }
        const QVector<aowis::db::SecurityAuditEvent> events = database.securityAudit(context, after, limit, &error);
        if (!error.isEmpty())
        {
            errors << error << "\n";
            return 1;
        }
        for (const aowis::db::SecurityAuditEvent &event : events)
        {
            output << event.sequence << "\t" << event.occurred_at_ms << "\t"
                   << event.actor_uuid.toString(QUuid::WithoutBraces) << "\t"
                   << event.action << "\t"
                   << event.subject_uuid.toString(QUuid::WithoutBraces) << "\n";
        }
        return 0;
    }
    const QUuid target(args.at(4));
    if (target.isNull())
    {
        errors << "Invalid target UUID.\n";
        return 2;
    }
    if (set_password)
    {
        // Read from stdin rather than argv to avoid credentials in process
        // command lines and shell history. The operator controls stdin.
        QTextStream input(stdin);
        const QByteArray password = input.readLine().toUtf8();
        if (!database.setUserPassword(context, target, password, &error))
        {
            errors << error << "\n";
            return 1;
        }
        output << "OK\n";
        return 0;
    }
    if (recovery)
    {
        qint64 lifetime_seconds = 900;
        if (args.size() == 6)
        {
            bool valid = false;
            lifetime_seconds = args.at(5).toLongLong(&valid);
            if (!valid || lifetime_seconds < 1 || lifetime_seconds > 3600)
            {
                errors << "Invalid recovery lifetime (1..3600 seconds).\n";
                return 2;
            }
        }
        const std::optional<aowis::db::IssuedRecoveryToken> result =
            database.issuePasswordRecovery(context, target, lifetime_seconds, &error);
        if (!result)
        {
            errors << error << "\n";
            return 1;
        }
        output << result->token << "\n";
        output.flush();
        errors << "Recovery UUID: " << result->uuid.toString(QUuid::WithoutBraces)
               << " (expires at UTC-ms " << result->expires_at_ms << ")\n";
        return 0;
    }
    if (issue)
    {
        qint64 lifetime_seconds = 24 * 60 * 60;
        if (args.size() == 6)
        {
            bool ok = false;
            lifetime_seconds = args.at(5).toLongLong(&ok);
            if (!ok)
            {
                errors << "Invalid lifetime.\n";
                return 2;
            }
        }
        const std::optional<aowis::db::IssuedSession> result = database.issueSession(
            context, target, lifetime_seconds, &error);
        if (!result)
        {
            errors << error << "\n";
            return 1;
        }
        // Only the bearer token is sent to stdout. Session ID is emitted
        // separately for convenient revocation, not mixed into secret output.
        output << result->bearer_token << "\n";
        output.flush();
        errors << "Session UUID: " << result->uuid.toString(QUuid::WithoutBraces)
               << " (expires at UTC-ms " << result->expires_at_ms << ")\n";
        return 0;
    }
    bool ok = false;
    if (revoke)
        ok = database.revokeSession(context, target, &error);
    else if (disable)
        ok = database.disableUser(context, target, &error);
    else if (enable)
        ok = database.enableUser(context, target, &error);
    else if (remove_member)
        ok = database.removeGroupMember(context, target, &error);
    else if (revoke_permission)
        ok = database.revokeRolePermission(context, target, &error);
    else if (revoke_assignment)
        ok = database.revokeRoleAssignment(context, target, &error);
    if (!ok)
    {
        errors << error << "\n";
        return 1;
    }
    output << "OK\n";
    return 0;
}
