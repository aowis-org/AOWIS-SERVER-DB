#include "database_gui.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSet>
#include <QStandardPaths>
#include <QTime>

#include <limits>

#include <sqlite3.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

namespace
{
QSet<DatabaseGui *> database_gui_instances;
}

EM_JS(void, aowisDatabaseGuiLoadStorage, (uintptr_t instance_address), {
    const mount_point = "/aowis";
    
    if (!Module.aowisDatabaseGuiStorageWaiters) {
        Module.aowisDatabaseGuiStorageWaiters = [];
    }
    
    Module.aowisDatabaseGuiStorageWaiters.push(instance_address);
    
    const notify_waiters = function(success) {
        const waiters = Module.aowisDatabaseGuiStorageWaiters;
        Module.aowisDatabaseGuiStorageWaiters = [];
        for (let index = 0; index < waiters.length; ++index) {
            _aowisDatabaseGuiStorageLoaded(waiters[index], success ? 1 : 0);
        }
    };
    
    if (Module.aowisDatabaseGuiStorageReady) {
        notify_waiters(true);
        return;
    }
    
    if (Module.aowisDatabaseGuiStorageLoading) {
        return;
    }
    
    Module.aowisDatabaseGuiStorageLoading = true;
    
    try {
        if (!Module.aowisDatabaseGuiStorageMounted) {
            try {
                FS.mkdir(mount_point);
            } catch (error) {
            }
            
            FS.mount(IDBFS, {}, mount_point);
            Module.aowisDatabaseGuiStorageMounted = true;
        }
        
        FS.syncfs(true, function(error) {
            Module.aowisDatabaseGuiStorageLoading = false;
            Module.aowisDatabaseGuiStorageReady = !error;
            
            if (error) {
                console.error("Failed to populate AOWIS persistent storage", error);
            }
            
            notify_waiters(!error);
        });
        
        if (navigator.storage && navigator.storage.persist) {
            navigator.storage.persist().catch(function(error) {
                console.warn("Unable to request persistent browser storage", error);
            });
        }
    } catch (error) {
        Module.aowisDatabaseGuiStorageLoading = false;
        console.error("Failed to mount AOWIS persistent storage", error);
        notify_waiters(false);
    }
});

EM_JS(void, aowisDatabaseGuiSyncStorage, (uintptr_t instance_address), {
    if (!Module.aowisDatabaseGuiSyncWaiters) {
        Module.aowisDatabaseGuiSyncWaiters = [];
    }
    
    Module.aowisDatabaseGuiSyncWaiters.push(instance_address);
    
    if (Module.aowisDatabaseGuiSyncRunning) {
        return;
    }
    
    Module.aowisDatabaseGuiSyncRunning = true;
    
    FS.syncfs(false, function(error) {
        const waiters = Module.aowisDatabaseGuiSyncWaiters;
        Module.aowisDatabaseGuiSyncWaiters = [];
        Module.aowisDatabaseGuiSyncRunning = false;
        
        if (error) {
            console.error("Failed to synchronize AOWIS persistent storage", error);
        }
        
        for (let index = 0; index < waiters.length; ++index) {
            _aowisDatabaseGuiStorageSynchronized(waiters[index], error ? 0 : 1);
        }
    });
});
#endif

DatabaseGui::DatabaseGui(QObject *parent)
    : QObject(parent)
{
#ifdef __EMSCRIPTEN__
    database_gui_instances.insert(this);
#endif
}

DatabaseGui::~DatabaseGui()
{
    close();

#ifdef __EMSCRIPTEN__
    database_gui_instances.remove(this);
#endif
}

void DatabaseGui::open(const DatabaseConfiguration &configuration)
{
    close();
    
    this->configuration = configuration;
    this->last_error.clear();
    this->database_path = resolveDatabasePath(configuration);
    
    if (configuration.backend != DatabaseBackend::SQLite)
    {
        failOpen(QStringLiteral("DatabaseGui supports only the SQLite backend"));
        return;
    }
    
    if (this->database_path.isEmpty())
    {
        failOpen(QStringLiteral("The SQLite database path is invalid"));
        return;
    }

#ifdef __EMSCRIPTEN__
    this->storage_ready = false;
    aowisDatabaseGuiLoadStorage(reinterpret_cast<std::uintptr_t>(this));
#else
    this->storage_ready = true;
    
    if (!prepareDatabaseDirectory() || !openSqliteDatabase())
    {
        failOpen(this->last_error);
    }
#endif
}

