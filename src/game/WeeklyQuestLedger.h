/*
 * Per-character weekly quest completion ledger.
 *
 * The ledger intentionally owns no reset side effects. A period is a
 * deterministic timestamp for the configured weekly boundary, so an offline
 * character naturally becomes eligible after the next boundary.
 */
#pragma once

#include "Common.h"

#include <ctime>

class WeeklyQuestLedger
{
public:
    // Return the start of the period containing now. The calculation uses the
    // same server-local time and existing MaintenanceDay/Instance.ResetTimeHour
    // settings as the other reset configuration, but does not invoke any reset
    // manager.
    static uint64 GetCurrentPeriod(time_t now);

    // Persist the completion synchronously before any reward is granted. This
    // reservation is deliberately fail-closed: callers must not grant the
    // reward unless the write succeeds.
    static bool RecordCompletion(uint32 guid, uint32 quest, uint64 period);
};
