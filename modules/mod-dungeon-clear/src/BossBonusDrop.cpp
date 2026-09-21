/*
 * Extra 1985500 drop on dungeon/raid bosses.
 *
 * Boss list comes from DungeonClear's roster rather than elite rank, so
 * custom Turtle bosses and event-spawned ones count too.
 */

#include "ScriptMgr.h"
#include "PlayerScript.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "Util.h"

#include "Ai/Dungeon/DungeonClear/Data/BossSpawnIndex.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

#include <cmath>
#include <string>
#include <vector>

namespace
{
    constexpr uint32 BOSS_BONUS_ITEM = 1985500;
    constexpr uint32 DUNGEON_BONUS_CHANCE = 10;
    constexpr uint32 RAID_BONUS_CHANCE = 50;
    constexpr float DUNGEON_BOSS_LEVEL_RANGE = 5.0f;

    bool MatchesBossEntry(Creature const* creature, uint32 entry)
    {
        if (!creature)
            return false;

        return creature->GetEntry() == entry || creature->GetOriginalEntry() == entry;
    }

    bool IsRecognizedBoss(Creature const* creature, Map* map)
    {
        if (!creature || !map)
            return false;

        Difficulty const difficulty = map->GetDifficulty();
        std::vector<DungeonBossInfo> definitions;
        auto const& base = BossSpawnIndex::Get(map->GetId(), difficulty);
        if (BossRosterRegistry::HasPatch(map->GetId()))
            definitions = BossRosterRegistry::Apply(map->GetId(), difficulty,
                                                      std::vector<DungeonBossInfo>(base.begin(), base.end()));
        else
            definitions.assign(base.begin(), base.end());

        for (DungeonBossInfo const& definition : definitions)
        {
            if (definition.kind == DungeonAnchorKind::Boss &&
                MatchesBossEntry(creature, definition.entry))
                return true;
        }

        // Roster doesn't cover every Turtle instance. boss_* scripts count as
        // bosses; Scarlet Citadel / Emerald Sanctum are the two that don't
        // follow that naming.
        std::string const scriptName = creature->GetScriptName();
        if (scriptName.rfind("boss_", 0) == 0)
            return true;

        uint32 const mapId = map->GetId();
        uint32 const entry = creature->GetEntry();
        return (mapId == 45 && entry == 2000004) || // Scarlet Citadel: Eric Vesper
               (mapId == 807 && (entry == 60747 || entry == 60748)); // Emerald Sanctum
    }

    void AddEligiblePlayer(Player* player, Creature* boss, std::vector<Player*>& eligible)
    {
        // Same rules as Group::GetDataForXPAtKill.
        if (!player || !player->IsAlive() || !player->IsInWorld() ||
            !player->IsAtGroupRewardDistance(boss))
            return;

        for (Player* existing : eligible)
            if (existing->GetObjectGuid() == player->GetObjectGuid())
                return;

        eligible.push_back(player);
    }

    std::vector<Player*> GetEligiblePlayers(Player* source, Creature* boss)
    {
        std::vector<Player*> eligible;

        if (Group* group = source ? source->GetGroup() : nullptr)
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                AddEligiblePlayer(itr->getSource(), boss, eligible);
        }

        // Solo, or if the tapper isn't in the group list.
        AddEligiblePlayer(source, boss, eligible);
        return eligible;
    }

    bool IsDungeonLevelEligible(Creature const* boss, std::vector<Player*> const& eligible)
    {
        if (!boss || eligible.empty())
            return false;

        uint32 levelSum = 0;
        for (Player const* player : eligible)
            levelSum += player->GetLevel();

        float const averageLevel = static_cast<float>(levelSum) /
                                    static_cast<float>(eligible.size());
        float const levelDifference = std::fabs(averageLevel - static_cast<float>(boss->GetLevel()));
        return levelDifference <= DUNGEON_BOSS_LEVEL_RANGE;
    }

    void TryAwardBonus(Player* player, Creature* boss)
    {
        if (!player || !boss)
            return;

        Map* map = boss->FindMap();
        if (!map || !map->IsDungeon() || !IsRecognizedBoss(boss, map))
            return;

        std::vector<Player*> const eligible = GetEligiblePlayers(player, boss);
        if (eligible.empty())
        {
            sLog.outString("[BossBonusDrop] map=%u entry=%u: no eligible players", map->GetId(), boss->GetEntry());
            return;
        }

        uint32 chance = RAID_BONUS_CHANCE;
        if (!map->IsRaid())
        {
            if (!IsDungeonLevelEligible(boss, eligible))
            {
                sLog.outString("[BossBonusDrop] map=%u entry=%u: dungeon level check failed for %u eligible players",
                               map->GetId(), boss->GetEntry(), uint32(eligible.size()));
                return;
            }
            chance = DUNGEON_BONUS_CHANCE;
        }

        // OnCreatureKill fires once per credited player. Lock the roll only
        // after the kill has a valid eligible-player set and has passed the
        // dungeon level check, so an invalid first callback cannot consume it.
        if (!boss->loot.TryMarkBossBonusDropRolled())
            return;

        if (!roll_chance_u(chance))
        {
            sLog.outString("[BossBonusDrop] map=%u entry=%u: roll missed (%u%%, eligible=%u, raid=%u)",
                           map->GetId(), boss->GetEntry(), chance, uint32(eligible.size()), map->IsRaid() ? 1 : 0);
            return;
        }

        if (boss->loot.AddFFAItem(BOSS_BONUS_ITEM, 1, eligible))
        {
            boss->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
            sLog.outString("[BossBonusDrop] map=%u entry=%u: roll succeeded; item attached for %u eligible players",
                           map->GetId(), boss->GetEntry(), uint32(eligible.size()));
        }
        else
            sLog.outError("[BossBonusDrop] map=%u entry=%u: roll succeeded but item could not be attached (loot slots=%u)",
                          map->GetId(), boss->GetEntry(), uint32(boss->loot.items.size()));
    }

    class BossBonusDropPlayerScript : public PlayerScript
    {
    public:
        BossBonusDropPlayerScript()
            : PlayerScript("BossBonusDropPlayerScript", {
                PLAYERHOOK_ON_CREATURE_KILL
            }) {}

        void OnCreatureKill(Player* player, Creature* creature) override
        {
            TryAwardBonus(player, creature);
        }
    };
}

void AddSC_dungeon_boss_bonus_drop()
{
    new BossBonusDropPlayerScript();
}