void DatabaseGui::close()
{
    const bool was_open = this->database_open;
    
    if (this->database_handle != nullptr)
    {
        sqlite3_close_v2(this->database_handle);
        this->database_handle = nullptr;
    }
    
    this->database_open = false;
    this->write_pending = false;
    
    if (was_open)
    {
        emit signalClosed();
    }
}

bool DatabaseGui::isOpen() const
{
    return this->database_open;
}

QString DatabaseGui::databasePath() const
{
    return this->database_path;
}

QString DatabaseGui::lastError() const
{
    return this->last_error;
}

bool DatabaseGui::execute(const QString &sql, const QVariantList &parameters)
{
    if (!this->database_open)
    {
        setLastError(QStringLiteral("The SQLite database is not open"));
        return false;
    }
    
    return executeInternal(sql, parameters, true);
}

bool DatabaseGui::query(const QString &sql, QVector<QVariantMap> *rows, const QVariantList &parameters)
{
    if (rows == nullptr)
    {
        setLastError(QStringLiteral("The query result container is null"));
        return false;
    }
    
    rows->clear();
    
    if (!this->database_open)
    {
        setLastError(QStringLiteral("The SQLite database is not open"));
        return false;
    }
    
    const QByteArray sql_utf8 = sql.toUtf8();
    sqlite3_stmt *statement = nullptr;
    const char *remaining_sql = nullptr;
    const int prepare_result = sqlite3_prepare_v2(this->database_handle, sql_utf8.constData(), sql_utf8.size(), &statement, &remaining_sql);
    
    if (prepare_result != SQLITE_OK)
    {
        setLastError(QString::fromUtf8(sqlite3_errmsg(this->database_handle)));
        return false;
    }
    
    if (statement == nullptr)
    {
        setLastError(QStringLiteral("The SQLite statement is empty"));
        return false;
    }
    
    if (remaining_sql != nullptr && !QString::fromUtf8(remaining_sql).trimmed().isEmpty())
    {
        setLastError(QStringLiteral("Only one SQLite statement may be executed per call"));
        sqlite3_finalize(statement);
        return false;
    }
    
    if (sqlite3_stmt_readonly(statement) == 0)
    {
        setLastError(QStringLiteral("DatabaseGui::query accepts read-only SQL statements only"));
        sqlite3_finalize(statement);
        return false;
    }
    
    if (!bindParameters(statement, parameters))
    {
        sqlite3_finalize(statement);
        return false;
    }
    
    int step_result = SQLITE_ROW;
    
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        QVariantMap row;
        const int column_count = sqlite3_column_count(statement);
        
        for (int column_index = 0; column_index < column_count; ++column_index)
        {
            const char *column_name = sqlite3_column_name(statement, column_index);
            row.insert(QString::fromUtf8(column_name), columnValue(statement, column_index));
        }
        
        rows->append(row);
    }
    
    if (step_result != SQLITE_DONE)
    {
        setLastError(QString::fromUtf8(sqlite3_errmsg(this->database_handle)));
        sqlite3_finalize(statement);
        return false;
    }
    
    sqlite3_finalize(statement);
    this->last_error.clear();
    return true;
}

bool DatabaseGui::beginTransaction()
{
    return execute(QStringLiteral("BEGIN IMMEDIATE"));
}

bool DatabaseGui::commitTransaction()
{
    return execute(QStringLiteral("COMMIT"));
}

bool DatabaseGui::rollbackTransaction()
{
    return execute(QStringLiteral("ROLLBACK"));
}

void DatabaseGui::synchronizePersistence()
{
    requestPersistenceSynchronization();
}

