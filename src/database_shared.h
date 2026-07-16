#ifndef DATABASE_SHARED_H
#define DATABASE_SHARED_H

#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <optional>

class DatabaseShared : public QObject
{
    Q_OBJECT
    
public:
    explicit DatabaseShared(const QSqlDatabase &database, QObject *parent = nullptr);
    
    std::optional<QString> configValue(const QString &key) const;
    bool setConfigValue(const QString &key, const QString &value);
    
private:
    bool initTables();
    
    QSqlDatabase database;
};

#endif // DATABASE_SHARED_H
