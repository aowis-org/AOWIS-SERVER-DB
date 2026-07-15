#ifndef DATABASE_SQLITE_H
#define DATABASE_SQLITE_H

#include "models/database_configuration.h"

#include <QObject>
#include <QSqlDatabase>
#include <QString>

#ifdef Q_OS_WASM
extern "C" void aowisDatabaseStorageReady(void *databaseGui, int success);
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

#ifdef Q_OS_WASM
    friend void aowisDatabaseStorageReady(void *databaseGui, int success);
#endif
    
    DatabaseConfiguration configuration;
    QSqlDatabase sql_database;
};

#endif // DATABASE_SQLITE_H