QString DatabaseGui::resolveDatabasePath(const DatabaseConfiguration &configuration) const
{
    QString filename = configuration.sqlite_database_filename.trimmed();
    
    if (filename.isEmpty())
    {
        filename = QStringLiteral("aowis-server.sqlite3");
    }
    
    QString configured_path = configuration.sqlite_database_path.trimmed();

#ifdef __EMSCRIPTEN__
    QString path;
    
    if (configured_path.isEmpty())
    {
        path = QDir(QStringLiteral("/aowis")).filePath(filename);
    }
    else if (QDir::isAbsolutePath(configured_path))
    {
        path = configured_path;
    }
    else
    {
        path = QDir(QStringLiteral("/aowis")).filePath(configured_path);
    }
    
    path = QDir::cleanPath(path);
    
    if (path == QStringLiteral("/aowis") || !path.startsWith(QStringLiteral("/aowis/")))
    {
        return QString();
    }
    
    return path;
#else
    if (configured_path.isEmpty())
    {
        const QString application_data_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        return QDir(application_data_path).filePath(filename);
    }
    
    if (QDir::isAbsolutePath(configured_path))
    {
        return QDir::cleanPath(configured_path);
    }
    
    const QString application_data_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir::cleanPath(QDir(application_data_path).filePath(configured_path));
#endif
}

bool DatabaseGui::prepareDatabaseDirectory()
{
    const QFileInfo database_file_info(this->database_path);
    const QString database_directory = database_file_info.absolutePath();
    
    if (database_directory.isEmpty())
    {
        setLastError(QStringLiteral("The SQLite database directory is invalid"));
        return false;
    }
    
    if (!QDir().mkpath(database_directory))
    {
        setLastError(QStringLiteral("Failed to create SQLite database directory: %1").arg(database_directory));
        return false;
    }
    
    return true;
}

bool DatabaseGui::openSqliteDatabase()
{
    const QByteArray database_path_utf8 = this->database_path.toUtf8();
    sqlite3 *database_handle = nullptr;
    const int open_result = sqlite3_open_v2(database_path_utf8.constData(), &database_handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    
    if (open_result != SQLITE_OK)
    {
        const QString error_message = database_handle != nullptr
                                          ? QString::fromUtf8(sqlite3_errmsg(database_handle))
                                          : QStringLiteral("Failed to allocate an SQLite database handle");
        
        if (database_handle != nullptr)
        {
            sqlite3_close_v2(database_handle);
        }
        
        setLastError(error_message);
        return false;
    }
    
    this->database_handle = database_handle;
    sqlite3_extended_result_codes(this->database_handle, 1);
    sqlite3_busy_timeout(this->database_handle, this->configuration.sqlite_busy_timeout_ms);
    
    if (!configureSqliteDatabase())
    {
        sqlite3_close_v2(this->database_handle);
        this->database_handle = nullptr;
        return false;
    }
    
    this->database_open = true;
    this->last_error.clear();
    emit signalOpened();

#ifdef __EMSCRIPTEN__
    requestPersistenceSynchronization();
#endif
    
    return true;
}

bool DatabaseGui::configureSqliteDatabase()
{
    if (!executeInternal(QStringLiteral("PRAGMA foreign_keys = ON"), QVariantList(), false))
    {
        return false;
    }
    
    if (!executeInternal(QStringLiteral("PRAGMA journal_mode = DELETE"), QVariantList(), false))
    {
        return false;
    }
    
    if (!executeInternal(QStringLiteral("PRAGMA synchronous = FULL"), QVariantList(), false))
    {
        return false;
    }
    
    if (!executeInternal(QStringLiteral("PRAGMA temp_store = MEMORY"), QVariantList(), false))
    {
        return false;
    }
    
    return true;
}

bool DatabaseGui::executeInternal(const QString &sql, const QVariantList &parameters, bool synchronize_changes)
{
    const QByteArray sql_utf8 = sql.toUtf8();
    sqlite3_stmt *statement = nullptr;
    const char *remaining_sql = nullptr;
    const int prepare_result = sqlite3_prepare_v2(this->database_handle, sql_utf8.constData(), sql_utf8.size(), &statement, &remaining_sql);
    
    if (prepare_result != SQLITE_OK)
    {
        setLastError(QString::fromUtf8(sqlite3_errmsg(this->database_handle)));
        return false;
    }
    
    if (statement == nullptr)
    {
        setLastError(QStringLiteral("The SQLite statement is empty"));
        return false;
    }
    
    if (remaining_sql != nullptr && !QString::fromUtf8(remaining_sql).trimmed().isEmpty())
    {
        setLastError(QStringLiteral("Only one SQLite statement may be executed per call"));
        sqlite3_finalize(statement);
        return false;
    }
    
    if (!bindParameters(statement, parameters))
    {
        sqlite3_finalize(statement);
        return false;
    }
    
    const bool statement_is_read_only = sqlite3_stmt_readonly(statement) != 0;
    int step_result = SQLITE_ROW;
    
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW)
    {
    }
    
    if (step_result != SQLITE_DONE)
    {
        setLastError(QString::fromUtf8(sqlite3_errmsg(this->database_handle)));
        sqlite3_finalize(statement);
        return false;
    }
    
    sqlite3_finalize(statement);
    this->last_error.clear();
    
    if (synchronize_changes)
    {
        if (!statement_is_read_only)
        {
            this->write_pending = true;
        }
        
        const bool transaction_active = sqlite3_get_autocommit(this->database_handle) == 0;
        
        if (this->write_pending && !transaction_active)
        {
            this->write_pending = false;
            requestPersistenceSynchronization();
        }
    }
    
    return true;
}

