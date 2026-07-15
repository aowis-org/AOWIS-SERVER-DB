#ifndef DATABASE_GUI_H
#define DATABASE_GUI_H

#include "models/database_configuration.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <cstdint>

struct sqlite3;
struct sqlite3_stmt;

#ifdef __EMSCRIPTEN__
extern "C" void aowisDatabaseGuiStorageLoaded(std::uintptr_t instance_address, int success);
extern "C" void aowisDatabaseGuiStorageSynchronized(std::uintptr_t instance_address, int success);
#endif

class DatabaseGui final : public QObject
{
    Q_OBJECT
    
public:
    explicit DatabaseGui(QObject *parent = nullptr);
    ~DatabaseGui() override;
    
    void open(const DatabaseConfiguration &configuration);
    void close();
    
    bool isOpen() const;
    QString databasePath() const;
    QString lastError() const;
    
    bool execute(const QString &sql, const QVariantList &parameters = QVariantList());
    bool query(const QString &sql, QVector<QVariantMap> *rows, const QVariantList &parameters = QVariantList());
    
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    
    void synchronizePersistence();
    
signals:
    void signalOpened();
    void signalOpenFailed(const QString &message);
    void signalClosed();
    void signalPersistenceSynchronized();
    void signalPersistenceFailed(const QString &message);
    
private:
    QString resolveDatabasePath(const DatabaseConfiguration &configuration) const;
    bool prepareDatabaseDirectory();
    bool openSqliteDatabase();
    bool configureSqliteDatabase();
    bool executeInternal(const QString &sql, const QVariantList &parameters, bool synchronize_changes);
    bool bindParameters(sqlite3_stmt *statement, const QVariantList &parameters);
    bool bindValue(sqlite3_stmt *statement, int parameter_index, const QVariant &value);
    QVariant columnValue(sqlite3_stmt *statement, int column_index) const;
    void setLastError(const QString &message);
    void failOpen(const QString &message);
    void requestPersistenceSynchronization();

#ifdef __EMSCRIPTEN__
    void handleStorageLoaded(bool success);
    void handleStorageSynchronized(bool success);
    
    friend void aowisDatabaseGuiStorageLoaded(std::uintptr_t instance_address, int success);
    friend void aowisDatabaseGuiStorageSynchronized(std::uintptr_t instance_address, int success);
#endif
    
    sqlite3 *database_handle = nullptr;
    DatabaseConfiguration configuration;
    QString database_path;
    QString last_error;
    bool database_open = false;
    bool storage_ready = false;
    bool write_pending = false;
    bool synchronization_in_progress = false;
    bool synchronization_pending = false;
};

#endif // DATABASE_GUI_H
