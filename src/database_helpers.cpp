#include "database_helpers.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
void bindValues(QSqlQuery &query, const QVariantMap &bindings)
{
    QVariantMap::const_iterator iterator = bindings.constBegin();
    
    while (iterator != bindings.constEnd())
    {
        query.bindValue(iterator.key(), iterator.value());
        ++iterator;
    }
}

void logDatabaseError(const QSqlQuery &query, const QString &statement)
{
    qCritical() << "DATABASE ERROR:" << query.lastError().text() << "STATEMENT:" << statement;
}
}

bool DatabaseHelpers::execute(const QSqlDatabase &database, const QString &statement, const QVariantMap &bindings)
{
    QSqlQuery query(database);
    
    if (!query.prepare(statement))
    {
        logDatabaseError(query, statement);
        return false;
    }
    
    bindValues(query, bindings);
    
    if (!query.exec())
    {
        logDatabaseError(query, statement);
        return false;
    }
    
    return true;
}

std::optional<QVariant> DatabaseHelpers::scalarValue(const QSqlDatabase &database, const QString &statement, const QVariantMap &bindings)
{
    QSqlQuery query(database);
    
    if (!query.prepare(statement))
    {
        logDatabaseError(query, statement);
        return std::nullopt;
    }
    
    bindValues(query, bindings);
    
    if (!query.exec())
    {
        logDatabaseError(query, statement);
        return std::nullopt;
    }
    
    if (!query.next())
        return std::nullopt;
    
    return query.value(0);
}

std::optional<QString> DatabaseHelpers::stringValue(const QSqlDatabase &database, const QString &statement, const QVariantMap &bindings)
{
    const std::optional<QVariant> value = scalarValue(database, statement, bindings);
    
    if (!value.has_value())
        return std::nullopt;
    
    return value->toString();
}
