// Host-side glue for bot lifecycle and startup. The bot objects live in the
// Player slots claimed by this module (ModuleSlots.h), so the core does not
// allocate them or need their concrete types.

#include "playerbot/playerbot.h"
#include "Objects/Player.h"
#include "World.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotAiExtension.h"
#include "Chat/Chat.h"

void CreateBotAI(Player* player)
{
    if (!player || GetBotAI(player))
        return;

    SetBotAI(player, new PlayerbotAI(player));
}

void RemoveBotAI(Player* player)
{
    PlayerbotAI* ai = GetBotAI(player);
    if (!ai)
        return;

    delete ai;
    SetBotAI(player, nullptr);
}

void CreateBotMgr(Player* player)
{
    if (!player || GetBotMgr(player))
        return;

    SetBotMgr(player, new PlayerbotMgr(player));
    // RandomPlayerbotMgr tracks real players for level sync, random-bot login,
    // and LFG auto-queue behavior.
    sRandomPlayerbotMgr.OnPlayerLogin(player);
}

void RemoveBotMgr(Player* player)
{
    PlayerbotMgr* mgr = GetBotMgr(player);
    if (!mgr)
        return;

    // Log out the master's alts before destroying their manager so no bot AI
    // can retain a dangling master reference.
    mgr->LogoutAllBots();
    sRandomPlayerbotMgr.OnPlayerLogout(player);
    delete mgr;
    SetBotMgr(player, nullptr);
}

bool IsRealPlayer(Player const* player)
{
    PlayerbotAI* ai = GetBotAI(player);
    return !ai || ai->IsRealPlayer();
}

void AddSC_playerbot_hooks();

void World::InitPlayerbotsAtStartup()
{
    sPlayerbotAIConfig.Initialize();
    sPlayerbotAiExtension.RunStartupHooks();

    // Register after ScriptRegistry's containers exist. Subsequent bot/core
    // integration is delivered through PlayerbotScripts.cpp hooks.
    AddSC_playerbot_hooks();
}

bool ChatHandler::HandleDungeonClearCommand(char* args)
{
    return sPlayerbotAiExtension.HandleDcCommand(this, args);
}
