#include <aowis/db/login_throttle.h>

#include <QCryptographicHash>

namespace aowis::db
{
namespace
{
void purgeExpired(QVector<qint64> &attempts, qint64 now_ms)
{
    qsizetype count = 0;
    while (count < attempts.size() && now_ms - attempts.at(count) >= LoginThrottle::window_ms)
        ++count;
    if (count > 0)
        attempts.remove(0, count);
}

QByteArray bucketKey(const QString &username)
{
    // Fold deliberately, so an attacker cannot evade per-name limits just
    // by varying Unicode normalization or username casing. This does NOT
    // change the database's own case-sensitive username lookup.
    return QCryptographicHash::hash(
        username.normalized(QString::NormalizationForm_KC).toCaseFolded().toUtf8(),
        QCryptographicHash::Sha256);
}
}

bool LoginThrottle::allowAttempt(const QString &username, qint64 monotonic_ms)
{
    purgeExpired(this->global_attempts_, monotonic_ms);

    // Expire idle buckets even when a different username is presented. The
    // map is bounded by global_max because only accepted requests are stored.
    for (QHash<QByteArray, QVector<qint64>>::iterator it = this->username_attempts_.begin();
         it != this->username_attempts_.end();)
    {
        purgeExpired(it.value(), monotonic_ms);
        if (it.value().isEmpty())
            it = this->username_attempts_.erase(it);
        else
            ++it;
    }

    const QByteArray key = bucketKey(username);
    const QHash<QByteArray, QVector<qint64>>::const_iterator bucket = this->username_attempts_.constFind(key);
    const qsizetype used = bucket == this->username_attempts_.cend() ? 0 : bucket.value().size();
    if (this->global_attempts_.size() >= global_max || used >= per_username_max)
        return false;

    this->global_attempts_.append(monotonic_ms);
    this->username_attempts_[key].append(monotonic_ms);
    return true;
}

} // namespace aowis::db
