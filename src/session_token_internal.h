#ifndef AOWIS_DB_SESSION_TOKEN_INTERNAL_H
#define AOWIS_DB_SESSION_TOKEN_INTERNAL_H

#include <QByteArray>

namespace aowis::db
{
// Internal shared token helpers for admin issuance and password login.
QByteArray makeSessionToken();
QByteArray sessionTokenDigest(const QByteArray &random_bytes);
} // namespace aowis::db

#endif // AOWIS_DB_SESSION_TOKEN_INTERNAL_H
