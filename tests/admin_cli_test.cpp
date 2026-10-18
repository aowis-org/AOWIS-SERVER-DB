#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition << error; return 1; } \
} while (false)

struct CommandResult
{
    bool started = false;
    bool finished = false;
    int exit_code = -1;
    QByteArray output;
    QByteArray diagnostics;
};

static CommandResult callAdmin(const QString &executable, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    CommandResult result;
    result.started = process.waitForStarted(5000);
    if (!result.started)
    {
        result.diagnostics = process.errorString().toUtf8();
        return result;
    }
    result.finished = process.waitForFinished(30000);
    if (!result.finished)
    {
        process.kill();
        process.waitForFinished();
    }
    result.exit_code = process.exitCode();
    result.output = process.readAllStandardOutput();
    result.diagnostics = process.readAllStandardError();
    return result;
}

static bool succeeded(const CommandResult &result)
{
    if (!result.started || !result.finished || result.exit_code != 0)
        qCritical().noquote() << "Operator CLI failure:" << result.exit_code << result.diagnostics;
    return result.started && result.finished && result.exit_code == 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("AOWIS"));
    app.setApplicationName(QStringLiteral("AOWIS-SERVER-DB"));
    QString error;
    REQUIRE(app.arguments().size() == 2);
    const QString executable = app.arguments().at(1);
    REQUIRE(QFileInfo::exists(executable));
    QTemporaryDir temp;
    REQUIRE(temp.isValid());

    const QString ini = temp.filePath(QStringLiteral("operator.ini"));
    const QString db_file = temp.filePath(QStringLiteral("nonexistent/subdir/projects.sqlite"));
    {
        QSettings settings(ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("sqlite"));
        settings.setValue(QStringLiteral("Database/File"), db_file);
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    const QStringList option{QStringLiteral("--config"), ini};
    const CommandResult bootstrap = callAdmin(executable,
        option + QStringList{QStringLiteral("bootstrap"), QStringLiteral("first-admin")});
    REQUIRE(succeeded(bootstrap));
    const QUuid administrator(QString::fromUtf8(bootstrap.output.trimmed()));
    REQUIRE(!administrator.isNull());
    REQUIRE(QFileInfo::exists(db_file));

    const QString id = administrator.toString(QUuid::WithoutBraces);
    const CommandResult verify = callAdmin(executable,
        QStringList{QStringLiteral("audit-verify"), id} + option);
    REQUIRE(succeeded(verify));
    REQUIRE(verify.output.contains("Valid security audit chain"));
    const CommandResult audit = callAdmin(executable,
        option + QStringList{QStringLiteral("audit"), id});
    REQUIRE(succeeded(audit));

    const CommandResult invalid = callAdmin(executable,
        option + QStringList{QStringLiteral("audit-verify"), db_file, id});
    REQUIRE(invalid.started && invalid.finished && invalid.exit_code == 2);
    REQUIRE(invalid.diagnostics.contains("Usage:"));

    const CommandResult bad_ini = callAdmin(executable,
        QStringList{QStringLiteral("--config"), QStringLiteral("relative.ini"),
                    QStringLiteral("bootstrap"), QStringLiteral("irrelevant")});
    REQUIRE(bad_ini.started && bad_ini.finished && bad_ini.exit_code == 2);
    REQUIRE(bad_ini.diagnostics.contains("absolute"));

    const QString other_ini = temp.filePath(QStringLiteral("missing.ini"));
    const QString missing_file = temp.filePath(QStringLiteral("missing.sqlite"));
    {
        QSettings settings(other_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/File"), missing_file);
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    const CommandResult missing = callAdmin(executable,
        QStringList{QStringLiteral("--config"), other_ini,
                    QStringLiteral("audit-verify"), id});
    REQUIRE(missing.started && missing.finished && missing.exit_code == 2);
    REQUIRE(!QFileInfo::exists(missing_file));

    // The same operator executable must work with a PostgreSQL config when
    // the Docker-based live backend-contract suite is enabled.
    const QString pg_db = qEnvironmentVariable("AOWIS_DB_TEST_PG_DB");
    if (!pg_db.isEmpty())
    {
        const QString schema = QStringLiteral("aowis_cli_") +
            QUuid::createUuid().toString(QUuid::Id128).toLower();
        const QString connection_name = QStringLiteral("aowis-cli-test-pg");
        {
            QSqlDatabase connection = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connection_name);
            connection.setDatabaseName(pg_db);
            REQUIRE(connection.open());
            QSqlQuery query(connection);
            REQUIRE(query.exec(QStringLiteral("CREATE SCHEMA %1").arg(schema)));
            query.finish();
            const QString pg_ini = temp.filePath(QStringLiteral("postgres.ini"));
            {
                QSettings settings(pg_ini, QSettings::IniFormat);
                settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgres"));
                settings.setValue(QStringLiteral("Database/Name"), pg_db);
                settings.setValue(QStringLiteral("Database/Schema"), schema);
                settings.sync();
                REQUIRE(settings.status() == QSettings::NoError);
            }
            const QStringList pg_option{QStringLiteral("--config"), pg_ini};
            const CommandResult pg_bootstrap = callAdmin(executable,
                pg_option + QStringList{QStringLiteral("bootstrap"), QStringLiteral("pg-admin")});
            // Cleanup even when one of the subprocess assertions fails.
            const bool boot_ok = succeeded(pg_bootstrap);
            const QUuid pg_user(QString::fromUtf8(pg_bootstrap.output.trimmed()));
            const CommandResult pg_verify = boot_ok && !pg_user.isNull() ?
                callAdmin(executable, pg_option + QStringList{
                    QStringLiteral("audit-verify"), pg_user.toString(QUuid::WithoutBraces)}) : CommandResult{};
            const bool verify_ok = succeeded(pg_verify);
            REQUIRE(query.exec(QStringLiteral("DROP SCHEMA %1 CASCADE").arg(schema)));
            query.finish();
            connection.close();
            REQUIRE(boot_ok && !pg_user.isNull() && verify_ok);
        }
        QSqlDatabase::removeDatabase(connection_name);
    }
    else if (qEnvironmentVariableIsSet("AOWIS_DB_TEST_PG_REQUIRED"))
    {
        qCritical() << "PostgreSQL is required but AOWIS_DB_TEST_PG_DB is not configured.";
        return 1;
    }
    return 0;
}
