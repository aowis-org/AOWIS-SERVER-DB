#ifndef DATABASE_GUI_H
#define DATABASE_GUI_H

#include <QObject>

#include <QSqlQuery>
#include <QSqlError>

#include <QDebug>

#include "database_sqlite.h"

class DatabaseGui : public QObject
{
    Q_OBJECT
public:
    explicit DatabaseGui(QObject *parent = nullptr);
    
    QString getTestName() const;
    
private:
    DatabaseSqlite *database_sqlite = nullptr;
    
private slots:
    void databaseOpened();
    
signals:
};

#endif // DATABASE_GUI_H
