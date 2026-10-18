#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include <QTcpServer>
#include <QTemporaryDir>

#define REQUIRE(condition) do { \
    if (!(condition)) { qCritical().noquote() << "FAILED" << __FILE__ << __LINE__ << #condition; return 1; } \
} while (false)

struct ServerResult
{
    bool started = false;
    bool listening = false;
    QByteArray diagnostics;
};

static ServerResult tryServer(const QString &executable, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();

    ServerResult result;
    result.started = process.waitForStarted(5000);
    if (result.started)
    {
        QByteArray output;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 8000 && process.state() != QProcess::NotRunning)
        {
            process.waitForReadyRead(500);
            output += process.readAllStandardOutput();
            if (output.contains("listening on 127.0.0.1:"))
            {
                result.listening = true;
                break;
            }
        }
        output += process.readAllStandardOutput();
        result.diagnostics = output + process.readAllStandardError();
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(5000);
        }
        result.diagnostics += process.readAllStandardError();
    }
    else
        result.diagnostics = process.errorString().toUtf8();
    if (!result.listening)
        qCritical().noquote() << "Server failed to start:" << result.diagnostics;
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    REQUIRE(app.arguments().size() == 2);
    const QString executable = app.arguments().at(1);
    REQUIRE(QFileInfo::exists(executable));

    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QString ini_file = temporary.filePath(QStringLiteral("server.ini"));
    const QString configured_db = temporary.filePath(QStringLiteral("configured.sqlite"));
    const QString override_db = temporary.filePath(QStringLiteral("overridden.sqlite"));
    const QString log_file = temporary.filePath(QStringLiteral("log.txt"));

    QTcpServer port_probe;
    REQUIRE(port_probe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = port_probe.serverPort();
    port_probe.close();

    {
        QSettings settings(ini_file, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("sqlite"));
        settings.setValue(QStringLiteral("Database/File"), configured_db);
        settings.setValue(QStringLiteral("Logging/File"), log_file);
        settings.setValue(QStringLiteral("HTTP/Port"), port);
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }

    const QStringList configuration{QStringLiteral("--config"), ini_file};
    const ServerResult default_start = tryServer(executable, configuration);
    REQUIRE(default_start.started && default_start.listening);
    REQUIRE(QFileInfo::exists(configured_db));
    REQUIRE(!QFileInfo::exists(override_db));

    const ServerResult override_start = tryServer(executable,
        configuration + QStringList{override_db});
    REQUIRE(override_start.started && override_start.listening);
    REQUIRE(QFileInfo::exists(override_db));

    // A CLI override must not modify the INI file itself.
    QSettings loaded(ini_file, QSettings::IniFormat);
    REQUIRE(loaded.value(QStringLiteral("Database/File")).toString() == configured_db);

    const QString pg_ini = temporary.filePath(QStringLiteral("pg.ini"));
    {
        QSettings settings(pg_ini, QSettings::IniFormat);
        settings.setValue(QStringLiteral("Database/Backend"), QStringLiteral("postgres"));
        settings.setValue(QStringLiteral("Database/Name"), QStringLiteral("unused"));
        settings.setValue(QStringLiteral("Database/Schema"), QStringLiteral("unused"));
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }
    QProcess invalid;
    invalid.start(executable, QStringList{QStringLiteral("--config"), pg_ini, override_db});
    REQUIRE(invalid.waitForStarted(5000));
    REQUIRE(invalid.waitForFinished(5000));
    REQUIRE(invalid.exitCode() == 2);
    REQUIRE(invalid.readAllStandardError().contains("Backend=postgres"));
    return 0;
}
