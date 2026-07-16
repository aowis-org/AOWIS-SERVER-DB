#include "database_gui.h"

#include <QDebug>

DatabaseGui::DatabaseGui(QObject *parent)
    : QObject{parent},
    database_sqlite(new DatabaseSqlite(this))
{
    connect(
        this->database_sqlite,
        &DatabaseSqlite::signalOpened,
        this,
        &DatabaseGui::databaseOpened
        );
    
    connect(
        this->database_sqlite,
        &DatabaseSqlite::signalError,
        this,
        [this](const QString &message)
        {
            qCritical() << "DATABASE ERROR:" << message;
            emit signalError(message);
        }
        );
}

void DatabaseGui::open(const DatabaseConfiguration &configuration)
{
    this->database_sqlite->open(configuration);
}

void DatabaseGui::databaseOpened()
{
    this->database = this->database_sqlite->database();
    
    if (this->database_shared != nullptr)
        this->database_shared->deleteLater();
    
    this->database_shared = new DatabaseShared(this->database, this);
    
    if (!this->database_shared->initialize())
    {
        const QString message =
            QStringLiteral("Failed to initialize shared database schema");
        
        qCritical() << "DATABASE ERROR:" << message;
        emit signalError(message);
        return;
    }
    
    emit signalReady();
}

DatabaseShared *DatabaseGui::sharedDatabase() const
{
    return this->database_shared;
}
