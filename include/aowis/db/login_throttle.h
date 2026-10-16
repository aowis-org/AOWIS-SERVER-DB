#ifndef AOWIS_DB_LOGIN_THROTTLE_H
#define AOWIS_DB_LOGIN_THROTTLE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace aowis::db
{

// Bounded HTTP-only pre-KDF throttle. HttpRoutes accesses this on its owning
// Qt event-loop thread; no mutex, wall clock, user enumeration, or database
// transaction is needed. SQL account lockouts remain independently enforced.
class LoginThrottle final
{
public:
    // Explicit monotonic milliseconds make boundary tests deterministic.
    bool allowAttempt(const QString &username, qint64 monotonic_ms);

    static constexpr int per_username_max = 8;
    static constexpr int global_max = 20;
    static constexpr qint64 window_ms = 60000;

private:
    QVector<qint64> global_attempts_;
    QHash<QByteArray, QVector<qint64>> username_attempts_;
};

} // namespace aowis::db

#endif // AOWIS_DB_LOGIN_THROTTLE_H
