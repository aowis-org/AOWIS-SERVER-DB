#ifndef AOWIS_DB_STORAGE_BACKEND_H
#define AOWIS_DB_STORAGE_BACKEND_H

#include <optional>

#include <QSqlDatabase>
#include <QString>
#include <QUuid>
#include <QVariant>

namespace aowis::db
{
// Internal, per-connection persistence-engine contract. This is deliberately
// NOT an ORM: domain queries still use Qt SQL, while engine-specific connection,
// transaction and schema-introspection behavior lives here.
// PostgreSQL must implement equivalent semantics before it can be exposed.
class StorageBackend
{
public:
    virtual ~StorageBackend() = default;

    // SQL UUID values are backend-specific; cryptographic hash encoding is not.
    // SQLite stores UUIDs as the exact 16 RFC-4122 bytes; PostgreSQL
    // binds native uuid values without rewriting
    // domain queries or confusing UUID NULL with the all-zero UUID.
    virtual QVariant bindUuid(const QUuid &uuid) const = 0;
    // SQL NULL maps to QUuid() where nullable identifiers are supported.
    virtual QUuid readUuid(const QVariant &value) const = 0;

    virtual bool open(QSqlDatabase &database, const QString &connection_name,
                      const QString &location, QString *error) const = 0;
    virtual bool configure(const QSqlDatabase &database, QString *error) const = 0;
    virtual bool verifySchema(const QSqlDatabase &database, QString *error) const = 0;
    // Create the current schema in the caller's write transaction. There is
    // no schema upgrade framework: development databases can be recreated.
    virtual bool createSchema(const QSqlDatabase &database, QString *error) const = 0;
    // Determine whether an existing database already contains application tables.
    virtual std::optional<bool> hasSchema(const QSqlDatabase &database,
                                           QString *error) const = 0;
    // Hold an appropriate write lock for the entire mutation. In SQLite this
    // means BEGIN IMMEDIATE; PostgreSQL uses a transaction-scoped advisory
    // lock to serialize cooperating AOWIS writers.
    virtual bool beginWrite(const QSqlDatabase &database, QString *error) const = 0;
    virtual bool commit(const QSqlDatabase &database, QString *error) const = 0;
    virtual void rollback(const QSqlDatabase &database) const = 0;
    // Return nullopt for SQL/metadata failures, false only when absent.
    virtual std::optional<bool> hasColumn(const QSqlDatabase &database,
                                           const QString &table, const QString &column,
                                           QString *error) const = 0;
};
}

#endif // AOWIS_DB_STORAGE_BACKEND_H
