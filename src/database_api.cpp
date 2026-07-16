#include "database_api.h"

DatabaseApi::DatabaseApi(QObject *parent)
    : QObject{parent}
{
    
}

void DatabaseApi::databaseOpened()
{
    this->database = this->database_postgresql->database();
    this->database_shared = new DatabaseShared(this->database, this);
    
    if (!this->database_shared->initialize()) {
        qCritical() << "DATABASE ERROR: Failed to initialize shared database schema";
        return;
    }
    
    emit signalReady();
}

DatabaseShared *DatabaseGui::sharedDatabase() const
{
    return this->database_shared;
}
