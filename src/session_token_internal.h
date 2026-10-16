#ifndef AOWIS_DB_SESSION_TOKEN_INTERNAL_H
#define AOWIS_DB_SESSION_TOKEN_INTERNAL_H

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>
#include <QtGlobal>

namespace aowis::db
{
// Internal shared token helpers for admin issuance and password login.
QByteArray makeSessionToken();
QByteArray sessionTokenDigest(const QByteArray &random_bytes);
// Called only while the caller holds BEGIN IMMEDIATE; prevents two writers
// from concurrently exceeding the per-account active-session quota.
constexpr qint64 max_active_sessions_per_user = 32;
bool hasSessionCapacity(const QSqlDatabase &connection, const QUuid &user_uuid,
                        qint64 at_ms, QString *error);
} // namespace aowis::db

#endif // AOWIS_DB_SESSION_TOKEN_INTERNAL_H
