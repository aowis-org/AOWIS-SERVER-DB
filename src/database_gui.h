#ifndef DATABASE_GUI_H
#define DATABASE_GUI_H

#include <QObject>
#include <QSqlDatabase>

#include "database_shared.h"
#include "database_sqlite.h"
#include "models/database_configuration.h"

class DatabaseGui : public QObject
{
    Q_OBJECT
    
public:
    explicit DatabaseGui(QObject *parent = nullptr);
    
    void open(const DatabaseConfiguration &configuration);
    DatabaseShared *sharedDatabase() const;
    
private:
    DatabaseSqlite *database_sqlite = nullptr;
    DatabaseShared *database_shared = nullptr;
    QSqlDatabase database;
    
private slots:
    void databaseOpened();
    
signals:
    void signalReady();
    void signalError(const QString &message);
};

#endif // DATABASE_GUI_H
