#ifndef DATABASE_GUI_H
#define DATABASE_GUI_H

#include <QObject>

class DatabaseApi : public QObject
{
    Q_OBJECT
public:
    explicit DatabaseApi(QObject *parent = nullptr);
    
    DatabaseShared *sharedDatabase() const;
    
private slots:
    void databaseOpened();
    
signals:
    void signalReady();
    
};

#endif // DATABASE_GUI_H
