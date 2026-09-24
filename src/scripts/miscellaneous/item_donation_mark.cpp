#include "scriptPCH.h"
#include "Database/DatabaseEnv.h"
#include "Spells/Spell.h"
#include "Spells/SpellEntry.h"
#include "Spells/SpellMgr.h"

#include <array>
#include <cstddef>
#include <limits.h>
#include <memory>
#include <utility>

namespace
{
    constexpr uint32 ITEM_MARK_ONE = 1985500;
    constexpr uint32 ITEM_MARK_TEN = 1985501;
    constexpr uint32 ITEM_MARK_HUNDRED = 1985502;
    constexpr uint32 SPELL_MARK_ONE = 61003;
    constexpr uint32 SPELL_MARK_TEN = 61004;
    constexpr uint32 SPELL_MARK_HUNDRED = 61005;
    constexpr uint32 SPELL_CAST_TIME_TEMPLATE = 2050; // Lesser Heal: 1.5 seconds
    constexpr char SPELL_SCRIPT_NAME[] = "item_donation_mark";

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

    bool IsDonationMark(Item const* item)
    {
        return item && GetDonationPoints(item->GetEntry()) != 0;
    }

    void RedeemDonationMark(Player* player, Item* item)
    {
        if (!player || !IsDonationMark(item))
            return;

        uint32 points = GetDonationPoints(item->GetEntry());
        uint32 accountId = player->GetSession()->GetAccountId();
        if (!accountId || !CanCreditDonationPoints(accountId, points))
        {
            player->GetSession()->SendNotification("Your Donation Point balance cannot hold this redemption.");
            return;
        }

        if (!CreditDonationPoints(accountId, points))
        {
            sLog.outError("[DONATION] Failed to credit %u points to account %u for item %u.",
                points, accountId, item->GetEntry());
            player->GetSession()->SendNotification("Donation Point credit failed. Your mark was not consumed; please contact a game master.");
            return;
        }

        uint32 count = 1;
        player->DestroyItemCount(item, count, true);
        player->SaveInventoryAndGoldToDB();

        sLog.outString("[Marks of the Ranger General] Player %u on account %u redeemed item %u for %u points.",
            player->GetGUIDLow(), accountId, item->GetEntry(), points);
        player->GetSession()->SendNotification("You redeemed a Mark of the Ranger General for %u Donation Point%s.",
            points, points == 1 ? "" : "s");
    }

    class DonationMarkSpellScript : public SpellScript
    {
    public:
        void OnFinish(Spell* spell, bool ok) const override
        {
            if (ok || !spell || !IsDonationMark(spell->GetCastItem()))
                return;

            if (Player* player = spell->GetCaster()->ToPlayer())
                player->GetSession()->SendNotification("Mark redemption did not finish; the mark was not consumed.");
        }

        void OnSuccessfulFinish(Spell* spell) const override
        {
            if (!spell)
                return;

            Player* player = spell->GetCaster()->ToPlayer();
            RedeemDonationMark(player, spell->GetCastItem());
        }
    };

    SpellScript* GetDonationMarkSpellScript(SpellEntry const*)
    {
        return new DonationMarkSpellScript();
    }

