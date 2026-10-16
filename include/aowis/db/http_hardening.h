#ifndef AOWIS_DB_HTTP_HARDENING_H
#define AOWIS_DB_HTTP_HARDENING_H

#include <QByteArray>
#include <QHttpServer>
#include <optional>

namespace aowis::db
{

// Configures a loopback-only HTTP adapter. Call before binding its listener.
// This is defense in depth, not an alternative to a TLS-terminating reverse
// proxy with its own body-size, rate, connection, and timeout limits.
void configureHttpHardening(QHttpServer &server);

// Parses exactly "Bearer " followed by 64 lowercase hexadecimal characters.
// Never accept identities or tokens from forwarding headers or URL arguments.
std::optional<QByteArray> parseBearerAuthorization(const QByteArray &authorization);

} // namespace aowis::db

#endif // AOWIS_DB_HTTP_HARDENING_H
