#include "database_sqlite.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

#ifdef Q_OS_WASM
#include <emscripten.h>

EM_JS(void, initializePersistentStorage, (void *databaseGui), {
    if (!Module.aowisIdbfsMounted) {
        FS.mkdirTree("/aowis");
        FS.mount(IDBFS, { autoPersist: true }, "/aowis");
        Module.aowisIdbfsMounted = true;
    }
    
    FS.syncfs(true, function(error) {
        Module._aowisDatabaseStorageReady(databaseGui, error ? 0 : 1);
    });
});
#endif

DatabaseSqlite::DatabaseSqlite(QObject *parent)
    : QObject(parent)
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
    initializePersistentStorage(this);
#else
    openSqlite();
#endif
}

void DatabaseSqlite::close()
{
    if (!this->sql_database.isValid())
        return;
    
    const QString connectionName = this->sql_database.connectionName();
    
    this->sql_database.close();
    this->sql_database = QSqlDatabase();
    
    QSqlDatabase::removeDatabase(connectionName);
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
        path = QStringLiteral("/aowis/aowis.sqlite");
#else
    if (path.isEmpty())
    {
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        
        if (!QDir().mkpath(directory))
        {
            emit signalError(QStringLiteral("Could not create database directory"));
            return;
        }
        
        path = directory + QStringLiteral("/aowis.sqlite");
    }
#endif
    
    this->sql_database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"),
        QStringLiteral("aowis-gui")
        );
    
    this->sql_database.setDatabaseName(path);
    
    if (!this->sql_database.open())
    {
        emit signalError(this->sql_database.lastError().text());
        return;
    }
    
    QSqlQuery query(this->sql_database);
    query.exec(QStringLiteral("PRAGMA journal_mode = DELETE"));
    query.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    
    emit signalOpened();
}

#ifdef Q_OS_WASM
extern "C" EMSCRIPTEN_KEEPALIVE void aowisDatabaseStorageReady(void *databaseGui, int success)
{
    DatabaseGui *database = static_cast<DatabaseGui *>(databaseGui);
    
    if (success == 0)
    {
        emit database->signalError(QStringLiteral("Could not load persistent browser storage"));
        return;
    }
    
    database->openSqlite();
}
#endif
