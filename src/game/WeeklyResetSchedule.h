/*
 * Copyright (C) 2026 Turtle WoW
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef MANGOS_WEEKLY_RESET_SCHEDULE_H
#define MANGOS_WEEKLY_RESET_SCHEDULE_H

#include <cstdint>
#include <ctime>
#include <functional>

/// Calendar schedule for a weekly boundary in the server's local time zone.
///
/// resetDay uses the same convention as tm::tm_wday (Sunday == 0).  The
/// schedule deliberately has no dependency on instance-reset state, rates, or
/// database state: a period is identified solely by its boundary timestamp.
class WeeklyResetSchedule
{
public:
    struct Period
    {
        time_t current = 0;
        time_t previous = 0;
        time_t next = 0;

        std::uint64_t Id() const { return static_cast<std::uint64_t>(current); }
    };

    WeeklyResetSchedule() = default;
    WeeklyResetSchedule(std::uint32_t resetDay, std::uint32_t resetHour)
        : m_resetDay(resetDay), m_resetHour(resetHour) {}

    bool IsValid() const { return m_resetDay < 7 && m_resetHour < 24; }
    std::uint32_t GetResetDay() const { return m_resetDay; }
    std::uint32_t GetResetHour() const { return m_resetHour; }

    /// Return the [current, next) period containing now, or an empty period
    /// when the configured day/hour is invalid.
    Period GetPeriod(time_t now) const;

    time_t CurrentBoundary(time_t now) const { return GetPeriod(now).current; }
    time_t PreviousBoundary(time_t now) const { return GetPeriod(now).previous; }
    time_t NextBoundary(time_t now) const { return GetPeriod(now).next; }
    std::uint64_t CurrentPeriodId(time_t now) const { return GetPeriod(now).Id(); }

private:
    time_t BoundaryForDate(std::tm date) const;
    static time_t AddCalendarDays(time_t timestamp, int days);

    std::uint32_t m_resetDay = 0;
    std::uint32_t m_resetHour = 0;
};

/// Idempotent in-process boundary detector.  The callback runs at most once
/// for a period while this observer is alive.  On startup it receives the
/// current period once, which gives callers a safe catch-up/logging point
/// after downtime without replaying each missed week.
class WeeklyResetObserver
{
public:
    using Callback = std::function<void(WeeklyResetSchedule::Period const&, bool startup)>;

    WeeklyResetObserver() = default;

    bool Configure(std::uint32_t resetDay, std::uint32_t resetHour);
    bool IsConfigured() const { return m_configured; }
    WeeklyResetSchedule const& GetSchedule() const { return m_schedule; }
    std::uint64_t GetObservedPeriodId() const { return m_observedPeriodId; }

    /// Observe now and invoke callback for startup or a newly crossed period.
    /// Clock rollback is ignored until the clock reaches a newer period; this
    /// prevents duplicate boundary handling after a wall-clock correction.
    bool Observe(time_t now, Callback const& callback);

private:
    WeeklyResetSchedule m_schedule;
    std::uint64_t m_observedPeriodId = 0;
    bool m_configured = false;
};

#endif