bool DatabaseGui::bindParameters(sqlite3_stmt *statement, const QVariantList &parameters)
{
    const int expected_parameter_count = sqlite3_bind_parameter_count(statement);
    
    if (expected_parameter_count != parameters.size())
    {
        setLastError(QStringLiteral("SQLite statement expects %1 parameters, but %2 were supplied")
                         .arg(expected_parameter_count)
                         .arg(parameters.size()));
        return false;
    }
    
    for (int parameter_index = 0; parameter_index < parameters.size(); ++parameter_index)
    {
        if (!bindValue(statement, parameter_index + 1, parameters.at(parameter_index)))
        {
            return false;
        }
    }
    
    return true;
}

bool DatabaseGui::bindValue(sqlite3_stmt *statement, int parameter_index, const QVariant &value)
{
    int bind_result = SQLITE_OK;
    
    if (!value.isValid() || value.isNull())
    {
        bind_result = sqlite3_bind_null(statement, parameter_index);
    }
    else
    {
        const int type_id = value.metaType().id();
        
        switch (type_id)
        {
        case QMetaType::Bool:
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::LongLong:
        case QMetaType::Short:
        case QMetaType::Char:
        case QMetaType::SChar:
            bind_result = sqlite3_bind_int64(statement, parameter_index, value.toLongLong());
            break;
            
        case QMetaType::UInt:
        case QMetaType::ULong:
        case QMetaType::ULongLong:
        case QMetaType::UShort:
        case QMetaType::UChar:
        {
            const qulonglong unsigned_value = value.toULongLong();
            
            if (unsigned_value > static_cast<qulonglong>(std::numeric_limits<qint64>::max()))
            {
                setLastError(QStringLiteral("Unsigned integer exceeds SQLite's signed 64-bit integer range"));
                return false;
            }
            
            bind_result = sqlite3_bind_int64(statement, parameter_index, static_cast<sqlite3_int64>(unsigned_value));
            break;
        }
            
        case QMetaType::Float:
        case QMetaType::Double:
            bind_result = sqlite3_bind_double(statement, parameter_index, value.toDouble());
            break;
            
        case QMetaType::QByteArray:
        {
            const QByteArray bytes = value.toByteArray();
            bind_result = sqlite3_bind_blob(statement, parameter_index, bytes.constData(), bytes.size(), SQLITE_TRANSIENT);
            break;
        }
            
        case QMetaType::QDateTime:
        {
            const QByteArray text = value.toDateTime().toString(Qt::ISODateWithMs).toUtf8();
            bind_result = sqlite3_bind_text(statement, parameter_index, text.constData(), text.size(), SQLITE_TRANSIENT);
            break;
        }
            
        case QMetaType::QDate:
        {
            const QByteArray text = value.toDate().toString(Qt::ISODate).toUtf8();
            bind_result = sqlite3_bind_text(statement, parameter_index, text.constData(), text.size(), SQLITE_TRANSIENT);
            break;
        }
            
        case QMetaType::QTime:
        {
            const QByteArray text = value.toTime().toString(Qt::ISODateWithMs).toUtf8();
            bind_result = sqlite3_bind_text(statement, parameter_index, text.constData(), text.size(), SQLITE_TRANSIENT);
            break;
        }
            
        default:
        {
            const QByteArray text = value.toString().toUtf8();
            bind_result = sqlite3_bind_text(statement, parameter_index, text.constData(), text.size(), SQLITE_TRANSIENT);
            break;
        }
        }
    }
    
    if (bind_result != SQLITE_OK)
    {
        setLastError(QString::fromUtf8(sqlite3_errmsg(this->database_handle)));
        return false;
    }
    
    return true;
}

