#ifndef DATABASE_GUI_H
#define DATABASE_GUI_H

#include <QObject>

#include <QSqlError>
#include <QSqlQuery>

#include <QDebug>

#include "database_shared.h"
#include "database_sqlite.h"

class DatabaseGui : public QObject
{
    Q_OBJECT
    
public:
    explicit DatabaseGui(QObject *parent = nullptr);
    
    QString getTestName() const;
    
private:
    DatabaseSqlite *database_sqlite = nullptr;
    DatabaseShared *database_shared = nullptr;
    
    QSqlDatabase database;
    
private slots:
    void databaseOpened();
    
signals:
    void signalReady();
};

#endif // DATABASE_GUI_H