    SpellEntry const* GetDonationMarkRedemptionSpell(uint32 itemEntry)
    {
        uint32 spellId = 0;
        std::size_t spellIndex = 0;
        switch (itemEntry)
        {
            case ITEM_MARK_ONE:
                spellId = SPELL_MARK_ONE;
                spellIndex = 0;
                break;
            case ITEM_MARK_TEN:
                spellId = SPELL_MARK_TEN;
                spellIndex = 1;
                break;
            case ITEM_MARK_HUNDRED:
                spellId = SPELL_MARK_HUNDRED;
                spellIndex = 2;
                break;
            default:
                return nullptr;
        }

        static std::array<std::unique_ptr<SpellEntry>, 3> redemptionSpells = []()
        {
            std::array<std::unique_ptr<SpellEntry>, 3> spells;
            SpellEntry const* castTimeTemplate = sSpellMgr.GetSpellEntry(SPELL_CAST_TIME_TEMPLATE);
            uint32 scriptId = sScriptMgr.GetScriptId(SPELL_SCRIPT_NAME);
            if (!castTimeTemplate || !scriptId)
                return spells;

            constexpr uint32 spellIds[] = { SPELL_MARK_ONE, SPELL_MARK_TEN, SPELL_MARK_HUNDRED };
            for (std::size_t i = 0; i < spells.size(); ++i)
            {
                SpellEntry const* customSpell = sSpellMgr.GetSpellEntry(spellIds[i]);
                if (!customSpell)
                    continue;

                // Use the known-good self-targeted cast setup from Lesser Heal,
                // but keep each custom DBC id so the client uses that spell's
                // name and visual. The only executed effect is the dummy below.
                std::unique_ptr<SpellEntry> spell = std::make_unique<SpellEntry>(*castTimeTemplate);
                spell->Id = customSpell->Id;
                spell->CastingTimeIndex = castTimeTemplate->CastingTimeIndex;
                spell->SpellVisual = customSpell->SpellVisual;
                spell->SpellIconID = customSpell->SpellIconID;
                spell->activeIconID = customSpell->activeIconID;
                spell->SpellName.fill("Redeeming Points");
                spell->Rank.fill("");
                spell->SpellFamilyName = SPELLFAMILY_GENERIC;
                spell->SpellFamilyFlags = 0;
                spell->DmgClass = SPELL_DAMAGE_CLASS_NONE;
                spell->ScriptId = scriptId;
                spell->RecoveryTime = 0;
                spell->CategoryRecoveryTime = 0;
                spell->StartRecoveryCategory = 0;
                spell->StartRecoveryTime = 0;

                for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    spell->Effect[i] = SPELL_EFFECT_NONE;
                    spell->EffectDieSides[i] = 0;
                    spell->EffectBaseDice[i] = 0;
                    spell->EffectDicePerLevel[i] = 0.0f;
                    spell->EffectRealPointsPerLevel[i] = 0.0f;
                    spell->EffectBasePoints[i] = 0;
                    spell->EffectBonusCoefficient[i] = 0.0f;
                    spell->EffectMechanic[i] = 0;
                    spell->EffectImplicitTargetA[i] = 0;
                    spell->EffectImplicitTargetB[i] = 0;
                    spell->EffectRadiusIndex[i] = 0;
                    spell->EffectApplyAuraName[i] = 0;
                    spell->EffectAmplitude[i] = 0;
                    spell->EffectMultipleValue[i] = 0.0f;
                    spell->EffectChainTarget[i] = 0;
                    spell->EffectItemType[i] = 0;
                    spell->EffectMiscValue[i] = 0;
                    spell->EffectTriggerSpell[i] = 0;
                    spell->EffectPointsPerComboPoint[i] = 0.0f;
                }

                spell->Effect[0] = SPELL_EFFECT_DUMMY;
                spell->EffectImplicitTargetA[0] = TARGET_UNIT_CASTER;
                spell->InitCachedValues();
                spells[i] = std::move(spell);
            }

            return spells;
        }();

        return redemptionSpells[spellIndex].get();
    }
}

bool ItemUse_item_donation_mark(Player* player, Item* item, SpellCastTargets& /*targets*/)
{
    if (!player || !item)
        return true;

    if (!IsDonationMark(item))
        return true;

    uint32 accountId = player->GetSession()->GetAccountId();
    if (!accountId || !CanCreditDonationPoints(accountId, GetDonationPoints(item->GetEntry())))
    {
        player->GetSession()->SendNotification("Your Donation Point balance cannot hold this redemption.");
        return true;
    }

    SpellEntry const* spell = GetDonationMarkRedemptionSpell(item->GetEntry());
    if (!spell)
    {
        sLog.outError("[DONATION] Redemption spell for item %u or script '%s' is unavailable.",
            item->GetEntry(), SPELL_SCRIPT_NAME);
        player->GetSession()->SendNotification("Mark redemption is unavailable right now. Please contact a game master.");
        return true;
    }

    if (player->CastSpell(player, spell, false, item) == SPELL_CAST_OK)
        player->GetSession()->SendNotification("Redeeming Points...");

    return true;
}

void AddSC_item_donation_mark()
{
    Script* newscript = new Script;
    newscript->Name = "item_donation_mark";
    newscript->pItemUse = &ItemUse_item_donation_mark;
    newscript->GetSpellScript = &GetDonationMarkSpellScript;
    newscript->RegisterSelf();
}
