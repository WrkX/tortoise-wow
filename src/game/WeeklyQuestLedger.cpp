#include "WeeklyQuestLedger.h"

#include "Database/DatabaseEnv.h"
#include "WeeklyResetSchedule.h"
#include "World.h"

uint64 WeeklyQuestLedger::GetCurrentPeriod(time_t now)
{
    WeeklyResetSchedule schedule(
        sWorld.getConfig(CONFIG_UINT32_MAINTENANCE_DAY),
        sWorld.getConfig(CONFIG_UINT32_INSTANCE_RESET_TIME_HOUR));
    return schedule.CurrentPeriodId(now);
}

bool WeeklyQuestLedger::RecordCompletion(uint32 guid, uint32 quest, uint64 period)
{
    if (!guid || !quest || !period)
        return false;

    // Reward persistence spans legacy MyISAM and InnoDB tables, so it cannot
    // be made atomic with this ledger through a database transaction. Reserve
    // the period synchronously before granting anything and keep the
    // reservation if the subsequent character save fails. That can require
    // manual recovery after a database failure, but it cannot duplicate an
    // economy reward.
    return CharacterDatabase.DirectPExecute(
        "INSERT INTO `character_weekly_quest` (`guid`, `quest`, `completed_period`) VALUES (%u, %u, " UI64FMTD ") "
        "ON DUPLICATE KEY UPDATE `completed_period` = IF(`completed_period` <> VALUES(`completed_period`), VALUES(`completed_period`), `completed_period`)",
        guid, quest, uint64(period));
}
