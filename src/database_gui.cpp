#include "database_gui.h"

DatabaseGui::DatabaseGui(QObject *parent)
    : QObject{parent},
    database_sqlite(new DatabaseSqlite(this))
{
    connect(this->database_sqlite, &DatabaseSqlite::signalOpened, this, &DatabaseGui::databaseOpened);
    connect(this->database_sqlite, &DatabaseSqlite::signalError, this, [this](const QString &message)
    {
        QSqlQuery query(this->database_sqlite->database());
        qCritical() << query.lastError().text();
    });
    
    DatabaseConfiguration configuration;
    database_sqlite->open(configuration);
}

void DatabaseGui::databaseOpened()
{
    QSqlQuery query(this->database_sqlite->database());
    
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS test ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL DEFAULT 'test'"
            ")"))
    )
    {
        qCritical() << query.lastError().text();
        return;
    }
    
    if (!query.exec(QStringLiteral("INSERT INTO test DEFAULT VALUES")))
        qCritical() << query.lastError().text();
    
}

QString DatabaseGui::getTestName() const
{
    QSqlQuery query(this->database_sqlite->database());
    
    if (!query.exec(QStringLiteral("SELECT name FROM test LIMIT 1")))
    {
        qCritical() << "DATABASE ERROR:" << query.lastError().text();
        return {};
    }
    
    if (!query.next())
        return {};
    
    return query.value(QStringLiteral("name")).toString();
}
