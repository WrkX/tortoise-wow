#include "WeeklyResetSchedule.h"

#include "Log.h"
#include "ScriptObjects.h"
#include "World.h"

#include <cstdio>

namespace
{
    char const* BoundaryText(time_t timestamp, std::tm& local, char (&buffer)[32])
    {
#ifdef _WIN32
        localtime_s(&local, &timestamp);
#else
        localtime_r(&timestamp, &local);
#endif
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min, local.tm_sec);
        return buffer;
    }

    class WeeklyResetRuntimeScript final : public WorldScript
    {
    public:
        WeeklyResetRuntimeScript()
            : WorldScript("WeeklyResetRuntimeScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            Configure();
        }

        void OnStartup() override
        {
            m_started = true;
            Observe();
        }

        void OnUpdate(uint32 diff) override
        {
            if (!m_started)
                return;

            if (m_checkTimer > diff)
            {
                m_checkTimer -= diff;
                return;
            }

            m_checkTimer = 60 * IN_MILLISECONDS;
            Observe();
        }

    private:
        void Configure()
        {
            uint32 const resetDay = sWorld.getConfig(CONFIG_UINT32_MAINTENANCE_DAY);
            uint32 const resetHour = sWorld.getConfig(CONFIG_UINT32_INSTANCE_RESET_TIME_HOUR);
            if (!m_observer.Configure(resetDay, resetHour))
            {
                sLog.outError("Weekly quest schedule disabled: reset day %u and hour %u are invalid (expected day 0-6 and hour 0-23).", resetDay, resetHour);
                return;
            }

            sLog.outString("Weekly quest schedule configured for day %u at %02u:00 server local time.", resetDay, resetHour);
        }

        void Observe()
        {
            m_observer.Observe(sWorld.GetGameTime(), [](WeeklyResetSchedule::Period const& period, bool startup)
            {
                char currentText[32] = {};
                char nextText[32] = {};
                std::tm currentLocal{};
                std::tm nextLocal{};
                BoundaryText(period.current, currentLocal, currentText);
                BoundaryText(period.next, nextLocal, nextText);

                sLog.outString("Weekly quest period %llu observed%s (boundary %s; next boundary %s).",
                    static_cast<unsigned long long>(period.Id()), startup ? " at startup" : " after boundary",
                    currentText, nextText);
                // Quest-giver availability is evaluated when a player opens a
                // quest menu in this core; there is no global availability
                // cache to invalidate.  The callback intentionally performs no
                // quest, honor, instance, or offline-character mutation.
            });
        }

        WeeklyResetObserver m_observer;
        uint32 m_checkTimer = 0;
        bool m_started = false;
    };

    WeeklyResetRuntimeScript s_weeklyResetRuntimeScript;
}
