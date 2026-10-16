#ifndef AOWIS_DB_HTTP_ROUTES_H
#define AOWIS_DB_HTTP_ROUTES_H

#include <aowis/db/database.h>
#include <aowis/db/login_throttle.h>

#include <functional>
#include <QElapsedTimer>
#include <optional>

#include <QHttpServer>
#include <QHttpServerRequest>

namespace aowis::db
{

// Optional HTTP adapter; it does not own or share the database connection.
// Register and use only on the database's owning Qt event-loop thread.
// The host must authenticate credentials and resolve them to a trusted actor;
// client-supplied actor identifiers must NEVER be treated as credentials.
class HttpRoutes final
{
public:
    using Authenticate = std::function<std::optional<OperationContext>(const QHttpServerRequest &)>;

    HttpRoutes(Database &database, Authenticate authenticate);
    bool registerOn(QHttpServer &server);

private:
    std::optional<OperationContext> actorFor(const QHttpServerRequest &request) const;

    Database &database_;
    Authenticate authenticate_;
    QElapsedTimer login_clock_;
    LoginThrottle login_throttle_;
};

} // namespace aowis::db

#endif // AOWIS_DB_HTTP_ROUTES_H
