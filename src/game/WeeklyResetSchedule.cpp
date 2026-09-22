#include "WeeklyResetSchedule.h"

namespace
{
    bool LocalTime(time_t timestamp, std::tm& result)
    {
#ifdef _WIN32
        return localtime_s(&result, &timestamp) == 0;
#else
        return localtime_r(&timestamp, &result) != nullptr;
#endif
    }
}

time_t WeeklyResetSchedule::BoundaryForDate(std::tm date) const
{
    date.tm_hour = static_cast<int>(m_resetHour);
    date.tm_min = 0;
    date.tm_sec = 0;
    // Let the C runtime select the correct offset for the configured local
    // date.  This is important on DST transitions where a fixed 7*DAY offset
    // would move the boundary by one hour.
    date.tm_isdst = -1;
    return std::mktime(&date);
}

time_t WeeklyResetSchedule::AddCalendarDays(time_t timestamp, int days)
{
    std::tm date{};
    if (!LocalTime(timestamp, date))
        return 0;

    date.tm_mday += days;
    date.tm_isdst = -1;
    return std::mktime(&date);
}

WeeklyResetSchedule::Period WeeklyResetSchedule::GetPeriod(time_t now) const
{
    Period period;
    if (!IsValid())
        return period;

    std::tm localNow{};
    if (!LocalTime(now, localNow))
        return period;

    // Start with the configured weekday in the local calendar.  mktime
    // normalizes month/year transitions and applies the local DST rules.
    int daysSinceBoundary = (localNow.tm_wday - static_cast<int>(m_resetDay) + 7) % 7;
    localNow.tm_mday -= daysSinceBoundary;
    period.current = BoundaryForDate(localNow);

    // At exactly the boundary, the new period starts immediately.  The
    // comparison also handles a DST normalization that makes a candidate
    // boundary one instant later than the requested local time.
    if (period.current > now)
        period.current = AddCalendarDays(period.current, -7);

    if (period.current == static_cast<time_t>(-1) || period.current == 0)
        return Period{};

    period.previous = AddCalendarDays(period.current, -7);
    period.next = AddCalendarDays(period.current, 7);
    if (period.previous == static_cast<time_t>(-1) || period.previous == 0 ||
        period.next == static_cast<time_t>(-1) || period.next == 0)
        return Period{};

    return period;
}

bool WeeklyResetObserver::Configure(std::uint32_t resetDay, std::uint32_t resetHour)
{
    WeeklyResetSchedule schedule(resetDay, resetHour);
    if (!schedule.IsValid())
    {
        m_configured = false;
        m_observedPeriodId = 0;
        return false;
    }

    // A configuration change defines a new schedule.  Forget only the
    // observer's in-process marker; deterministic eligibility remains based on
    // the period returned by the newly configured schedule.
    if (!m_configured || m_schedule.GetResetDay() != resetDay || m_schedule.GetResetHour() != resetHour)
        m_observedPeriodId = 0;

    m_schedule = schedule;
    m_configured = true;
    return true;
}

bool WeeklyResetObserver::Observe(time_t now, Callback const& callback)
{
    if (!m_configured)
        return false;

    WeeklyResetSchedule::Period const period = m_schedule.GetPeriod(now);
    if (!period.current)
        return false;

    std::uint64_t const periodId = period.Id();
    if (!m_observedPeriodId)
    {
        m_observedPeriodId = periodId;
        if (callback)
            callback(period, true);
        return true;
    }

    if (periodId <= m_observedPeriodId)
        return false;

    m_observedPeriodId = periodId;
    if (callback)
        callback(period, false);
    return true;
}
