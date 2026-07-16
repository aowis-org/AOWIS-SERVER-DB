#include "database_sqlite.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

#ifdef Q_OS_WASM
#include <emscripten.h>

EM_JS(void, initializeSessionStorage, (void *databaseSqlite), {
    try {
        if (!Module.aowisIdbfsMountPath) {
            const navigationEntries = performance.getEntriesByType("navigation");
            const navigationEntry = navigationEntries.length > 0 ? navigationEntries[0] : null;
            const isReload = navigationEntry && navigationEntry.type === "reload";
            
            let sessionId = isReload
                                ? sessionStorage.getItem("aowis-database-session-id")
                                : null;
            
            if (!sessionId) {
                const bytes = new Uint8Array(16);
                crypto.getRandomValues(bytes);
                
                sessionId = Array.from(
                                     bytes,
                                     value => value.toString(16).padStart(2, "0")
                                     ).join("");
                
                sessionStorage.setItem("aowis-database-session-id", sessionId);
            }
            
            const mountPath = "/aowis/" + sessionId;
            
            FS.mkdirTree(mountPath);
            FS.mount(IDBFS, { autoPersist: true }, mountPath);
            
            Module.aowisIdbfsMountPath = mountPath;
        }
        
        const databasePath = Module.aowisIdbfsMountPath + "/aowis.sqlite";
        
        FS.syncfs(true, function(error) {
            if (error) {
                console.error("Could not load AOWIS browser session storage:", error);
                Module._aowisDatabaseStorageReady(databaseSqlite, 0, 0);
                return;
            }
            
            const pathLength = lengthBytesUTF8(databasePath) + 1;
            const pathPointer = _malloc(pathLength);
            
            stringToUTF8(databasePath, pathPointer, pathLength);
            Module._aowisDatabaseStorageReady(databaseSqlite, 1, pathPointer);
            _free(pathPointer);
        });
    } catch (error) {
        console.error("Could not initialize AOWIS browser session storage:", error);
        Module._aowisDatabaseStorageReady(databaseSqlite, 0, 0);
    }
});
#endif

DatabaseSqlite::DatabaseSqlite(QObject *parent)
    : QObject(parent),
    connection_name(
        QStringLiteral("aowis-sqlite-%1")
            .arg(QString::number(reinterpret_cast<quintptr>(this), 16))
        )
{
}

DatabaseSqlite::~DatabaseSqlite()
{
    close();
}

void DatabaseSqlite::open(const DatabaseConfiguration &configuration)
{
    close();
    
    this->configuration = configuration;
    
    if (this->configuration.backend != DatabaseBackend::SQLite)
    {
        emit signalError(QStringLiteral("Only SQLite is implemented"));
        return;
    }

#ifdef Q_OS_WASM
    initializeSessionStorage(this);
#else
    openSqlite();
#endif
}

void DatabaseSqlite::close()
{
    if (!this->sql_database.isValid())
        return;
    
    this->sql_database.close();
    this->sql_database = QSqlDatabase();
    
    QSqlDatabase::removeDatabase(this->connection_name);
}

QSqlDatabase DatabaseSqlite::database() const
{
    return this->sql_database;
}

void DatabaseSqlite::openSqlite()
{
    QString path = this->configuration.sqlite_database_path;

#ifdef Q_OS_WASM
    if (path.isEmpty())
        path = this->wasm_database_path;
#else
    if (path.isEmpty())
    {
        path =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/aowis.sqlite");
    }
    
    const QFileInfo databaseFile(path);
    
    if (!QDir().mkpath(databaseFile.absolutePath()))
    {
        emit signalError(QStringLiteral("Could not create database directory"));
        return;
    }
#endif
    
    if (path.isEmpty())
    {
        emit signalError(QStringLiteral("SQLite database path is empty"));
        return;
    }
    
    this->sql_database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"),
        this->connection_name
        );
    
    this->sql_database.setDatabaseName(path);
    this->sql_database.setConnectOptions(
        QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000")
        );
    
    if (!this->sql_database.open())
    {
        emit signalError(this->sql_database.lastError().text());
        close();
        return;
    }
    
    if (!executePragma(QStringLiteral("PRAGMA foreign_keys = ON")))
    {
        close();
        return;
    }

#ifdef Q_OS_WASM
    if (!executePragma(QStringLiteral("PRAGMA journal_mode = DELETE")))
#else
    if (!executePragma(QStringLiteral("PRAGMA journal_mode = WAL")))
#endif
    {
        close();
        return;
    }
    
    emit signalOpened();
}

bool DatabaseSqlite::executePragma(const QString &statement)
{
    QSqlQuery query(this->sql_database);
    
    if (query.exec(statement))
        return true;
    
    emit signalError(
        QStringLiteral("%1: %2")
            .arg(statement, query.lastError().text())
        );
    
    return false;
}

#ifdef Q_OS_WASM
extern "C" EMSCRIPTEN_KEEPALIVE void aowisDatabaseStorageReady(
    void *databaseSqlite,
    int success,
    const char *databasePath
    )
{
    DatabaseSqlite *database =
        static_cast<DatabaseSqlite *>(databaseSqlite);
    
    if (success == 0 || databasePath == nullptr)
    {
        emit database->signalError(
            QStringLiteral("Could not load browser session storage")
            );
        return;
    }
    
    database->wasm_database_path = QString::fromUtf8(databasePath);
    database->openSqlite();
}
#endif
