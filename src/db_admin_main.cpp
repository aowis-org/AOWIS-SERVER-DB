#include <aowis/db/configuration.h>
#include <aowis/db/database.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("AOWIS"));
    app.setApplicationName(QStringLiteral("AOWIS-SERVER-DB"));
    QTextStream output(stdout);
    QTextStream errors(stderr);
    // Parse --config separately, leaving command operands independent of the
    // chosen database backend. The INI is the sole source of DB parameters.
    const QStringList supplied_arguments = app.arguments();
    QStringList args{ supplied_arguments.first() };
    QString ini_file;
    for (int index = 1; index < supplied_arguments.size(); ++index)
    {
        const QString argument = supplied_arguments.at(index);
        if (argument == QStringLiteral("--config"))
        {
            if (!ini_file.isEmpty() || index + 1 == supplied_arguments.size() ||
                supplied_arguments.at(index + 1).isEmpty() ||
                supplied_arguments.at(index + 1).startsWith(QStringLiteral("--")))
            {
                errors << "Expected one absolute INI file after --config.\n";
                return 2;
            }
            ini_file = supplied_arguments.at(++index);
        }
        else if (argument.startsWith(QStringLiteral("--")))
        {
            errors << "Unknown option: " << argument << "\n";
            return 2;
        }
        else
            args.append(argument);
    }
    const QString usage = QStringLiteral(
        "Usage: aowis-db-admin [--config ABSOLUTE_INI_FILE] COMMAND [ARGUMENTS...]\n"
        "  aowis-db-admin bootstrap USERNAME\n"
        "  aowis-db-admin issue ADMIN_UUID USER_UUID [LIFETIME_SECONDS]\n"
        "  aowis-db-admin set-password ADMIN_UUID USER_UUID < PASSWORD_STDIN\n"
        "  aowis-db-admin recover-password ADMIN_UUID USER_UUID [LIFETIME_SECONDS]\n"
        "  aowis-db-admin revoke ADMIN_UUID SESSION_UUID\n"
        "  aowis-db-admin disable-user ADMIN_UUID USER_UUID\n"
        "  aowis-db-admin enable-user ADMIN_UUID USER_UUID\n"
        "  aowis-db-admin remove-member ADMIN_UUID MEMBERSHIP_UUID\n"
        "  aowis-db-admin revoke-role-permission ADMIN_UUID GRANT_UUID\n"
        "  aowis-db-admin revoke-role-assignment ADMIN_UUID ASSIGNMENT_UUID\n"
        "  aowis-db-admin audit ADMIN_UUID [AFTER_SEQUENCE [LIMIT]]\n"
        "  aowis-db-admin audit-verify ADMIN_UUID\n"
        "  aowis-db-admin redact-sessions ADMIN_UUID [MIN_AGE_DAYS [LIMIT]]\n"
        "Database backend and location come from config.ini (SQLite by default).\n"
        "Capture issued tokens securely (umask 077); never publish or commit them.\n");
    if (args.size() < 3)
    {
        errors << usage;
        return 2;
    }
    const QString command = args.at(1);
    const bool bootstrap = command == QStringLiteral("bootstrap") && args.size() == 3;
    const bool issue = command == QStringLiteral("issue") && (args.size() == 4 || args.size() == 5);
    const bool set_password = command == QStringLiteral("set-password") && args.size() == 4;
    const bool recovery = command == QStringLiteral("recover-password") && (args.size() == 4 || args.size() == 5);
    const bool revoke = command == QStringLiteral("revoke") && args.size() == 4;
    const bool disable = command == QStringLiteral("disable-user") && args.size() == 4;
    const bool enable = command == QStringLiteral("enable-user") && args.size() == 4;
    const bool remove_member = command == QStringLiteral("remove-member") && args.size() == 4;
    const bool revoke_permission = command == QStringLiteral("revoke-role-permission") && args.size() == 4;
    const bool revoke_assignment = command == QStringLiteral("revoke-role-assignment") && args.size() == 4;
    const bool audit = command == QStringLiteral("audit") &&
                       (args.size() == 3 || args.size() == 4 || args.size() == 5);
    const bool audit_verify = command == QStringLiteral("audit-verify") && args.size() == 3;
    const bool redact_sessions = command == QStringLiteral("redact-sessions") &&
        (args.size() == 3 || args.size() == 4 || args.size() == 5);
    if (!bootstrap && !issue && !set_password && !recovery && !revoke && !disable && !enable && !remove_member &&
        !revoke_permission && !revoke_assignment && !audit && !audit_verify && !redact_sessions)
    {
        errors << usage;
        return 2;
    }
    QString error;
    const std::optional<aowis::db::Configuration> config =
        aowis::db::Configuration::load(ini_file, &error);
    if (!config)
    {
        errors << "Invalid configuration: " << error << "\n";
        return 2;
    }
    aowis::db::Database database;
    if (config->database_backend == aowis::db::Configuration::Backend::PostgreSQL)
    {
        // libpq supplies authentication and host/port configuration.
        if (!database.openPostgres(config->postgres_database, config->postgres_schema, &error))
        {
            errors << "Unable to open PostgreSQL database: " << error << "\n";
            return 1;
        }
    }
    else
    {
        // A typo in the selected path must not create a second empty database
        // during normal administrative operations.
        if (!bootstrap && !QFileInfo::exists(config->sqlite_file))
        {
            errors << "Configured SQLite database does not exist: " << config->sqlite_file << "\n";
            return 2;
        }
        if (bootstrap && !QDir().mkpath(QFileInfo(config->sqlite_file).absolutePath()))
        {
            errors << "Unable to create configured SQLite database directory.\n";
            return 1;
        }
        if (!database.openSqlite(config->sqlite_file, &error))
        {
            errors << "Unable to open SQLite database: " << error << "\n";
            return 1;
        }
    }
    if (bootstrap)
    {
        const std::optional<aowis::db::UserInfo> user = database.bootstrapAdministrator(args.at(2), &error);
        if (!user)
        {
            errors << error << "\n";
            return 1;
        }
        output << user->uuid.toString(QUuid::WithoutBraces) << "\n";
        return 0;
    }
    const QUuid administrator(args.at(2));
    if (administrator.isNull())
    {
        errors << "Invalid administrator UUID.\n";
        return 2;
    }
    const aowis::db::OperationContext context{administrator};
    if (redact_sessions)
    {
        bool valid = true;
        qint64 days = 30;
        int limit = 100;
        if (args.size() >= 4)
            days = args.at(3).toLongLong(&valid);
        if (!valid || days < 0 || days > 3650)
        {
            errors << "Invalid minimum session age (0..3650 days).\n";
            return 2;
        }
        if (args.size() == 5)
            limit = args.at(4).toInt(&valid);
        if (!valid || limit < 1 || limit > 500)
        {
            errors << "Invalid session-redaction batch limit (1..500).\n";
            return 2;
        }
        const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - days * 86400000LL;
        const std::optional<int> count = database.redactSpentSessionTokens(context, cutoff, limit, &error);
        if (!count)
        {
            errors << error << "\n";
            return 1;
        }
        output << "Redacted credentials for " << *count << " spent sessions\n";
        return 0;
    }
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
        if (args.size() >= 4)
            after = args.at(3).toLongLong(&valid);
        if (!valid || after < 0)
        {
            errors << "Invalid audit sequence.\n";
            return 2;
        }
        if (args.size() == 5)
            limit = args.at(4).toInt(&valid);
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
    const QUuid target(args.at(3));
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
        if (args.size() == 5)
        {
            bool valid = false;
            lifetime_seconds = args.at(4).toLongLong(&valid);
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
        if (args.size() == 5)
        {
            bool ok = false;
            lifetime_seconds = args.at(4).toLongLong(&ok);
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
