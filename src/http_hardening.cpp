#include <aowis/db/http_hardening.h>

#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QLatin1StringView>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QHttpHeaders>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#include <QHttpServerConfiguration>
#endif

#include <chrono>
#include <utility>

namespace aowis::db
{

std::optional<QByteArray> parseBearerAuthorization(const QByteArray &authorization)
{
    constexpr qsizetype prefix_length = 7;
    constexpr qsizetype token_length = 64;
    if (authorization.size() != prefix_length + token_length ||
        !authorization.startsWith(QByteArrayLiteral("Bearer ")))
        return std::nullopt;

    const QByteArray token = authorization.mid(prefix_length);
    for (const char ch : token)
    {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return std::nullopt;
    }
    return token;
}

void configureHttpHardening(QHttpServer &server)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    QHttpServerConfiguration configuration = server.configuration();
    // On loopback these limits are shared by all clients of the reverse proxy.
    // The proxy must additionally rate-limit clients by their real IP.
    configuration.setRateLimitPerSecond(120);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    configuration.setKeepAliveTimeout(std::chrono::seconds(10));
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    // Parser-level limits are available from Qt 6.11. Earlier Qt requires
    // equivalent limits at the proxy; route handlers still bound JSON bodies.
    configuration.setMaximumBodySize(8 * 1024 * 1024);
    configuration.setMaximumUrlSize(8192);
    configuration.setMaximumTotalHeaderSize(16384);
    configuration.setMaximumHeaderFieldSize(4096);
    configuration.setMaximumHeaderFieldCount(64);
#endif
    server.setConfiguration(configuration);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    server.addAfterRequestHandler(&server,
        [](const QHttpServerRequest &, QHttpServerResponse &response) {
            QHttpHeaders headers = response.headers();
            headers.replaceOrAppend(QLatin1StringView("Cache-Control"), QLatin1StringView("no-store"));
            headers.replaceOrAppend(QLatin1StringView("Pragma"), QLatin1StringView("no-cache"));
            headers.replaceOrAppend(QLatin1StringView("X-Content-Type-Options"), QLatin1StringView("nosniff"));
            headers.replaceOrAppend(QLatin1StringView("Referrer-Policy"), QLatin1StringView("no-referrer"));
            response.setHeaders(std::move(headers));
        });
#endif
}

} // namespace aowis::db
