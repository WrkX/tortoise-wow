#include "scriptPCH.h"
#include "Database/DatabaseEnv.h"

#include <limits.h>

namespace
{
    constexpr uint32 ITEM_MARK_ONE = 1985500;
    constexpr uint32 ITEM_MARK_TEN = 1985501;
    constexpr uint32 ITEM_MARK_HUNDRED = 1985502;

    constexpr uint32 POINTS_ONE = 1;
    constexpr uint32 POINTS_TEN = 10;
    constexpr uint32 POINTS_HUNDRED = 100;

    uint32 GetDonationPoints(uint32 itemEntry)
    {
        switch (itemEntry)
        {
            case ITEM_MARK_ONE:
                return POINTS_ONE;
            case ITEM_MARK_TEN:
                return POINTS_TEN;
            case ITEM_MARK_HUNDRED:
                return POINTS_HUNDRED;
            default:
                return 0;
        }
    }

    bool CanCreditDonationPoints(uint32 accountId, uint32 points)
    {
        QueryResult* result = LoginDatabase.PQuery(
            "SELECT `coins` FROM `shop_coins` WHERE `id` = %u", accountId);

        if (!result)
            return true;

        Field* fields = result->Fetch();
        int32 currentCoins = fields[0].GetInt32();
        delete result;

        return currentCoins >= 0 && static_cast<int64>(currentCoins) + points <= INT_MAX;
    }

    bool CreditDonationPoints(uint32 accountId, uint32 points)
    {
        return LoginDatabase.DirectPExecute(
            "INSERT INTO `shop_coins` (`id`, `coins`) VALUES (%u, %u) "
            "ON DUPLICATE KEY UPDATE `coins` = `coins` + %u",
            accountId, points, points);
    }
}

bool ItemUse_item_donation_mark(Player* player, Item* item, SpellCastTargets& /*targets*/)
{
    if (!player || !item)
        return true;

    uint32 points = GetDonationPoints(item->GetEntry());
    if (!points)
        return true;

    uint32 accountId = player->GetSession()->GetAccountId();
    if (!accountId || !CanCreditDonationPoints(accountId, points))
    {
        player->GetSession()->SendNotification("Your Mark of the Ranger General balance cannot hold this redemption.");
        return true;
    }

    if (!CreditDonationPoints(accountId, points))
    {
        sLog.outError("[DONATION] Failed to credit %u points to account %u for item %u.",
            points, accountId, item->GetEntry());
        player->GetSession()->SendNotification("The mark could not be redeemed. Please try again later.");
        return true;
    }

    uint32 count = 1;
    player->DestroyItemCount(item, count, true);
    player->SaveInventoryAndGoldToDB();

    sLog.outString("[Marks of the Ranger General] Player %u on account %u redeemed item %u for %u points.",
        player->GetGUIDLow(), accountId, item->GetEntry(), points);
    player->GetSession()->SendNotification("You redeemed %u Mark(s) of the Ranger General.", points);

    return true;
}

void AddSC_item_donation_mark()
{
    Script* newscript = new Script;
    newscript->Name = "item_donation_mark";
    newscript->pItemUse = &ItemUse_item_donation_mark;
    newscript->RegisterSelf();
}
