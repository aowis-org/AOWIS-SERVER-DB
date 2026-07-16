#include "database_gui.h"

DatabaseGui::DatabaseGui(QObject *parent)
    : QObject{parent},
    database_sqlite(new DatabaseSqlite(this))
{
    connect(this->database_sqlite, &DatabaseSqlite::signalOpened, this, &DatabaseGui::databaseOpened);
    connect(this->database_sqlite, &DatabaseSqlite::signalError, this, [](const QString &message)
    {
        qCritical() << "DATABASE ERROR:" << message;
    });
    
    DatabaseConfiguration configuration;
    this->database_sqlite->open(configuration);
}

void DatabaseGui::databaseOpened()
{
    this->database = this->database_sqlite->database();
    this->database_shared = new DatabaseShared(this->database, this);
    
    QSqlQuery query(this->database);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS test ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL DEFAULT 'test'"
            ")")))
    {
        qCritical() << "DATABASE ERROR:" << query.lastError().text();
        return;
    }
    
    if (!query.exec(QStringLiteral("INSERT INTO test DEFAULT VALUES")))
    {
        qCritical() << "DATABASE ERROR:" << query.lastError().text();
        return;
    }
    
    this->database_shared = new DatabaseShared(this->database, this);
    
    emit signalReady();
}

QString DatabaseGui::getTestName() const
{
    QSqlQuery query(this->database);
    
    if (!query.exec(QStringLiteral("SELECT name FROM test LIMIT 1")))
    {
        qCritical() << "DATABASE ERROR:" << query.lastError().text();
        return {};
    }
    
    if (!query.next())
        return {};
    
    return query.value(QStringLiteral("name")).toString();
}
