/*
 * Copyright (C) 2026 Turtle WoW
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef MANGOS_WEEKLY_QUEST_CONTENT_H
#define MANGOS_WEEKLY_QUEST_CONTENT_H

#include "Common.h"

namespace WeeklyQuestContent
{
// ReqCreatureOrGOId fields are signed MEDIUMINTs, so objective credit IDs
// must remain at or below 8,388,607.
constexpr uint32 BattlegroundMatchCredit = 8000100;
constexpr uint32 RaidBossCredit = 8000101;
}

#endif
