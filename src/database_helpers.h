#ifndef DATABASE_HELPERS_H
#define DATABASE_HELPERS_H

#include <QSqlDatabase>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <optional>

class DatabaseHelpers final
{
public:
    static bool execute(const QSqlDatabase &database, const QString &statement, const QVariantMap &bindings = {});
    static std::optional<QVariant> scalarValue(const QSqlDatabase &database, const QString &statement, const QVariantMap &bindings = {});
    static std::optional<QString> stringValue(const QSqlDatabase &database, const QString &statement, const QVariantMap &bindings = {});
    
private:
    DatabaseHelpers() = delete;
};

#endif // DATABASE_HELPERS_H