QVariant DatabaseGui::columnValue(sqlite3_stmt *statement, int column_index) const
{
    const int column_type = sqlite3_column_type(statement, column_index);
    
    switch (column_type)
    {
    case SQLITE_INTEGER:
        return QVariant::fromValue(static_cast<qint64>(sqlite3_column_int64(statement, column_index)));
        
    case SQLITE_FLOAT:
        return QVariant::fromValue(sqlite3_column_double(statement, column_index));
        
    case SQLITE_TEXT:
    {
        const unsigned char *text = sqlite3_column_text(statement, column_index);
        const int byte_count = sqlite3_column_bytes(statement, column_index);
        return QString::fromUtf8(reinterpret_cast<const char *>(text), byte_count);
    }
        
    case SQLITE_BLOB:
    {
        const void *data = sqlite3_column_blob(statement, column_index);
        const int byte_count = sqlite3_column_bytes(statement, column_index);
        return QByteArray(static_cast<const char *>(data), byte_count);
    }
        
    case SQLITE_NULL:
    default:
        return QVariant();
    }
}

void DatabaseGui::setLastError(const QString &message)
{
    this->last_error = message;
}

void DatabaseGui::failOpen(const QString &message)
{
    setLastError(message);
    emit signalOpenFailed(message);
}

void DatabaseGui::requestPersistenceSynchronization()
{
#ifdef __EMSCRIPTEN__
    if (!this->storage_ready)
    {
        this->synchronization_pending = true;
        return;
    }
    
    if (this->synchronization_in_progress)
    {
        this->synchronization_pending = true;
        return;
    }
    
    this->synchronization_in_progress = true;
    aowisDatabaseGuiSyncStorage(reinterpret_cast<std::uintptr_t>(this));
#else
    emit signalPersistenceSynchronized();
#endif
}

#ifdef __EMSCRIPTEN__
void DatabaseGui::handleStorageLoaded(bool success)
{
    if (!success)
    {
        failOpen(QStringLiteral("Failed to load persistent browser storage from IndexedDB"));
        return;
    }
    
    this->storage_ready = true;
    
    if (!prepareDatabaseDirectory() || !openSqliteDatabase())
    {
        failOpen(this->last_error);
        return;
    }
    
    if (this->synchronization_pending)
    {
        this->synchronization_pending = false;
        requestPersistenceSynchronization();
    }
}

void DatabaseGui::handleStorageSynchronized(bool success)
{
    this->synchronization_in_progress = false;
    
    if (!success)
    {
        setLastError(QStringLiteral("Failed to synchronize the SQLite database to IndexedDB"));
        emit signalPersistenceFailed(this->last_error);
        return;
    }
    
    if (this->synchronization_pending)
    {
        this->synchronization_pending = false;
        requestPersistenceSynchronization();
        return;
    }
    
    emit signalPersistenceSynchronized();
}

extern "C" EMSCRIPTEN_KEEPALIVE void aowisDatabaseGuiStorageLoaded(std::uintptr_t instance_address, int success)
{
    DatabaseGui *database = reinterpret_cast<DatabaseGui *>(instance_address);
    
    if (!database_gui_instances.contains(database))
    {
        return;
    }
    
    database->handleStorageLoaded(success != 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void aowisDatabaseGuiStorageSynchronized(std::uintptr_t instance_address, int success)
{
    DatabaseGui *database = reinterpret_cast<DatabaseGui *>(instance_address);
    
    if (!database_gui_instances.contains(database))
    {
        return;
    }
    
    database->handleStorageSynchronized(success != 0);
}
#endif
