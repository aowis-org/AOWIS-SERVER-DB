#include "sqlite_backend.h"

#include <QSqlError>
#include <QSqlQuery>

namespace aowis::db
{
namespace
{
void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

bool exec(const QSqlDatabase &database, const QString &statement, QString *error)
{
    QSqlQuery query(database);
    if (query.exec(statement))
        return true;
    setError(error, query.lastError().text());
    return false;
}
}

QVariant SqliteBackend::bindUuid(const QUuid &uuid) const
{
    // This is a BLOB, not the textual UUID and not SQL NULL even for
    // QUuid(). Both SQLite storage and historical integrity checks rely on
    // exactly these bytes.
    return QVariant(uuid.toRfc4122());
}

QUuid SqliteBackend::readUuid(const QVariant &value) const
{
    if (value.isNull())
        return QUuid();
    const QByteArray bytes = value.toByteArray();
    return bytes.size() == 16 ? QUuid::fromRfc4122(bytes) : QUuid();
}

bool SqliteBackend::open(QSqlDatabase &database, const QString &connection_name,
                         const QString &file_path, QString *error) const
{
    database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
    database.setDatabaseName(file_path);
    if (database.open())
        return true;
    setError(error, database.lastError().text());
    return false;
}

bool SqliteBackend::configure(const QSqlDatabase &database, QString *error) const
{
    if (!exec(database, QStringLiteral("PRAGMA foreign_keys = ON"), error) ||
        !exec(database, QStringLiteral("PRAGMA busy_timeout = 5000"), error) ||
        !exec(database, QStringLiteral("PRAGMA journal_mode = WAL"), error))
        return false;

    // PRAGMA foreign_keys=ON can succeed without taking effect if called
    // within a transaction. Never operate on a database with FK checks off.
    QSqlQuery pragma(database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys")) || !pragma.next() ||
        pragma.value(0).toInt() != 1)
    {
        setError(error, QStringLiteral("SQLite foreign-key enforcement is disabled"));
        return false;
    }
    pragma.finish();
    if (!pragma.exec(QStringLiteral("PRAGMA busy_timeout")) || !pragma.next() ||
        pragma.value(0).toInt() < 5000)
    {
        setError(error, QStringLiteral("SQLite busy timeout could not be configured"));
        return false;
    }
    pragma.finish();
    // In-memory databases use 'memory', not WAL, by SQLite design.
    if (!pragma.exec(QStringLiteral("PRAGMA journal_mode")) || !pragma.next())
    {
        setError(error, QStringLiteral("Cannot read SQLite journal mode"));
        return false;
    }
    const QString mode = pragma.value(0).toString();
    pragma.finish();
    const QString expected = database.databaseName() == QStringLiteral(":memory:")
        ? QStringLiteral("memory") : QStringLiteral("wal");
    if (mode.compare(expected, Qt::CaseInsensitive) != 0)
    {
        setError(error, QStringLiteral("Unexpected SQLite journal mode: %1").arg(mode));
        return false;
    }
    return true;
}

std::optional<bool> SqliteBackend::hasSchema(const QSqlDatabase &database, QString *error) const
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%')")) || !query.next())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    return query.value(0).toBool();
}

bool SqliteBackend::beginWrite(const QSqlDatabase &database, QString *error) const
{
    // BEGIN IMMEDIATE reserves the single SQLite writer before any edits.
    return exec(database, QStringLiteral("BEGIN IMMEDIATE"), error);
}

bool SqliteBackend::commit(const QSqlDatabase &database, QString *error) const
{
    return exec(database, QStringLiteral("COMMIT"), error);
}

void SqliteBackend::rollback(const QSqlDatabase &database) const
{
    exec(database, QStringLiteral("ROLLBACK"), nullptr);
}

std::optional<bool> SqliteBackend::hasColumn(const QSqlDatabase &database,
                                              const QString &table, const QString &column,
                                              QString *error) const
{
    // Interpolated SQLite PRAGMA identifiers must not contain user SQL.
    const auto validIdentifier = [](const QString &name) {
        if (name.isEmpty())
            return false;
        for (const QChar ch : name)
        {
            const ushort c = ch.unicode();
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                return false;
        }
        return true;
    };
    if (!validIdentifier(table) || !validIdentifier(column))
    {
        setError(error, QStringLiteral("Invalid schema identifier"));
        return std::nullopt;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table)))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    while (query.next())
    {
        if (query.value(1).toString() == column)
            return true;
    }
    if (query.lastError().isValid())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    return false;
}

} // namespace aowis::db
