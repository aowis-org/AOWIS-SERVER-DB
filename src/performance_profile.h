#ifndef AOWIS_DB_PERFORMANCE_PROFILE_H
#define AOWIS_DB_PERFORMANCE_PROFILE_H

#include <QElapsedTimer>
#include <QtGlobal>

#include <cstdio>

namespace aowis::db::performance_profile
{

// Development-only, opt-in diagnostics. Nothing is retained in the database
// or exposed through REST, and normal operations do not emit profiling data.
// Durations are monotonic microseconds, not wall-clock timestamps.
class PhaseTimer final
{
public:
    explicit PhaseTimer(const char *operation)
        : operation_(operation), enabled_(qEnvironmentVariableIntValue("AOWIS_DB_PROFILE_PHASES") == 1)
    {
        if (this->enabled_)
            this->timer_.start();
    }

    void mark(const char *phase, qsizetype count = -1)
    {
        if (!this->enabled_)
            return;
        const qint64 microseconds = this->timer_.nsecsElapsed() / 1000;
        if (count >= 0)
            std::fprintf(stderr, "aowis_db_profile op=%s phase=%s elapsed_us=%lld count=%lld\n",
                         this->operation_, phase, static_cast<long long>(microseconds),
                         static_cast<long long>(count));
        else
            std::fprintf(stderr, "aowis_db_profile op=%s phase=%s elapsed_us=%lld\n",
                         this->operation_, phase, static_cast<long long>(microseconds));
        this->timer_.restart();
    }

private:
    const char *operation_;
    bool enabled_ = false;
    QElapsedTimer timer_;
};

} // namespace aowis::db::performance_profile

#endif // AOWIS_DB_PERFORMANCE_PROFILE_H
