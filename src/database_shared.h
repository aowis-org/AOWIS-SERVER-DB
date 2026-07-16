#ifndef DATABASE_SHARED_H
#define DATABASE_SHARED_H

#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <QDateTime>
#include <QUuid>

#include <optional>

#include <aowis/model/project.h>

class DatabaseShared : public QObject
{
    Q_OBJECT
    
public:
    explicit DatabaseShared(const QSqlDatabase &database, QObject *parent = nullptr);
    
    bool initialize();
    
    QUuid createProject(const QString &name, const QString &description = {});
    std::optional<Project> projectById(const QUuid &projectId) const;
    
    std::optional<QString> configValue(const QString &key) const;
    bool setConfigValue(const QString &key, const QString &value);
    
private:
    QSqlDatabase database;
};

#endif // DATABASE_SHARED_H
