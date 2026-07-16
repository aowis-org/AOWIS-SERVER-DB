#ifndef DATABASE_SQLITE_H
#define DATABASE_SQLITE_H

#include "models/database_configuration.h"

#include <QObject>
#include <QSqlDatabase>
#include <QString>

#ifdef Q_OS_WASM
extern "C" void aowisDatabaseStorageReady(
    void *databaseSqlite,
    int success,
    const char *databasePath
    );
#endif

class DatabaseSqlite final : public QObject
{
    Q_OBJECT
    
public:
    explicit DatabaseSqlite(QObject *parent = nullptr);
    ~DatabaseSqlite() override;
    
    void open(const DatabaseConfiguration &configuration);
    void close();
    
    QSqlDatabase database() const;
    
signals:
    void signalOpened();
    void signalError(const QString &message);
    
private:
    void openSqlite();
    bool executePragma(const QString &statement);

#ifdef Q_OS_WASM
    friend void aowisDatabaseStorageReady(
        void *databaseSqlite,
        int success,
        const char *databasePath
        );
#endif
    
    DatabaseConfiguration configuration;
    QSqlDatabase sql_database;
    QString connection_name;

#ifdef Q_OS_WASM
    QString wasm_database_path;
#endif
};

#endif // DATABASE_SQLITE_H
