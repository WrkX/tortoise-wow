#include "botpch.h"

#include "QuestGroupFill.h"
#include "QuestGroupFillService.h"

#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // These values are also the mask used by the module's ScriptObjects hook.
    constexpr uint8 ROLE_TANK = 0x01;
    constexpr uint8 ROLE_HEALER = 0x02;
    constexpr uint8 ROLE_DPS = 0x04;

    struct Candidate
    {
        Player* player;
        uint8 roles;
        uint8 assignedRole;
        int score;
    };

    struct PendingShare
    {
        uint32 questId = 0;
        uint32 groupId = 0;
        uint32 requestId = 0;
        uint32 timeout = 0;
        uint32 checkTimer = 0;
        std::vector<ObjectGuid> bots;
    };

    struct PendingActivation
    {
        uint32 questId = 0;
        uint8 size = 0;
        uint32 groupId = 0;
        uint32 requestId = 0;
        uint32 timeout = 0;
        uint32 checkTimer = 0;
        std::vector<uint32> bots;
    };

    uint32 LevelDifference(Player const* first, Player const* second)
    {
        return first->GetLevel() > second->GetLevel()
            ? first->GetLevel() - second->GetLevel()
            : second->GetLevel() - first->GetLevel();
    }

    bool IsNearLeader(Player* leader, Player* bot)
    {
        return leader->GetMapId() == bot->GetMapId() &&
               leader->GetInstanceId() == bot->GetInstanceId() &&
               leader->GetDistance(bot) <= 10.0f;
    }

    bool RegroupBot(Player* leader, Player* bot)
    {
        if (IsNearLeader(leader, bot))
            return true;

        // NearTeleportTo is appropriate on the current map and does not leave
        // a bot waiting for a client teleport acknowledgement. A regular
        // TeleportTo is needed for another map/instance; bot sessions complete
        // that transition through their normal teleport-ack hook.
        if (leader->GetMapId() == bot->GetMapId() &&
            leader->GetInstanceId() == bot->GetInstanceId())
        {
            bot->NearTeleportTo(leader->GetPositionX(), leader->GetPositionY(),
                                leader->GetPositionZ(), leader->GetOrientation());
        }
        else
        {
            bot->TeleportTo(leader->GetMapId(), leader->GetPositionX(),
                            leader->GetPositionY(), leader->GetPositionZ(),
                            leader->GetOrientation());
        }

        return IsNearLeader(leader, bot);
    }

    void ShareQuest(Player* leader, uint32 questId)
    {
        if (!leader || !leader->GetSession() || !leader->GetGroup())
            return;

        WorldPacket packet(CMSG_PUSHQUESTTOPARTY, 4);
        packet << questId;
        leader->GetSession()->HandlePushQuestToParty(packet);
    }

    class QuestGroupFillService final : public QuestGroupFill::Service
    {
    public:
        std::string Start(Player* leader, QuestGroupFill::Request const& request) override
        {
            if (!leader)
                return "Quest group fill failed: player not found.";

            if (Script_IsMachineDriven(leader))
                return "Quest group fill failed: only a real player can lead the group.";

            // A new request supersedes an older regroup/share retry for this
            // leader. Without this, an old quest could be shared after the
            // player explicitly started a different fill.
            if (!m_resumingActivation)
            {
                if (request.requestId)
                {
                    auto share = m_pendingShares.find(leader->GetGUIDLow());
                    auto activation = m_pendingActivations.find(leader->GetGUIDLow());
                    bool const sameShare = share != m_pendingShares.end() && share->second.requestId == request.requestId;
                    bool const sameActivation = activation != m_pendingActivations.end() && activation->second.requestId == request.requestId;
                    if (sameShare || sameActivation)
                        return Remember(leader, "Quest listing fill is already regrouping or activating assistants.");
                    if (share != m_pendingShares.end() || activation != m_pendingActivations.end())
                        return Remember(leader, "Quest listing fill is waiting for another group-fill request to finish.");
                }
                else
                {
                    m_pendingShares.erase(leader->GetGUIDLow());
                    CancelPendingActivation(leader->GetGUIDLow());
                }
            }

            if (!leader->IsAlive() || leader->IsInCombat() || leader->InBattleGround() || leader->InBattleGroundQueue())
                return Remember(leader, "Quest group fill failed: leave combat or battleground activity first.");

            if (request.size < 2 || request.size > 5)
                return "Quest group fill failed: group size must be between 2 and 5.";

            if (request.questId)
            {
                if (!sObjectMgr.GetQuestTemplate(request.questId))
                    return "Quest group fill failed: quest " + std::to_string(request.questId) + " does not exist.";

                QuestStatus questStatus = leader->GetQuestStatus(request.questId);
                if (questStatus == QUEST_STATUS_COMPLETE)
                    return Remember(leader, "Quest group fill complete: you already completed quest " + std::to_string(request.questId) + ".");
                if (questStatus != QUEST_STATUS_INCOMPLETE)
                    return Remember(leader, "Quest group fill failed: you do not have quest " + std::to_string(request.questId) + " active.");
            }

            Group* group = leader->GetGroup();
            if (group && group->isRaidGroup())
                return Remember(leader, "Quest group fill failed: the leader is already in a raid group.");
            if (group && !group->IsLeader(leader->GetObjectGuid()))
                return Remember(leader, "Quest group fill failed: you must be the group leader.");
            if (group && group->GetMembersCount() > request.size)
                return Remember(leader, "Quest group fill failed: the existing group is larger than the requested size.");

            uint32 currentSize = group ? group->GetMembersCount() : 1;
            uint32 needed = request.size - currentSize;
            if (needed == 0)
            {
                if (request.questId)
                    ShareQuest(leader, request.questId);
                return Remember(leader, "Quest group fill complete: the group already has " + std::to_string(currentSize) + " members" +
                    (request.questId ? "; quest sharing was retried." : "."));
            }

            std::vector<Candidate> candidates;
            uint32 levelWindow = request.force ? 10 : 5;
            for (auto const& entry : sObjectAccessor.GetPlayers())
            {
                Player* bot = entry.second;
                if (!bot || bot == leader || !bot->IsInWorld() || !bot->IsAlive())
                    continue;
                if (!Script_IsMachineDriven(bot) || !sRandomPlayerbotMgr.IsFreeBot(bot))
                    continue;
                if (bot->GetGroup() || bot->InBattleGround() || bot->InBattleGroundQueue() || bot->IsInCombat())
                    continue;
                if (bot->GetTeam() != leader->GetTeam())
                    continue;
                if (LevelDifference(leader, bot) > levelWindow)
                    continue;

                // A machine-driven bot can still be an ungrouped alt of a
                // real player. Such a bot is not free for this request.
                if (PlayerbotAI* ai = GetBotAI(bot))
                {
                    if (ai->IsRealPlayer() || ai->HasRealPlayerMaster())
                        continue;
                }

                if (leader->HandleHardcoreInteraction(bot, true) != Player::HardcoreInteractionResult::Allowed)
                    continue;

                uint8 roles = Script_GetAllowedRoles(bot);
                int score = 0;
                if (request.questId && bot->GetQuestStatus(request.questId) == QUEST_STATUS_INCOMPLETE)
                    score += 100; // Quest carriers need no share/accept round trip.
                if (bot->GetMapId() == leader->GetMapId() && bot->GetInstanceId() == leader->GetInstanceId())
                    score += 20;
                if (IsNearLeader(leader, bot))
                    score += 10;
                if (roles & ROLE_TANK)
                    score += 3;
                if (roles & ROLE_HEALER)
                    score += 2;

                candidates.push_back({ bot, roles, 0, score });
            }

            std::sort(candidates.begin(), candidates.end(), [](Candidate const& left, Candidate const& right)
            {
                if (left.score != right.score)
                    return left.score > right.score;
                return left.player->GetObjectGuid() < right.player->GetObjectGuid();
            });

            if (candidates.empty() && request.force)
            {
                uint32 activated = ScheduleOfflineBots(leader, request, needed);
                if (activated)
                    return Remember(leader, "Quest group fill pending: activating " + std::to_string(activated) +
                        " compatible offline bot" + (activated == 1 ? "" : "s") + ".");
            }

            if (candidates.empty())
                return Remember(leader, "Quest group fill partial: no free same-faction, level-compatible bots are online.");

            // Quest groups use soft role targets: a healer from size three and
            // a tank from size four. Existing members count first; unlike the
            // dungeon matcher, a missing role never prevents the group forming.
            bool hasTank = false;
            bool hasHealer = false;
            auto countRoles = [&](Player* member)
            {
                uint8 roles = Script_GetAllowedRoles(member);
                if (!hasTank && (roles & ROLE_TANK))
                    hasTank = true;
                else if (!hasHealer && (roles & ROLE_HEALER))
                    hasHealer = true;
            };

            if (group)
            {
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                    if (Player* member = sObjectMgr.GetPlayer(slot.guid))
                        countRoles(member);
            }
            else
            {
                countRoles(leader);
            }

            std::vector<Candidate> selected;
            auto takeRole = [&](uint8 role)
            {
                auto itr = std::find_if(candidates.begin(), candidates.end(), [role](Candidate const& candidate)
                {
                    return (candidate.roles & role) != 0;
                });
                if (itr == candidates.end())
                    return false;

                itr->assignedRole = role;
                selected.push_back(*itr);
                candidates.erase(itr);
                return true;
            };

            if (request.size >= 3 && !hasHealer && selected.size() < needed)
                hasHealer = takeRole(ROLE_HEALER);
            if (request.size >= 4 && !hasTank && selected.size() < needed)
                hasTank = takeRole(ROLE_TANK);

            while (selected.size() < needed && !candidates.empty())
            {
                Candidate candidate = candidates.front();
                candidates.erase(candidates.begin());
                if (candidate.roles & ROLE_DPS)
                    candidate.assignedRole = ROLE_DPS;
                else if (!hasHealer && (candidate.roles & ROLE_HEALER))
                {
                    candidate.assignedRole = ROLE_HEALER;
                    hasHealer = true;
                }
                else if (!hasTank && (candidate.roles & ROLE_TANK))
                {
                    candidate.assignedRole = ROLE_TANK;
                    hasTank = true;
                }
                selected.push_back(candidate);
            }

            uint32 addCount = selected.size();

            bool const createdGroup = !group;
            if (createdGroup)
            {
                group = new Group;
                if (!group->Create(leader->GetObjectGuid(), leader->GetName()))
                {
                    delete group;
                    return Remember(leader, "Quest group fill failed: could not create a player-led group.");
                }
                sObjectMgr.AddGroup(group);
            }

            uint32 added = 0;
            uint32 shared = 0;
            uint32 pendingTravel = 0;
            std::vector<ObjectGuid> pendingBots;
            for (uint32 index = 0; index < addCount; ++index)
            {
                Candidate const& candidate = selected[index];
                Player* bot = candidate.player;

                bool const nearby = RegroupBot(leader, bot);

                if (candidate.assignedRole)
                    Script_SetForcedRole(bot, candidate.assignedRole);

                if (!group->AddMember(bot->GetObjectGuid(), bot->GetName(), GROUP_JOIN))
                    continue;
                ++added;

                if (!request.questId || bot->GetQuestStatus(request.questId) == QUEST_STATUS_INCOMPLETE)
                    continue;
                if (nearby)
                    ++shared;
                else
                {
                    ++pendingTravel;
                    pendingBots.push_back(bot->GetObjectGuid());
                }
            }

            if (added == 0)
            {
                if (createdGroup)
                {
                    group->Disband();
                    sObjectMgr.RemoveGroup(group);
                    delete group;
                }
                return Remember(leader, "Quest group fill failed: no selected bot could join the group.");
            }

            // Reuse the normal server quest-share path. It validates distance,
            // quest status, quest log capacity, and quest-divider state. We do
            // not grant or inject a quest directly into a bot's quest log.
            if (request.questId && shared)
                ShareQuest(leader, request.questId);

            if (request.questId && !pendingBots.empty())
            {
                PendingShare& pending = m_pendingShares[leader->GetGUIDLow()];
                if (pending.questId != request.questId || pending.groupId != group->GetId() ||
                    pending.requestId != request.requestId)
                    pending = { request.questId, group->GetId(), request.requestId, 30 * IN_MILLISECONDS, 0, {} };
                pending.timeout = 30 * IN_MILLISECONDS;
                for (ObjectGuid const& guid : pendingBots)
                    if (std::find(pending.bots.begin(), pending.bots.end(), guid) == pending.bots.end())
                        pending.bots.push_back(guid);
            }

            uint32 activated = 0;
            if (added < needed && request.force)
                activated = ScheduleOfflineBots(leader, request, needed - added);

            std::ostringstream result;
            std::string const subject = request.questId
                ? " for quest " + std::to_string(request.questId)
                : " for the Quests & Zones listing";
            if (added == needed && pendingTravel == 0)
                result << "Quest group fill complete: added " << added << " bot" << (added == 1 ? "" : "s")
                       << subject << ".";
            else
                result << "Quest group fill partial: added " << added << "/" << needed
                       << " bot" << (added == 1 ? "" : "s") << subject << ".";
            if (shared)
                result << " Quest sharing requested for " << shared << " bot" << (shared == 1 ? "" : "s") << ".";
            if (pendingTravel)
                result << " " << pendingTravel << " bot" << (pendingTravel == 1 ? " is" : "s are") << " regrouping; sharing will be possible when nearby.";
            if (activated)
                result << " Activating " << activated << " compatible offline bot" << (activated == 1 ? "" : "s") << " for the remaining slots.";
            return Remember(leader, result.str());
        }

        void Update(uint32 diff)
        {
            UpdatePendingActivations(diff);

            for (auto itr = m_pendingShares.begin(); itr != m_pendingShares.end();)
            {
                PendingShare& pending = itr->second;
                if (pending.timeout <= diff)
                {
                    m_lastResult[itr->first] = "Quest group fill partial: bot regrouping timed out; group members remain available as assistants.";
                    itr = m_pendingShares.erase(itr);
                    continue;
                }
                pending.timeout -= diff;

                if (pending.checkTimer > diff)
                {
                    pending.checkTimer -= diff;
                    ++itr;
                    continue;
                }
                pending.checkTimer = IN_MILLISECONDS;

                Player* leader = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr->first));
                if (!leader || !leader->IsInWorld() || !leader->GetGroup() ||
                    leader->GetGroup()->GetId() != pending.groupId)
                {
                    itr = m_pendingShares.erase(itr);
                    continue;
                }

                if (leader->GetQuestStatus(pending.questId) != QUEST_STATUS_INCOMPLETE)
                {
                    m_lastResult[itr->first] = "Quest group fill partial: quest sharing stopped because the leader no longer has the quest active.";
                    itr = m_pendingShares.erase(itr);
                    continue;
                }

                // Regrouping may finish while the leader is temporarily busy,
                // but do not inject a quest-share prompt during combat, death,
                // or battleground activity. The normal timeout still bounds
                // how long this retry can remain pending.
                if (!leader->IsAlive() || leader->IsInCombat() ||
                    leader->InBattleGround() || leader->InBattleGroundQueue())
                {
                    ++itr;
                    continue;
                }

                bool ready = true;
                for (ObjectGuid const& botGuid : pending.bots)
                {
                    Player* bot = sObjectMgr.GetPlayer(botGuid);
                    if (!bot || bot->GetGroup() != leader->GetGroup() || !IsNearLeader(leader, bot))
                    {
                        ready = false;
                        break;
                    }
                }

                if (!ready)
                {
                    ++itr;
                    continue;
                }

                ShareQuest(leader, pending.questId);
                m_lastResult[itr->first] = "Quest group fill complete: bots regrouped and normal quest sharing was requested.";
                itr = m_pendingShares.erase(itr);
            }

            CleanupActivatedBots();
        }

        std::string Status(Player* leader) const override
        {
            if (!leader)
                return "Quest group fill status unavailable: player not found.";
            auto itr = m_lastResult.find(leader->GetGUIDLow());
            return itr == m_lastResult.end() ? "Quest group fill is idle." : itr->second;
        }

        std::string Cancel(Player* leader, uint32 requestId = 0) override
        {
            if (!leader)
                return "Quest group fill cancel failed: player not found.";
            bool cancelled = false;
            auto share = m_pendingShares.find(leader->GetGUIDLow());
            if (share != m_pendingShares.end() && (!requestId || share->second.requestId == requestId))
            {
                m_pendingShares.erase(share);
                cancelled = true;
            }
            auto activation = m_pendingActivations.find(leader->GetGUIDLow());
            if (activation != m_pendingActivations.end() && (!requestId || activation->second.requestId == requestId))
            {
                m_pendingActivations.erase(activation);
                cancelled = true;
            }
            if (!cancelled)
                return "Quest group fill is idle; nothing was pending.";
            CleanupActivatedBots();
            return Remember(leader, "Quest group fill pending work cancelled; current group members were kept.");
        }

    private:
        uint32 ScheduleOfflineBots(Player* leader, QuestGroupFill::Request const& request, uint32 missing)
        {
            if (!leader || !missing)
                return 0;

            // Request a few extra logins because cached class data does not
            // reveal the bot's current spec/role. Both per-request and global
            // caps prevent a force command from waking the entire bot stock.
            uint32 const requestCap = std::min<uint32>(12, missing * 3);
            uint32 const globalCap = 40;
            std::vector<uint32> requested;
            uint32 const levelWindow = leader->IsHardcore() ? 5 : 10;
            std::list<uint32> const activeList = sRandomPlayerbotMgr.GetActiveRotationBots();
            std::set<uint32> const activeRotation(activeList.begin(), activeList.end());

            for (auto const& entry : sObjectMgr.GetAllPlayerCacheData())
            {
                if (requested.size() >= requestCap || m_activatedOwners.size() >= globalCap)
                    break;

                PlayerCacheData const* data = entry.second;
                if (!data || !sPlayerbotAIConfig.IsInRandomAccountList(data->uiAccount))
                    continue;
                if (data->uiHardcoreStatus == HARDCORE_MODE_STATUS_DEAD)
                    continue;
                if ((data->uiHardcoreStatus != HARDCORE_MODE_STATUS_NONE) != leader->IsHardcore())
                    continue;
                if (Player::TeamForRace(data->uiRace) != leader->GetTeam())
                    continue;
                uint32 const levelDiff = data->uiLevel > leader->GetLevel()
                    ? data->uiLevel - leader->GetLevel()
                    : leader->GetLevel() - data->uiLevel;
                if (levelDiff > levelWindow)
                    continue;

                uint32 const lowGuid = data->uiGuid;
                if (activeRotation.find(lowGuid) != activeRotation.end() ||
                    m_activatedOwners.find(lowGuid) != m_activatedOwners.end() ||
                    sRandomPlayerbotMgr.IsExternallyManaged(lowGuid) ||
                    sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, lowGuid), false))
                    continue;

                sRandomPlayerbotMgr.SetExternallyManaged(lowGuid, true);
                if (!sRandomPlayerbotMgr.AddRandomBot(lowGuid))
                {
                    sRandomPlayerbotMgr.SetExternallyManaged(lowGuid, false);
                    continue;
                }

                m_activatedOwners[lowGuid] = leader->GetGUIDLow();
                requested.push_back(lowGuid);
            }

            if (!requested.empty())
            {
                PendingActivation pending = {
                    request.questId,
                    request.size,
                    leader->GetGroup() ? leader->GetGroup()->GetId() : 0,
                    request.requestId,
                    45 * IN_MILLISECONDS,
                    0,
                    requested
                };
                m_pendingActivations[leader->GetGUIDLow()] = pending;
            }

            return requested.size();
        }

        bool CancelPendingActivation(uint32 leaderGuid)
        {
            auto itr = m_pendingActivations.find(leaderGuid);
            if (itr == m_pendingActivations.end())
                return false;
            m_pendingActivations.erase(itr);
            return true;
        }

        bool IsActivationPending(uint32 leaderGuid, uint32 botGuid) const
        {
            auto itr = m_pendingActivations.find(leaderGuid);
            return itr != m_pendingActivations.end() &&
                std::find(itr->second.bots.begin(), itr->second.bots.end(), botGuid) != itr->second.bots.end();
        }

        void UpdatePendingActivations(uint32 diff)
        {
            for (auto itr = m_pendingActivations.begin(); itr != m_pendingActivations.end();)
            {
                uint32 const leaderGuid = itr->first;
                PendingActivation pending = itr->second;

                if (pending.timeout <= diff)
                {
                    m_lastResult[leaderGuid] = "Quest group fill partial: offline bot activation timed out.";
                    itr = m_pendingActivations.erase(itr);
                    continue;
                }
                itr->second.timeout -= diff;

                if (itr->second.checkTimer > diff)
                {
                    itr->second.checkTimer -= diff;
                    ++itr;
                    continue;
                }
                itr->second.checkTimer = IN_MILLISECONDS;

                Player* leader = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, leaderGuid));
                if (!leader || !leader->IsInWorld() ||
                    (pending.questId && leader->GetQuestStatus(pending.questId) != QUEST_STATUS_INCOMPLETE) ||
                    (pending.groupId && (!leader->GetGroup() || leader->GetGroup()->GetId() != pending.groupId)) ||
                    (!pending.groupId && leader->GetGroup()) ||
                    (leader->GetGroup() && !leader->GetGroup()->IsLeader(leader->GetObjectGuid())))
                {
                    itr = m_pendingActivations.erase(itr);
                    continue;
                }

                if (!leader->IsAlive() || leader->IsInCombat() ||
                    leader->InBattleGround() || leader->InBattleGroundQueue())
                {
                    ++itr;
                    continue;
                }

                bool anyReady = false;
                for (uint32 const botGuid : pending.bots)
                {
                    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
                    if (bot && bot->IsInWorld())
                    {
                        anyReady = true;
                        break;
                    }
                }

                if (!anyReady)
                {
                    ++itr;
                    continue;
                }

                uint32 const remainingTimeout = itr->second.timeout;
                itr = m_pendingActivations.erase(itr);

                m_resumingActivation = true;
                Start(leader, { pending.questId, pending.size, false, pending.requestId });
                m_resumingActivation = false;

                uint32 const groupSize = leader->GetGroup() ? leader->GetGroup()->GetMembersCount() : 1;
                std::vector<uint32> stillPending;
                if (groupSize < pending.size)
                {
                    for (uint32 const botGuid : pending.bots)
                    {
                        Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
                        if (!bot || !bot->GetGroup())
                            stillPending.push_back(botGuid);
                    }
                }

                if (!stillPending.empty() && remainingTimeout)
                {
                    pending.timeout = remainingTimeout;
                    pending.checkTimer = IN_MILLISECONDS;
                    pending.bots = stillPending;
                    pending.groupId = leader->GetGroup() ? leader->GetGroup()->GetId() : 0;
                    m_pendingActivations[leaderGuid] = pending;
                    m_lastResult[leaderGuid] += " Waiting for " + std::to_string(stillPending.size()) + " offline activation(s).";
                }
            }
        }

        void CleanupActivatedBots()
        {
            for (auto itr = m_activatedOwners.begin(); itr != m_activatedOwners.end();)
            {
                uint32 const botGuid = itr->first;
                uint32 const leaderGuid = itr->second;
                Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, botGuid));
                Player* leader = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, leaderGuid));

                bool const groupedWithOwner = bot && leader && bot->GetGroup() &&
                    bot->GetGroup() == leader->GetGroup();
                if (groupedWithOwner || IsActivationPending(leaderGuid, botGuid))
                {
                    ++itr;
                    continue;
                }

                // There is no safe cancellation for a SQL login already in
                // flight. Keep the reservation until the callback either
                // materializes the bot or OnPlayerLoginError clears it.
                if (!bot)
                {
                    if (sRandomPlayerbotMgr.IsExternallyManaged(botGuid))
                    {
                        ++itr;
                        continue;
                    }
                    itr = m_activatedOwners.erase(itr);
                    continue;
                }

                // If another group claimed the bot after activation, stop
                // owning it but leave that group untouched.
                if (bot->GetGroup())
                {
                    sRandomPlayerbotMgr.SetExternallyManaged(botGuid, false);
                    itr = m_activatedOwners.erase(itr);
                    continue;
                }

                if (bot->IsInCombat() || !sRandomPlayerbotMgr.ReleaseQuestFillBot(botGuid))
                {
                    ++itr;
                    continue;
                }
                itr = m_activatedOwners.erase(itr);
            }
        }

        std::string Remember(Player* leader, std::string result)
        {
            m_lastResult[leader->GetGUIDLow()] = result;
            return result;
        }

        std::map<uint32, std::string> m_lastResult;
        std::map<uint32, PendingShare> m_pendingShares;
        std::map<uint32, PendingActivation> m_pendingActivations;
        std::map<uint32, uint32> m_activatedOwners;
        bool m_resumingActivation = false;
    };

    QuestGroupFillService& GetQuestGroupFillService()
    {
        static QuestGroupFillService service;
        return service;
    }
}

void RegisterQuestGroupFillService()
{
    QuestGroupFill::RegisterService(&GetQuestGroupFillService());
}

void UpdateQuestGroupFillService(uint32 diff)
{
    GetQuestGroupFillService().Update(diff);
}
