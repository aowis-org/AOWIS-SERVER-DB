#include "postgres_backend.h"

#include <QMetaType>
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

// One transaction-wide advisory lock serializes read/modify/write operations
// within each AOWIS schema, matching SQLite's BEGIN IMMEDIATE semantics.
// PostgreSQL two-key advisory locks are database-local; the namespace OID is
// unique among schemas in that database. This preserves single-schema writer
// serialization without making unrelated schemas wait for one global lock.
// All cooperating AOWIS writers must use the same namespace and OID rule.
constexpr qint32 write_lock_namespace = 1096232777; // "AWOI"
}

PostgresBackend::PostgresBackend(const QString &schema_name)
    : schema_name_(schema_name)
{
}

QVariant PostgresBackend::bindUuid(const QUuid &uuid) const
{
    // QSqlResultPrivate::isVariantNull() treats a null QUuid as SQL NULL,
    // even though AOWIS sometimes intentionally stores the all-zero UUID.
    // Bind that one value as UUID text instead; PostgreSQL infers the native
    // uuid type from the target column or comparison expression.
    if (uuid.isNull())
        return QVariant(uuid.toString(QUuid::WithoutBraces));
    return QVariant::fromValue(uuid);
}

QUuid PostgresBackend::readUuid(const QVariant &value) const
{
    if (value.isNull())
        return QUuid();
    if (value.metaType().id() == QMetaType::QUuid)
        return value.toUuid();
    // Some SQL expressions return UUIDs as text rather than typed columns.
    // Reject everything except a valid UUID string; never interpret a BYTEA
    // or malformed value as a valid PostgreSQL identifier.
    if (value.metaType().id() == QMetaType::QString)
        return QUuid::fromString(value.toString());
    return QUuid();
}

bool PostgresBackend::open(QSqlDatabase &database, const QString &connection_name,
                           const QString &database_name, QString *error) const
{
    if (database_name.isEmpty())
    {
        setError(error, QStringLiteral("PostgreSQL database name is required"));
        return false;
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QPSQL")))
    {
        setError(error, QStringLiteral("Qt QPSQL driver is unavailable"));
        return false;
    }
    database = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connection_name);
    database.setDatabaseName(database_name);
    // libpq gets host, port, user and credentials from PGHOST, PGPORT,
    // PGUSER, PGPASSWORD, .pgpass or service configuration as usual.
    // The database name is not a libpq connection string.
    if (database.open())
        return true;
    setError(error, database.lastError().text());
    return false;
}

bool PostgresBackend::configure(const QSqlDatabase &database, QString *error) const
{
    // Schema names are identifiers, not values. Validate strictly before
    // putting one into SQL; PostgreSQL silently ignores missing search_path
    // entries, so also verify that current_schema() is exactly the selection.
    if (this->schema_name_.isEmpty() || this->schema_name_.size() > 63)
    {
        setError(error, QStringLiteral("Invalid PostgreSQL schema name"));
        return false;
    }
    for (const QChar character : this->schema_name_)
    {
        const ushort c = character.unicode();
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
        {
            setError(error, QStringLiteral("PostgreSQL schema names must contain only lowercase ASCII, digits, or underscores"));
            return false;
        }
    }
    if (this->schema_name_.at(0).isDigit())
    {
        setError(error, QStringLiteral("PostgreSQL schema name cannot start with a digit"));
        return false;
    }
    if (!exec(database, QStringLiteral("SET search_path TO %1").arg(this->schema_name_), error))
        return false;
    QSqlQuery actual(database);
    if (!actual.exec(QStringLiteral("SELECT current_schema()")) || !actual.next() ||
        actual.value(0).toString() != this->schema_name_)
    {
        setError(error, QStringLiteral("PostgreSQL schema does not exist or is not accessible: %1").arg(this->schema_name_));
        return false;
    }
    actual.finish();
    // AOWIS stores timestamps as integer Unix milliseconds.
    return exec(database, QStringLiteral("SET TIME ZONE 'UTC'"), error) &&
           exec(database, QStringLiteral("SET lock_timeout = '5s'"), error);
}

std::optional<bool> PostgresBackend::hasSchema(const QSqlDatabase &database,
                                               QString *error) const
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_tables "
            "WHERE schemaname = current_schema())")) || !query.next())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    return query.value(0).toBool();
}

bool PostgresBackend::beginWrite(const QSqlDatabase &database, QString *error) const
{
    if (!exec(database, QStringLiteral("BEGIN"), error))
        return false;

    // Resolve the selected schema's exact PostgreSQL namespace OID in the
    // transaction. Unlike hashing a schema name, the OID cannot collide with
    // any other live schema in this database. PostgreSQL advisory lock keys
    // are signed int32: casting the 32-bit OID preserves its bit pattern.
    // Also fail closed if the connection's search_path was unexpectedly
    // changed since configure(), rather than locking one schema and writing
    // into another.
    QSqlQuery schema(database);
    if (!schema.prepare(QStringLiteral(
            "SELECT n.oid::integer FROM pg_catalog.pg_namespace n "
            "WHERE n.nspname=CAST(? AS name) AND n.nspname=current_schema()")))
    {
        setError(error, schema.lastError().text());
        exec(database, QStringLiteral("ROLLBACK"), nullptr);
        return false;
    }
    schema.addBindValue(this->schema_name_);
    if (!schema.exec() || !schema.next() || schema.value(0).isNull())
    {
        setError(error, schema.lastError().isValid() ? schema.lastError().text() :
                 QStringLiteral("PostgreSQL selected schema is missing or search_path changed"));
        schema.finish();
        exec(database, QStringLiteral("ROLLBACK"), nullptr);
        return false;
    }
    const qint32 schema_resource = schema.value(0).toInt();
    schema.finish();

    QSqlQuery lock(database);
    if (lock.prepare(QStringLiteral("SELECT pg_advisory_xact_lock(?, ?)")))
    {
        lock.addBindValue(write_lock_namespace);
        lock.addBindValue(schema_resource);
        if (lock.exec())
        {
            lock.finish();
            return true;
        }
    }
    setError(error, lock.lastError().text());
    lock.finish();
    exec(database, QStringLiteral("ROLLBACK"), nullptr);
    return false;
}

bool PostgresBackend::commit(const QSqlDatabase &database, QString *error) const
{
    return exec(database, QStringLiteral("COMMIT"), error);
}

void PostgresBackend::rollback(const QSqlDatabase &database) const
{
    exec(database, QStringLiteral("ROLLBACK"), nullptr);
}

std::optional<bool> PostgresBackend::hasColumn(const QSqlDatabase &database,
                                               const QString &table, const QString &column,
                                               QString *error) const
{
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
        setError(error, QStringLiteral("Invalid PostgreSQL schema identifier"));
        return std::nullopt;
    }
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT EXISTS (SELECT 1 FROM information_schema.columns "
            "WHERE table_schema = current_schema() AND table_name = ? AND column_name = ?)")))
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    query.addBindValue(table);
    query.addBindValue(column);
    if (!query.exec() || !query.next())
    {
        setError(error, query.lastError().text());
        return std::nullopt;
    }
    return query.value(0).toBool();
}

} // namespace aowis::db
