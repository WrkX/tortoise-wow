
#include "Category.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>
#include "ItemBag.h"
#include "ahbot/AhBot.h"
#include "AhBotEconomy.h"
#include "World.h"
#include "Config/Config.h"
#include "Chat/Chat.h"
#include "AhBotConfig.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "WorldSession.h"
#include "Objects/Player.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "AccountMgr.h"
#include "playerbot/playerbot.h"
#include "Mail/Mail.h"
#include "Util.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <thread>

using namespace ahbot;

bool AhBot::HandleAhBotCommand(ChatHandler* handler, char const* args)
{
    auctionbot.HandleCommand(args ? args : "", handler);
    return true;
}

uint32 AhBot::auctionIds[MAX_AUCTIONS] = {1,6,7};
uint32 AhBot::auctioneers[MAX_AUCTIONS] = {79707,4656,23442};
std::map<uint32, uint32> AhBot::factions;

void AhBot::Init()
{
    sLog.outString("[AhBot] Initializing AhBot by ike3");

    if (!sAhBotConfig.Initialize())
    {
        sLog.outString("[AhBot] Disabled or failed to load ahbot.conf — AhBot will not run");
        return;
    }

    sLog.outString("[AhBot] Config: GUID=%llu, updateInterval=%ds, maxItemLevel=%d, maxRequiredLevel=%d, priceMultiplier=%.2f",
        (unsigned long long)sAhBotConfig.guid,
        sAhBotConfig.updateInterval,
        sAhBotConfig.maxItemLevel,
        sAhBotConfig.maxRequiredLevel,
        sAhBotConfig.priceMultiplier);

    factions[1] = 1;
    factions[2] = 1;
    factions[3] = 1;
    factions[4] = 2;
    factions[5] = 2;
    factions[6] = 2;
    factions[7] = 3;

    availableItems.Init();

    sLog.outString("[AhBot] Initialization complete. First auction check in %d seconds.", sAhBotConfig.updateInterval);
}

AhBot::~AhBot()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> g(workerMutex);
        worker.swap(workerThread);
    }
    if (worker.joinable())
        worker.join();
}

ObjectGuid AhBot::GetAHBplayerGUID()
{
    return ObjectGuid(sAhBotConfig.guid);
}

#ifdef MANGOS
class AhbotThread: public ACE_Task <ACE_MT_SYNCH>
{
public:
    int svc(void) { auctionbot.ForceUpdate(); return 0; }
};
#endif

void AhBot::StartWorker()
{
    if (sWorld.IsShutdowning() || sWorld.IsStopped())
        return;

#ifdef MANGOS
    AhbotThread *thread = new AhbotThread();
    thread->activate();
    return;
#endif

    std::lock_guard<std::mutex> g(workerMutex);
    if (workerThread.joinable())
    {
        if (updating.load())
            return;
        workerThread.join();
    }

    workerThread = std::thread([]()
    {
        CharacterDatabase.ThreadStart();
        WorldDatabase.ThreadStart();
        auctionbot.ForceUpdate();
        CharacterDatabase.ThreadEnd();
        WorldDatabase.ThreadEnd();
    });
}

void AhBot::Update()
{
    if (sWorld.IsShutdowning())
        return;

    if (sWorld.IsStopped())
        return;

    // Carry out whatever the bot thread decided on last pass. This has to come
    // before the nextAICheckTime early-out below, or queued work would only run
    // once every check interval instead of on the next tick.
    RunQueuedWork();

    time_t now = time(0);

    if (now < nextAICheckTime)
        return;

    if (updating)
    {
        sLog.outString("[AhBot] Update skipped — previous check still running");
        return;
    }

    sLog.outString("[AhBot] Scheduling auction check (next after this: in %d seconds)", sAhBotConfig.updateInterval);
    nextAICheckTime = time(0) + sAhBotConfig.updateInterval;
    StartWorker();
    CleanupPropositions();
}

void AhBot::RequestUpdate(bool simulate)
{
    pendingSimulate.store(simulate);
    StartWorker();
}

void AhBot::ForceUpdate(bool simulate)
{
    if (!simulate)
        simulate = pendingSimulate.exchange(false);
	if (!sAhBotConfig.enabled)
	{
		sLog.outString("[AhBot] ForceUpdate called but AhBot is disabled in ahbot.conf");
		return;
	}

    if (sWorld.IsShutdowning() || sWorld.IsStopped())
        return;

	bool expected = false;
	if (!updating.compare_exchange_strong(expected, true))
	{
		sLog.outString("[AhBot] ForceUpdate called but previous check is still running — skipping");
		return;
	}

    dryRun = simulate;
	sLog.outString("[AhBot] === Auction check starting%s ===", dryRun ? " (simulate)" : "");

	if (!allBidders.size())
	{
		sLog.outString("[AhBot] No bidders loaded yet — calling LoadRandomBots");
		LoadRandomBots();
	}

	if (!allBidders.size())
	{
		sLog.outError("[AhBot] No bidders available — cannot post or answer auctions. Check that AhBot.GUID is set to a valid character GUID in ahbot.conf.");
        dryRun = false;
		updating = false;
		return;
	}

	sLog.outString("[AhBot] Bidders loaded: %zu total (A=%zu H=%zu N=%zu) seller=%s buyer=%s",
		allBidders.size(), bidders[1].size(), bidders[2].size(), bidders[3].size(),
        sAhBotConfig.sellerEnabled ? "on" : "off",
        sAhBotConfig.buyerEnabled ? "on" : "off");

    LoadCycleCache();
    AssignSellerPersonas();
	CheckCategoryMultipliers();

	int answered = 0, added = 0;
	for (int i = 0; i < MAX_AUCTIONS; i++)
	{
        if (sWorld.IsShutdowning() || sWorld.IsStopped())
            break;
		sLog.outString("[AhBot] --- Checking auction house id=%u ---", auctionIds[i]);
        const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[i]);
        std::vector<AuctionSnapshot> snaps;
        if (ahEntry)
            snaps = sAuctionMgr.GetAuctionsMap(ahEntry)->GetAuctionsSnapshot();
        HouseSnapshotIndex index = BuildHouseIndex(snaps);

		InAuctionItemsBag inAuctionItems(auctionIds[i]);
		inAuctionItems.Init(true);

		int ahAnswered = 0, ahAdded = 0;
        if (sAhBotConfig.buyerEnabled)
        {
            for (int j = 0; j < CategoryList::instance.size(); j++)
            {
                Category* category = CategoryList::instance[j];
                ahAnswered += Answer(i, category, &inAuctionItems, index);
            }
        }

        if (sAhBotConfig.sellerEnabled)
        {
            uint32 target = ResolveHouseTarget(i, index.totalCount);
            int remaining = (int)ItemsToPostThisCycle(index.totalCount, target, sAhBotConfig.itemsPerCycle);
            if (target == 0 && sAhBotConfig.itemsPerCycle == 0)
                remaining = 0x7fffffff;
            else if (target == 0 && sAhBotConfig.itemsPerCycle > 0)
                remaining = (int)sAhBotConfig.itemsPerCycle;

            if (sAhBotConfig.realismDebug)
                sLog.outString("[AhBot] House %u: listings=%u target=%u remainingThisCycle=%d",
                    auctionIds[i], index.totalCount, target, remaining);

            if (HasWeightedProportions())
            {
                int weighted = 0;
                for (int c = 0; c < CategoryList::instance.size(); ++c)
                {
                    if (sAhBotConfig.GetListProportion(CategoryList::instance[c]->GetDisplayName()) > 0)
                        ++weighted;
                }
                if (weighted > 0 && weighted * 4 < CategoryList::instance.size())
                    sLog.outError("[AhBot] ListProportion covers %d/%d categories; unlisted buckets are skipped. Configure a full set or leave all at 0.",
                        weighted, CategoryList::instance.size());
                int attempts = 0;
                int maxAttempts = remaining >= 0x00ffffff ? 500 : std::max(remaining * 20, 50);
                while (remaining > 0 && attempts < maxAttempts)
                {
                    ++attempts;
                    Category* category = PickWeightedCategory();
                    if (!category)
                        break;
                    ahAdded += AddAuctions(i, category, &inAuctionItems, index, remaining);
                }
            }
            else
            {
                for (int j = 0; j < CategoryList::instance.size() && remaining > 0; j++)
                {
                    Category* category = CategoryList::instance[j];
                    ahAdded += AddAuctions(i, category, &inAuctionItems, index, remaining);
                }
            }
        }

		sLog.outString("[AhBot] Auction house id=%u: answered=%d added=%d", auctionIds[i], ahAnswered, ahAdded);
		answered += ahAnswered;
		added += ahAdded;
	}

    if (!dryRun)
	    CleanupHistory();

	sLog.outString("[AhBot] === Check complete: %d answered, %d added. Next check in %d seconds ===",
		answered, added, sAhBotConfig.updateInterval);
    dryRun = false;
    updating = false;
}

struct SortByPricePredicate
{
    bool operator()(AuctionSnapshot const & a, AuctionSnapshot const & b) const
    {
        if (a.startbid == b.startbid)
            return a.buyout < b.buyout;

        return a.startbid < b.startbid;
    }
};

std::vector<AuctionSnapshot> AhBot::LoadAuctions(const std::vector<AuctionSnapshot>& auctionEntryMap,
        Category*& category, int& auction)
{
    std::vector<AuctionSnapshot> entries;
    for (std::vector<AuctionSnapshot>::const_iterator itr = auctionEntryMap.begin();
            itr != auctionEntryMap.end(); ++itr)
    {
        const AuctionSnapshot& entry = *itr;
        if (IsBotAuction(entry.owner) || IsBotAuction(entry.bidder))
            continue;
        if (!entry.itemCount)
            continue;

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry.itemTemplate);
        if (!proto || !category->Contains(proto))
            continue;

        uint32 price = category->GetPricingStrategy()->GetBuyPrice(proto, auctionIds[auction]);
        if (!price)
        {
            sLog.outDetail("%s (x%u) in auction %d: price cannot be determined",
                    proto->Name1.c_str(), entry.itemCount, auctionIds[auction]);
            continue;
        }

        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), SortByPricePredicate());
    return entries;
}

void AhBot::FindMinPrice(const std::vector<AuctionSnapshot>& auctionEntryMap, const AuctionSnapshot& entry, uint32 itemId, uint32 itemCount, uint32* minBid,
        uint32* minBuyout)
{
    *minBid = 0;
    *minBuyout = 0;
    if (!itemCount)
        return;

    for (std::vector<AuctionSnapshot>::const_iterator itr = auctionEntryMap.begin();
            itr != auctionEntryMap.end(); ++itr)
    {
        const AuctionSnapshot& other = *itr;
        if (other.owner == entry.owner)
            continue;
        if (other.itemTemplate != itemId || !other.itemCount)
            continue;

        uint32 startbid = other.startbid / other.itemCount * itemCount;
        uint32 bid = other.bid / other.itemCount * itemCount;
        uint32 buyout = other.buyout / other.itemCount * itemCount;

        if (!bid && startbid && (!*minBid || *minBid > startbid))
            *minBid = startbid;

        if (bid && (!*minBid || *minBid > bid))
            *minBid = bid;

        if (buyout && (!*minBuyout || *minBuyout > buyout))
            *minBuyout = buyout;
    }
}

static int8 InventoryTypeToEquipSlot(uint32 invType)
{
    switch (invType)
    {
        case INVTYPE_HEAD:           return EQUIPMENT_SLOT_HEAD;
        case INVTYPE_NECK:           return EQUIPMENT_SLOT_NECK;
        case INVTYPE_SHOULDERS:      return EQUIPMENT_SLOT_SHOULDERS;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:           return EQUIPMENT_SLOT_CHEST;
        case INVTYPE_WAIST:          return EQUIPMENT_SLOT_WAIST;
        case INVTYPE_LEGS:           return EQUIPMENT_SLOT_LEGS;
        case INVTYPE_FEET:           return EQUIPMENT_SLOT_FEET;
        case INVTYPE_WRISTS:         return EQUIPMENT_SLOT_WRISTS;
        case INVTYPE_HANDS:          return EQUIPMENT_SLOT_HANDS;
        case INVTYPE_FINGER:         return EQUIPMENT_SLOT_FINGER1;
        case INVTYPE_TRINKET:        return EQUIPMENT_SLOT_TRINKET1;
        case INVTYPE_CLOAK:          return EQUIPMENT_SLOT_BACK;
        case INVTYPE_WEAPON:
        case INVTYPE_2HWEAPON:
        case INVTYPE_WEAPONMAINHAND: return EQUIPMENT_SLOT_MAINHAND;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:       return EQUIPMENT_SLOT_OFFHAND;
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:         return EQUIPMENT_SLOT_RANGED;
        default:                     return -1;
    }
}

static uint32 GetEquippedItemLevel(uint32 botGuid, uint8 slot, uint32& outGuid)
{
    outGuid = 0;
    auto result = CharacterDatabase.PQuery(
        "SELECT ci.item, ii.itemEntry FROM character_inventory ci "
        "JOIN item_instance ii ON ci.item = ii.guid "
        "WHERE ci.guid = '%u' AND ci.bag = 0 AND ci.slot = '%u'",
        botGuid, slot);
    if (!result)
        return 0;

    Field* fields = result->Fetch();
    outGuid = fields[0].GetUInt32();
    uint32 itemEntry = fields[1].GetUInt32();
    delete result;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemEntry);
    return proto ? proto->ItemLevel : 0;
}

bool AhBot::TryEquipItem(uint32 bidder, uint32 itemGuidLow, ItemPrototype const* proto)
{
    int8 primarySlot = InventoryTypeToEquipSlot(proto->InventoryType);
    if (primarySlot < 0)
        return false;

    auto charResult = CharacterDatabase.PQuery(
        "SELECT race, class, level FROM characters WHERE guid = '%u'", bidder);
    if (!charResult)
        return false;

    Field* charFields = charResult->Fetch();
    uint32 race  = charFields[0].GetUInt32();
    uint32 cls   = charFields[1].GetUInt32();
    uint32 level = charFields[2].GetUInt32();
    delete charResult;

    if (proto->RequiredLevel && level < proto->RequiredLevel)
        return false;
    if (proto->AllowableClass && !(proto->AllowableClass & (1 << (cls - 1))))
        return false;
    if (proto->AllowableRace && !(proto->AllowableRace & (1 << (race - 1))))
        return false;

    // For rings and trinkets pick the slot with the lower-level current item
    bool isDualSlot = (proto->InventoryType == INVTYPE_FINGER || proto->InventoryType == INVTYPE_TRINKET);
    uint8 slot = (uint8)primarySlot;
    uint32 currentGuid = 0;
    uint32 currentLevel = GetEquippedItemLevel(bidder, slot, currentGuid);

    if (isDualSlot)
    {
        uint8 altSlot = (proto->InventoryType == INVTYPE_FINGER) ? EQUIPMENT_SLOT_FINGER2 : EQUIPMENT_SLOT_TRINKET2;
        uint32 altGuid = 0;
        uint32 altLevel = GetEquippedItemLevel(bidder, altSlot, altGuid);
        if (altLevel < currentLevel)
        {
            slot = altSlot;
            currentGuid = altGuid;
            currentLevel = altLevel;
        }
    }

    if (proto->ItemLevel <= currentLevel)
        return false;

    sLog.outString("[AhBot] Equipping upgrade on bot guid=%u slot=%u: %s ilvl=%u (was ilvl=%u)",
            bidder, slot, proto->Name1.c_str(), proto->ItemLevel, currentLevel);

    CharacterDatabase.BeginTransaction();
    // Delete all item_instances for everything currently in this slot, then clear the slot.
    // A slot may have multiple rows if inventory was previously corrupted; clean them all.
    CharacterDatabase.PExecute(
        "DELETE ii FROM item_instance ii "
        "JOIN character_inventory ci ON ci.item = ii.guid "
        "WHERE ci.guid = '%u' AND ci.bag = 0 AND ci.slot = '%u'",
        bidder, slot);
    CharacterDatabase.PExecute("DELETE FROM character_inventory WHERE guid='%u' AND bag=0 AND slot='%u'", bidder, slot);
    CharacterDatabase.PExecute("UPDATE item_instance SET owner_guid='%u' WHERE guid='%u'", bidder, itemGuidLow);
    CharacterDatabase.PExecute("INSERT INTO character_inventory (guid, bag, slot, item, item_template) VALUES ('%u', 0, '%u', '%u', '%u')",
            bidder, slot, itemGuidLow, proto->ItemId);
    CharacterDatabase.CommitTransaction();

    return true;
}

int AhBot::Answer(int auction, Category* category, ItemBag* inAuctionItems, const HouseSnapshotIndex& index)
{
    (void)index;
    const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[auction]);
    if (!ahEntry)
        return 0;

    int answered = 0;
    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(ahEntry);
    std::vector<AuctionSnapshot> auctionEntryMap = auctionHouse->GetAuctionsSnapshot();
    int64 availableMoney = GetAvailableMoney(auctionIds[auction]);

    std::vector<AuctionSnapshot> entries = LoadAuctions(auctionEntryMap, category, auction);
    uint32 want = PickCandidateCount((uint32)entries.size(), sAhBotConfig.buyerCandidatesMin, sAhBotConfig.buyerCandidatesMax, urand(0, 0x7fffffff));
    if (want < entries.size())
    {
        Shuffle(entries);
        entries.resize(want);
    }

    sLog.outDetail("[AhBot] Answer AH %u category %s: scanning %zu entries, money=%ld",
            auctionIds[auction], category->GetName().c_str(), entries.size(), availableMoney);

    for (std::vector<AuctionSnapshot>::const_iterator itr = entries.begin(); itr != entries.end(); ++itr)
    {
        const AuctionSnapshot& snap = *itr;
        uint32 owner = snap.owner;
        if (owner == sAhBotConfig.guid)
            continue;
        if (!snap.itemCount)
            continue;
        if (!sAhBotConfig.buyerWillBidAgainstPlayers && snap.bid)
            continue;

        uint32 account = sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER, owner));
        if (!account)
        {
            sLog.outDetail("[AhBot] Skipping entry %u (owner guid=%u): account lookup failed — owner not in DB?",
                    snap.Id, owner);
            continue;
        }
        if (sPlayerbotAIConfig.IsInRandomAccountList(account))
        {
            sLog.outDetail("[AhBot] Skipping entry %u (owner guid=%u account=%u): owner is a bot account",
                    snap.Id, owner, account);
            continue;
        }

        const ItemPrototype* proto = sObjectMgr.GetItemPrototype(snap.itemTemplate);
        if (!proto)
            continue;

        sLog.outString("[AhBot] Evaluating %s (x%u) entry=%u from real player (guid=%u account=%u) AH=%u startbid=%u buyout=%u",
                proto->Name1.c_str(), snap.itemCount, snap.Id, owner, account, auctionIds[auction],
                snap.startbid, snap.buyout);

        std::vector<uint32> items = availableItems.Get(category);
        if (std::find(items.begin(), items.end(), proto->ItemId) == items.end())
        {
            sLog.outString("[AhBot] SKIP %s (x%u): not in bot's available item pool for category %s",
                    proto->Name1.c_str(), snap.itemCount, category->GetName().c_str());
            continue;
        }

        uint32 answerCount = GetAnswerCount(proto->ItemId, auctionIds[auction], sAhBotConfig.itemBuyMaxInterval);
        uint32 maxAnswerCount = category->GetMaxAllowedItemAuctionCount(proto);
        if (maxAnswerCount && answerCount > maxAnswerCount)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): already answered %u times (max=%u) within interval",
                    proto->Name1.c_str(), snap.itemCount, answerCount, maxAnswerCount);
            continue;
        }

        if (proto->RequiredLevel > sAhBotConfig.maxRequiredLevel || proto->ItemLevel > sAhBotConfig.maxItemLevel)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): reqLevel=%u itemLevel=%u exceeds max (reqLevel<=%u itemLevel<=%u)",
                    proto->Name1.c_str(), snap.itemCount,
                    proto->RequiredLevel, proto->ItemLevel,
                    sAhBotConfig.maxRequiredLevel, sAhBotConfig.maxItemLevel);
            continue;
        }

        std::ostringstream priceExplain;
        uint32 price = category->GetPricingStrategy()->GetBuyPrice(proto, auctionIds[auction], &priceExplain);
        if (!price)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): buy price is 0 (%s)",
                    proto->Name1.c_str(), snap.itemCount, priceExplain.str().c_str());
            continue;
        }

        uint32 bidPrice = SaturatingMul(snap.itemCount, price);
        uint32 buyoutHigh = price + price / 3;
        if (buyoutHigh < price)
            buyoutHigh = 4294967295u;
        uint32 buyoutPrice = SaturatingMul(snap.itemCount, urand(price, buyoutHigh));
        uint32 willingPerItem = ScaleU32(price, sAhBotConfig.buyerAcceptablePriceModifier);

        uint32 curPrice = snap.bid;
        if (!curPrice) curPrice = snap.startbid;
        if (!curPrice) curPrice = snap.buyout;

        uint32 bidder = GetRandomBidder(auctionIds[auction]);
        if (!bidder)
        {
            sLog.outError("[AhBot] No bidders for auction %d", auctionIds[auction]);
            break;
        }

        if (curPrice > buyoutPrice)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): listing price %u > bot max price %u (price/unit=%u)",
                    proto->Name1.c_str(), snap.itemCount, curPrice, buyoutPrice, price);
            CheckSendMail(bidder, buyoutPrice, snap);
            continue;
        }

        if (availableMoney < (int64)curPrice)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): listing price %u > available money %ld",
                    proto->Name1.c_str(), snap.itemCount, curPrice, availableMoney);
            continue;
        }

        uint32 vendorCap = 0;
        if (sAhBotConfig.buyerPreventOverpayVendor)
            vendorCap = GetVendorBuyPrice(proto->ItemId);

        uint32 percentileCap = 0;
        PricePercentiles stats;
        if (sAhBotConfig.customPriceStatsEnabled && TryGetPriceStats(proto->ItemId, auctionIds[auction], stats, snap.itemRandomPropertyId)
            && stats.sampleCount >= sAhBotConfig.customPriceStatsMinSampleCount)
            percentileCap = BuyerMaxAcceptedPrice(stats, sAhBotConfig.customPriceStatsBuyerMaxAcceptedPercentile);

        BuyerDecision decision = DecideBuyerOffer(snap.startbid, snap.bid, snap.buyout, snap.itemCount,
            willingPerItem, percentileCap, vendorCap, (uint32)std::max<int64>(0, availableMoney),
            sAhBotConfig.buyerAlwaysBidMax);
        if ((sAhBotConfig.buyerPreventOverpayVendor || sAhBotConfig.customPriceStatsEnabled || sAhBotConfig.buyerAlwaysBidMax)
            && !decision.buyout && !decision.bid)
        {
            if (sAhBotConfig.realismDebug)
                sLog.outString("[AhBot] SKIP %s (x%u): buyer decision blocked vendor=%d percentile=%d budget=%d",
                    proto->Name1.c_str(), snap.itemCount, decision.blockedByVendor, decision.blockedByPercentile, decision.blockedByBudget);
            continue;
        }

        uint32 minBid = 0, minBuyout = 0;
        FindMinPrice(auctionEntryMap, snap, proto->ItemId, snap.itemCount, &minBid, &minBuyout);

        if (minBid && snap.bid && minBid < snap.bid)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): current bid %u > cheaper listing %u (minBid)",
                    proto->Name1.c_str(), snap.itemCount, snap.bid, minBid);
            continue;
        }

        if (minBid && snap.startbid && minBid < snap.startbid)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): startbid %u > cheaper listing %u (minBid)",
                    proto->Name1.c_str(), snap.itemCount, snap.startbid, minBid);
            CheckSendMail(bidder, minBid, snap);
            continue;
        }

        double priceLevel = (double)curPrice / (double)buyoutPrice;
        uint32 buytime = GetBuyTime(snap.Id, proto->ItemId, auctionIds[auction], category, priceLevel);
        if (time(0) < buytime)
        {
            sLog.outString("[AhBot] SKIP %s (x%u): buy delay not expired, will act in %ld seconds",
                    proto->Name1.c_str(), snap.itemCount, (long)(buytime - time(0)));
            continue;
        }

        PendingPurchase pending;
        pending.auctionId   = snap.Id;
        pending.bidder      = bidder;
        pending.bidAmount   = decision.bid && decision.bidAmount ? decision.bidAmount : curPrice + urand(1, 1 + bidPrice / 10);
        pending.unitPrice   = price;
        pending.minBuyout   = minBuyout;
        pending.houseIndex  = auction;

        if (dryRun)
        {
            sLog.outString("[AhBot] Simulate buy: %ux %s on AH %u for %u (bidder guid=%u)",
                    snap.itemCount, proto->Name1.c_str(), auctionIds[auction], pending.bidAmount, bidder);
        }
        else
        {
            std::lock_guard<std::mutex> g(queuedWorkMutex);
            queuedPurchases.push_back(pending);
            sLog.outString("[AhBot] Queued buy: %ux %s on AH %u for %u (bidder guid=%u)",
                    snap.itemCount, proto->Name1.c_str(), auctionIds[auction], pending.bidAmount, bidder);
        }

        availableMoney -= curPrice;
        {
            std::lock_guard<std::mutex> g(cacheMutex);
            cycleCache.availableMoney[auctionIds[auction]] = availableMoney;
        }
        answered++;
    }

    return answered;
}

uint32 AhBot::GetTime(std::string category, uint32 id, uint32 auctionHouse, uint32 type)
{
    uint32 faction = factions[auctionHouse];
    std::string key = HistoryTimeKey(category, id, faction, type);
    {
        std::lock_guard<std::mutex> g(cacheMutex);
        if (cycleCache.valid)
        {
            std::map<std::string, uint32>::const_iterator it = cycleCache.historyTimes.find(key);
            if (it != cycleCache.historyTimes.end())
                return it->second;
            return 0;
        }
    }

    auto results = CharacterDatabase.PQuery("SELECT MAX(buytime) FROM ahbot_history WHERE item = '%u' AND won = '%u' AND auction_house = '%u' AND category = '%s'",
        id, type, faction, category.c_str());
    std::unique_ptr<QueryResult> results_guard(results);

    if (!results)
        return 0;

    Field* fields = results->Fetch();
    uint32 result = fields[0].GetUInt32();

    return result;
}

void AhBot::SetTime(std::string category, uint32 id, uint32 auctionHouse, uint32 type, uint32 value)
{
    uint32 faction = factions[auctionHouse];
    {
        std::lock_guard<std::mutex> g(cacheMutex);
        cycleCache.historyTimes[HistoryTimeKey(category, id, faction, type)] = value;
    }

    if (dryRun)
        return;

    CharacterDatabase.PExecute("DELETE FROM ahbot_history WHERE item = '%u' AND won = '%u' AND auction_house = '%u' AND category = '%s'",
        id, type, faction, category.c_str());

    CharacterDatabase.PExecute("INSERT INTO ahbot_history (buytime, item, bid, buyout, category, won, auction_house) "
        "VALUES ('%u', '%u', '%u', '%u', '%s', '%u', '%u')",
        value, id, 0, 0,
        category.c_str(), type, faction);
}

uint32 AhBot::GetBuyTime(uint32 entry, uint32 itemId, uint32 auctionHouse, Category*& category, double priceLevel)
{
    uint32 entryTime = GetTime("entry", entry, auctionHouse, AHBOT_WON_DELAY);
    if (entryTime > time(0))
        return entryTime;

    uint32 result = entryTime;

    std::string categoryName = category->GetName();
    uint32 categoryTime = GetTime(categoryName, 0, auctionHouse, AHBOT_WON_DELAY);
    uint32 itemTime = GetTime("item", itemId, auctionHouse, AHBOT_WON_DELAY);

    if (categoryTime < time(0)) categoryTime = time(0);
    if (itemTime < time(0)) itemTime = time(0);

    double rarity = category->GetPricingStrategy()->GetRarityPriceMultiplier(itemId);
    categoryTime += urand(sAhBotConfig.itemBuyMinInterval, sAhBotConfig.itemBuyMaxInterval) * priceLevel;
    itemTime += urand(sAhBotConfig.itemBuyMinInterval, sAhBotConfig.itemBuyMaxInterval) * priceLevel / rarity;
    entryTime = std::max(categoryTime, itemTime);

    SetTime(categoryName, 0, auctionHouse, AHBOT_WON_DELAY, categoryTime);
    SetTime("item", itemId, auctionHouse, AHBOT_WON_DELAY, itemTime);
    SetTime("entry", entry, auctionHouse, AHBOT_WON_DELAY, entryTime);

    return result ? result : entryTime;
}

uint32 AhBot::GetSellTime(uint32 itemId, uint32 auctionHouse, Category*& category)
{
    uint32 itemSellTime = GetTime("item", itemId, auctionHouse, AHBOT_SELL_DELAY);
    uint32 itemBuyTime = GetTime("item", itemId, auctionHouse, AHBOT_WON_DELAY);
    uint32 itemTime = std::max(itemSellTime, itemBuyTime);

    if (itemTime > time(0))
        return itemTime;

    uint32 result = itemTime;

    std::string categoryName = category->GetDisplayName();
    uint32 categorySellTime = GetTime(categoryName, 0, auctionHouse, AHBOT_SELL_DELAY);
    uint32 categoryBuyTime = GetTime(categoryName, 0, auctionHouse, AHBOT_WON_DELAY);
    uint32 categoryTime = std::max(categorySellTime, categoryBuyTime);

    if (categoryTime < time(0)) categoryTime = time(0);
    if (itemTime < time(0)) itemTime = time(0);

    double rarity = category->GetPricingStrategy()->GetRarityPriceMultiplier(itemId);
    categoryTime += urand(sAhBotConfig.itemSellMinInterval, sAhBotConfig.itemSellMaxInterval);
    itemTime += urand(sAhBotConfig.itemSellMinInterval, sAhBotConfig.itemSellMaxInterval) * rarity;
    itemTime = std::max(itemTime, categoryTime);

    SetTime(categoryName, 0, auctionHouse, AHBOT_SELL_DELAY, categoryTime);
    SetTime("item", itemId, auctionHouse, AHBOT_SELL_DELAY, itemTime);

    return result ? result : itemTime;
}

int AhBot::AddAuctions(int auction, Category* category, ItemBag* inAuctionItems, const HouseSnapshotIndex& index, int& remainingCycle)
{
    if (remainingCycle <= 0)
        return 0;

    int32 maxAllowedAuctionCount = categoryMaxAuctionCount[category->GetDisplayName()];
    if (inAuctionItems->GetCount(category) >= maxAllowedAuctionCount)
    {
        sLog.outDetail("[AhBot] Category '%s' on AH %u: at cap (%d/%d), skipping",
            category->GetDisplayName().c_str(), auctionIds[auction],
            inAuctionItems->GetCount(category), maxAllowedAuctionCount);
        return 0;
    }

    int added = 0;
    int ladded = 0;
    std::vector<uint32> available = availableItems.Get(category);
    if (available.empty())
        return 0;

    // Listing-stats scarcity: drop rarely-seen items from this cycle's pool.
    if (sAhBotConfig.listingStatsEnabled)
    {
        std::vector<uint32> weighted;
        weighted.reserve(available.size());
        std::lock_guard<std::mutex> g(cacheMutex);
        for (uint32 itemId : available)
        {
            ItemStatKey key = MakeStatKey(itemId, 0, auctionIds[auction]);
            std::map<ItemStatKey, uint32>::const_iterator seenIt = cycleCache.listingSeen.find(key);
            std::map<ItemStatKey, uint32>::const_iterator snapIt = cycleCache.listingSnapshots.find(key);
            uint32 seen = seenIt == cycleCache.listingSeen.end() ? 0 : seenIt->second;
            uint32 snaps = snapIt == cycleCache.listingSnapshots.end() ? 0 : snapIt->second;
            float weight = sAhBotConfig.listingStatsFallbackWeight;
            if (seen >= sAhBotConfig.listingStatsMinSeenCount && snaps > 0)
            {
                float pct = (float)seen / (float)snaps;
                weight = std::pow(pct, sAhBotConfig.listingStatsScarcityExponent);
            }
            uint32 copies = std::max<uint32>(1, (uint32)std::ceil(weight * 10.0f));
            for (uint32 n = 0; n < copies && weighted.size() < available.size() * 4; ++n)
                weighted.push_back(itemId);
        }
        if (!weighted.empty())
            available.swap(weighted);
    }

    for (int32 i = 0; i <= maxAllowedAuctionCount && remainingCycle > 0 && inAuctionItems->GetCount(category) < maxAllowedAuctionCount; ++i)
    {
        uint32 indexItem = urand(0, available.size() - 1);
        uint32 itemId = available[indexItem];

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto)
            continue;

        int32 maxAllowedItems = category->GetMaxAllowedItemAuctionCount(proto);
        if (maxAllowedItems && inAuctionItems->GetCount(category, proto->ItemId) >= maxAllowedItems)
        {
            sLog.outDetail("%s in auction %d: has reached max %d/%d",
                proto->Name1.c_str(), auctionIds[auction], inAuctionItems->GetCount(category, proto->ItemId), maxAllowedItems);
            continue;
        }

        if (sAhBotConfig.maxActiveEnabled)
        {
            uint32 cap = (uint32)sAhBotConfig.GetMaxActiveForCategory(category->GetDisplayName());
            std::map<uint32, uint32>::const_iterator activeIt = index.botActiveCount.find(proto->ItemId);
            uint32 active = activeIt == index.botActiveCount.end() ? 0 : activeIt->second;
            if (cap && active >= cap)
            {
                if (sAhBotConfig.dynamicSupplyDebug || sAhBotConfig.realismDebug)
                    sLog.outString("[AhBot] Skip %s: active %u >= cap %u", proto->Name1.c_str(), active, cap);
                continue;
            }
        }

        if (sAhBotConfig.dynamicSupplyEnabled)
        {
            uint32 skipChance = (uint32)sAhBotConfig.GetEmptyMarketChance(category->GetDisplayName());
            if (skipChance > 100)
                skipChance = 100;
            if (skipChance && urand(1, 100) <= skipChance)
            {
                if (sAhBotConfig.dynamicSupplyDebug || sAhBotConfig.realismDebug)
                    sLog.outString("[AhBot] Skip %s: dynamic supply gap %u%%", proto->Name1.c_str(), skipChance);
                continue;
            }
        }

        uint32 sellTime = GetSellTime(proto->ItemId, auctionIds[auction], category);
        if ((uint32)time(0) < sellTime)
        {
            ladded += 1;
            sLog.outDetail( "%s in auction %d: will add in %ld seconds",
                    proto->Name1.c_str(), auctionIds[auction], sellTime - time(0));
            continue;
        }
        else if (time(0) - sellTime > sAhBotConfig.maxSellInterval)
        {
            sLog.outDetail( "%s in auction %d: too old (%ld secs)",
                    proto->Name1.c_str(), auctionIds[auction], time(0) - sellTime);
            continue;
        }
        inAuctionItems->Add(proto);
        int posted = AddAuction(auction, category, proto, index);
        added += posted;
        remainingCycle -= posted;
    }

    if (added > 0 || ladded > 0)
        sLog.outString("[AhBot] Category '%s' on AH %u: %d new listing(s), %d pending (sell delay not elapsed)",
            category->GetDisplayName().c_str(), auctionIds[auction], added, ladded);

    return added;
}

int AhBot::AddAuction(int auction, Category* category, ItemPrototype const* proto, const HouseSnapshotIndex& index)
{
    uint32 owner = GetRandomBidder(auctionIds[auction]);
    if (!owner)
    {
        sLog.outError("[AhBot] No valid bidder found for auction house %u — cannot list %s", auctionIds[auction], proto->Name1.c_str());
        return 0;
    }

    std::string name;
    if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, owner), name))
    {
        sLog.outError("[AhBot] Owner GUID %u has no character record — cannot list %s", owner, proto->Name1.c_str());
        return 0;
    }

    uint32 price = category->GetPricingStrategy()->GetSellPrice(proto, auctionIds[auction]);
    if (!dryRun)
        updateMarketPrice(proto->ItemId, price, auctionIds[auction]);
    price = category->GetPricingStrategy()->GetSellPrice(proto, auctionIds[auction]);
    price = ApplySellPriceAdjustments(proto, price, owner, index, category, auctionIds[auction]);

    uint32 stackCount = ChooseListingStack(proto, category);
    if (!price || !stackCount)
        return 0;

    if (price > sAhBotConfig.stackReducePrice)
        stackCount /= (price / sAhBotConfig.stackReducePrice);

    if (!stackCount)
        stackCount = 1;

    if (urand(0, 100) <= sAhBotConfig.underPriceProbability * 100)
        price = price * 100 / urand(100, 200);

    uint32 buyoutHigh = price + price / 3;
    if (buyoutHigh < price)
        buyoutHigh = 4294967295u;
    uint32 bidPrice = PricingStrategy::RoundPrice(SaturatingMul(stackCount, price));
    uint32 buyoutPrice = PricingStrategy::RoundPrice(SaturatingMul(stackCount, urand(price, buyoutHigh)));
    if (sAhBotConfig.bidVariationLowReducePercent > 0.0f || sAhBotConfig.bidVariationHighReducePercent > 0.0f)
        bidPrice = PricingStrategy::RoundPrice(BidFromBuyout(buyoutPrice, sAhBotConfig.bidVariationLowReducePercent,
            sAhBotConfig.bidVariationHighReducePercent, urand(0, 0x7fffffff)));

    uint32 auction_time;
    if (sAhBotConfig.listingExpireMinSeconds && sAhBotConfig.listingExpireMaxSeconds)
        auction_time = urand(sAhBotConfig.listingExpireMinSeconds, sAhBotConfig.listingExpireMaxSeconds);
    else
        auction_time = uint32(urand(8, 24) * HOUR * sWorld.getConfig(CONFIG_FLOAT_RATE_AUCTION_TIME));

    if (dryRun)
    {
        sLog.outString("[AhBot] Simulate list: %ux %s on AH %u for %u..%u (owner: %s guid=%u persona=%u)",
            stackCount, proto->Name1.c_str(), auctionIds[auction], bidPrice, buyoutPrice, name.c_str(), owner,
            (unsigned)GetPersonaForBidder(owner));
        return 1;
    }

    PendingListing listing;
    listing.houseIndex = auction;
    listing.owner = owner;
    listing.itemId = proto->ItemId;
    listing.stackCount = stackCount;
    listing.bidPrice = bidPrice;
    listing.buyoutPrice = buyoutPrice;
    listing.auctionTime = auction_time;
    {
        std::lock_guard<std::mutex> g(queuedWorkMutex);
        queuedListings.push_back(listing);
    }

    sLog.outString("[AhBot] Queued list: %dx %s on AH %u for %ug%us..%ug%us (owner: %s guid=%u)",
        stackCount, proto->Name1.c_str(), auctionIds[auction],
        bidPrice / 10000, (bidPrice % 10000) / 100,
        buyoutPrice / 10000, (buyoutPrice % 10000) / 100,
        name.c_str(), owner);
    return 1;
}

void AhBot::HandleCommand(std::string command, ChatHandler* handler)
{
    while (!command.empty() && command[0] == ' ')
        command.erase(command.begin());

    std::string lowered = command;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lowered == "reload")
    {
        if (updating.load())
        {
            CommandReply(handler, "AhBot is busy; retry reload after the current cycle.");
            return;
        }
        bool ok = sAhBotConfig.Reload();
        InvalidateCycleCache();
        CommandReply(handler, ok ? "AhBot config reloaded." : "AhBot config reload failed or bot is disabled.");
        return;
    }

    if (lowered == "status")
    {
        PrintStatus(handler);
        return;
    }

    if (lowered == "simulate" || lowered == "dryrun")
    {
        if (!sAhBotConfig.enabled)
        {
            CommandReply(handler, "AhBot is disabled.");
            return;
        }
        CommandReply(handler, "Starting simulated auction cycle (no posts, buys, or writes).");
        RequestUpdate(true);
        return;
    }

    if (!sAhBotConfig.enabled && lowered != "help" && atoi(command.c_str()) == 0)
    {
        CommandReply(handler, "AhBot is disabled. Use 'ahbot reload' after enabling it in ahbot.conf.");
        return;
    }

    if (lowered == "expire")
    {
        for (int i = 0; i < MAX_AUCTIONS; i++)
            Expire(i);
        CharacterDatabase.PExecute("DELETE FROM ahbot_category");
        CharacterDatabase.PExecute("UPDATE ahbot_history SET buytime = buytime - 3600 * 24;");
        CommandReply(handler, "Bot auctions marked expired.");
        return;
    }

    if (lowered == "stats")
    {
        for (int i = 0; i < MAX_AUCTIONS; i++)
            PrintStats(i, handler);
        return;
    }

    if (lowered == "update")
    {
        RequestUpdate(false);
        CommandReply(handler, "Auction check scheduled.");
        return;
    }

    if (lowered == "dump")
    {
        Dump();
        CommandReply(handler, "Price dump written to the server log.");
        return;
    }

    uint32 itemId = atoi(command.c_str());
    if (!itemId)
    {
        CommandReply(handler, "ahbot status - seller/buyer flags, house targets, cache");
        CommandReply(handler, "ahbot reload - re-read ahbot.conf");
        CommandReply(handler, "ahbot update - run one check cycle");
        CommandReply(handler, "ahbot simulate|dryrun - cycle without mutations");
        CommandReply(handler, "ahbot stats - per-house listing counts");
        CommandReply(handler, "ahbot expire - expire bot auctions");
        CommandReply(handler, "ahbot dump - log sell/buy prices");
        CommandReply(handler, "ahbot <itemId> - show item price");
        return;
    }

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
    if (!proto)
        return;

    for (int i=0; i<CategoryList::instance.size(); i++)
    {
        Category* category = CategoryList::instance[i];
        if (category->Contains(proto))
        {
            std::vector<uint32> items = availableItems.Get(category);
            if (std::find(items.begin(), items.end(), proto->ItemId) == items.end())
                continue;

            std::ostringstream out;
            out << proto->Name1.c_str() << " (" << category->GetDisplayName() << "), "
                    << category->GetMaxAllowedAuctionCount() << "x" << category->GetMaxAllowedItemAuctionCount(proto)
                    << "x" << category->GetStackCount(proto) << " max";
            CommandReply(handler, out.str());
            for (int auction = 0; auction < MAX_AUCTIONS; auction++)
            {
                std::ostringstream house;
                house << "--- auction house " << auctionIds[auction] << "(faction: " << factions[auctionIds[auction]] << ", money: "
                    << GetAvailableMoney(auctionIds[auction])
                    << ") ---";
                CommandReply(handler, house.str());

                std::ostringstream exp1;
                std::ostringstream sell;
                sell << "sell: " << ChatHelper::formatMoney(category->GetPricingStrategy()->GetSellPrice(proto, auctionIds[auction], true, &exp1));
                sell << " ("  << exp1.str().c_str() << ")";
                CommandReply(handler, sell.str());

                std::ostringstream exp2;
                std::ostringstream buy;
                buy << "buy: " << ChatHelper::formatMoney(category->GetPricingStrategy()->GetBuyPrice(proto, auctionIds[auction], &exp2));
                buy << " ("  << exp2.str().c_str() << ")";
                CommandReply(handler, buy.str());

                std::ostringstream market;
                market << "market: " << ChatHelper::formatMoney(category->GetPricingStrategy()->GetMarketPrice(proto->ItemId, auctionIds[auction]));
                CommandReply(handler, market.str());

                PricePercentiles stats;
                if (TryGetPriceStats(proto->ItemId, auctionIds[auction], stats))
                {
                    std::ostringstream p;
                    p << "stats n=" << stats.sampleCount
                      << " p10=" << stats.priceP10
                      << " p25=" << stats.priceP25
                      << " med=" << stats.priceMedian
                      << " p75=" << stats.priceP75
                      << " p90=" << stats.priceP90;
                    CommandReply(handler, p.str());
                }
            }
            sLog.outString("%s priced via category %s", proto->Name1.c_str(), category->GetDisplayName().c_str());
        }
    }
}

void AhBot::Expire(int auction)
{
    if (!sAhBotConfig.enabled)
        return;

    AuctionHouseEntry const* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[auction]);
    if(!ahEntry)
        return;

    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(ahEntry);

    // This one writes expireTime, so it needs the live entries rather than a
    // snapshot. Each iteration is a cheap set lookup, so just hold the lock
    // across the whole loop.
    int count = 0;
    {
        AuctionHouseObject::Guard g(auctionHouse->GetLock());
        AuctionHouseObject::AuctionEntryMapBounds bounds = auctionHouse->GetAuctionsBounds_locked();
        for (AuctionHouseObject::AuctionEntryMap::iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            if (IsBotAuction(itr->second->owner))
            {
                itr->second->expireTime = sWorld.GetGameTime();
                count++;
            }
        }
    }

    sLog.outString("%d auctions marked as expired in auction %d", count, auctionIds[auction]);
}

void AhBot::PrintStats(int auction, ChatHandler* handler)
{
    if (!sAhBotConfig.enabled)
        return;

    AuctionHouseEntry const* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[auction]);
    if(!ahEntry)
        return;

    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(ahEntry);
    std::ostringstream out;
    out << auctionHouse->GetCount() << " auctions available on auction house " << auctionIds[auction];
    CommandReply(handler, out.str());
}

void AhBot::AddToHistory(AuctionEntry* entry, uint32 won)
{
    if (!sAhBotConfig.enabled || !entry)
        return;

    if (!IsBotAuction(entry->owner) && !IsBotAuction(entry->bidder))
        return;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry->itemTemplate);
    if (!proto)
        return;

    std::string category = "";
    for (int i = 0; i < CategoryList::instance.size(); i++)
    {
        if (CategoryList::instance[i]->Contains(proto))
        {
            category = CategoryList::instance[i]->GetName();
            break;
        }
    }

    if (!won)
    {
        won = AHBOT_WON_PLAYER;
        if (IsBotAuction(entry->bidder))
            won = AHBOT_WON_SELF;
    }

    sLog.outDetail( "AddToHistory: market price adjust");
    int count = entry->itemCount ? entry->itemCount : 1;
    updateMarketPrice(proto->ItemId, entry->buyout / count, entry->auctionHouseEntry->houseId);

    uint32 now = time(0);
    CharacterDatabase.PExecute("INSERT INTO ahbot_history (buytime, item, bid, buyout, category, won, auction_house) "
        "VALUES ('%u', '%u', '%u', '%u', '%s', '%u', '%u')",
        now, entry->itemTemplate, entry->bid ? entry->bid : entry->startbid, entry->buyout,
        category.c_str(), won, factions[entry->auctionHouseEntry->houseId]);
}

uint32 AhBot::GetAnswerCount(uint32 itemId, uint32 auctionHouse, uint32 withinTime)
{
    uint32 faction = factions[auctionHouse];
    {
        std::lock_guard<std::mutex> g(cacheMutex);
        if (cycleCache.valid)
        {
            std::map<uint64, uint32>::const_iterator it = cycleCache.answerCounts.find(CacheKey(itemId, faction));
            if (it != cycleCache.answerCounts.end())
                return it->second;
            return 0;
        }
    }

    uint32 count = 0;

    auto results = CharacterDatabase.PQuery("SELECT COUNT(*) FROM ahbot_history WHERE "
        "item = '%u' AND won in (2, 3) AND auction_house = '%u' AND buytime > '%lu'",
        itemId, faction, time(0) - withinTime);
    std::unique_ptr<QueryResult> results_guard(results);
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            count = fields[0].GetUInt32();
        } while (results->NextRow());
    }

    return count;
}

void AhBot::CleanupHistory()
{
    uint32 when = time(0) - 3600 * 24 * sAhBotConfig.historyDays;
    CharacterDatabase.PExecute("DELETE FROM ahbot_history WHERE buytime < '%u'", when);
}

uint32 AhBot::GetAvailableMoney(uint32 auctionHouse)
{
    {
        std::lock_guard<std::mutex> g(cacheMutex);
        if (cycleCache.valid)
        {
            std::map<uint32, int64>::const_iterator it = cycleCache.availableMoney.find(auctionHouse);
            if (it != cycleCache.availableMoney.end())
                return it->second < 0 ? 0 : (uint32)it->second;
        }
    }

    int64 result = sAhBotConfig.alwaysAvailableMoney;

    std::map<uint32, uint64> data;
    data[AHBOT_WON_PLAYER] = 0;
    data[AHBOT_WON_SELF] = 0;

    uint32 faction = factions[auctionHouse];
    const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionHouse);
    auto results = CharacterDatabase.PQuery(
        "SELECT won, SUM(bid) FROM ahbot_history WHERE auction_house = '%u' GROUP BY won HAVING won > 0 ORDER BY won",
        faction);
    std::unique_ptr<QueryResult> results_guard(results);
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            data[fields[0].GetUInt32()] = fields[1].GetUInt64();

        } while (results->NextRow());
    }

    results = CharacterDatabase.PQuery(
        "SELECT max(buytime) FROM ahbot_history WHERE auction_house = '%u' AND won = '2'",
        faction);
    results_guard.reset(results);
    if (results)
    {
        Field* fields = results->Fetch();
        uint32 lastBuyTime = fields[0].GetUInt32();
        uint32 now = time(0);
        if (lastBuyTime && now > lastBuyTime)
        result += (now - lastBuyTime) / 3600 / 24 * sAhBotConfig.alwaysAvailableMoney;
    }

    std::vector<AuctionSnapshot> auctionEntryMap = sAuctionMgr.GetAuctionsMap(ahEntry)->GetAuctionsSnapshot();
    for (std::vector<AuctionSnapshot>::const_iterator itr = auctionEntryMap.begin(); itr != auctionEntryMap.end(); ++itr)
    {
        if (!IsBotAuction(itr->bidder))
            continue;

        result -= itr->bid;
    }

    result += (int64)data[AHBOT_WON_PLAYER] - (int64)data[AHBOT_WON_SELF];
    uint32 money = 0;
    if (result > 0)
        money = result > 4294967295ll ? 4294967295u : (uint32)result;
    {
        std::lock_guard<std::mutex> g(cacheMutex);
        cycleCache.availableMoney[auctionHouse] = money;
    }
    return money;
}

void AhBot::CheckCategoryMultipliers()
{
    auto results = CharacterDatabase.PQuery("SELECT category, multiplier, max_auction_count, expire_time FROM ahbot_category");
    std::unique_ptr<QueryResult> results_guard(results);
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            categoryMultipliers[fields[0].GetString()] = fields[1].GetFloat();
            categoryMaxAuctionCount[fields[0].GetString()] = fields[2].GetInt32();
            categoryMultiplierExpireTimes[fields[0].GetString()] = fields[3].GetUInt64();

        } while (results->NextRow());
    }

    if (!dryRun)
        CharacterDatabase.PExecute("DELETE FROM ahbot_category");

    std::set<std::string> tmp;
    for (int i = 0; i < CategoryList::instance.size(); i++)
    {
        std::string name = CategoryList::instance[i]->GetDisplayName();

        if (tmp.find(name) != tmp.end())
            continue;

        tmp.insert(name);
        if (categoryMultiplierExpireTimes[name] <= (uint64)time(0) || categoryMultipliers[name] <= 0)
        {
            uint32 k = urand(1, 100);
            double m = 1.0;
            double r = (double)urand(100, 200) / 100.0;
            if (k < 50) m = r; // 1..2
            else if (k < 80) m = 1 + r; // 2..3
            else if (k < 90) m = 2 + r; // 3..4
            else m = 3 + r; // 4..5
            categoryMultipliers[name] = m;
            categoryMultiplierExpireTimes[name] = time(0) + urand(4, 7) * 3600 * 24;
        }

        categoryMaxAuctionCount[name] = CategoryList::instance[i]->GetMaxAllowedAuctionCount();

        if (!dryRun)
            CharacterDatabase.PExecute("INSERT INTO ahbot_category (category, multiplier, max_auction_count, expire_time) "
                "VALUES ('%s', '%f', '%u', '%zu')",
                name.c_str(), categoryMultipliers[name], categoryMaxAuctionCount[name], categoryMultiplierExpireTimes[name]);
    }
}


void AhBot::updateMarketPrice(uint32 itemId, double price, uint32 auctionHouse)
{
    double marketPrice = 0;
    uint64 key = CacheKey(itemId, auctionHouse);
    {
        std::lock_guard<std::mutex> g(cacheMutex);
        std::map<uint64, double>::const_iterator it = cycleCache.marketPrices.find(key);
        if (it != cycleCache.marketPrices.end())
            marketPrice = it->second;
    }

    if (marketPrice == 0)
    {
        auto results = CharacterDatabase.PQuery("SELECT price FROM ahbot_price WHERE item = '%u' AND auction_house = '%u'", itemId, auctionHouse);
        std::unique_ptr<QueryResult> results_guard(results);
        if (results)
            marketPrice = results->Fetch()[0].GetFloat();
    }

    if (marketPrice > 0)
        marketPrice = (marketPrice + price) / 2;
    else
        marketPrice = price;

    {
        std::lock_guard<std::mutex> g(cacheMutex);
        cycleCache.marketPrices[key] = marketPrice;
    }

    if (dryRun)
        return;

    CharacterDatabase.PExecute("DELETE FROM ahbot_price WHERE item = '%u' AND auction_house = '%u'", itemId, auctionHouse);
    CharacterDatabase.PExecute("INSERT INTO ahbot_price (item, price, auction_house) VALUES ('%u', '%lf', '%u')", itemId, marketPrice, auctionHouse);
}

bool AhBot::IsBotAuction(uint32 bidder) const
{
    return allBidders.find(bidder) != allBidders.end();
}

uint32 AhBot::GetRandomBidder(uint32 auctionHouse)
{
    uint32 faction = factions[auctionHouse];
    std::vector<uint32> guids = bidders[faction];
    if (guids.empty())
    {
        sLog.outError("[AhBot] GetRandomBidder: no bidders registered for AH %u (faction %u)", auctionHouse, faction);
        return 0;
    }

    std::vector<uint32> online;
    for (std::vector<uint32>::iterator i = guids.begin(); i != guids.end(); ++i)
    {
        uint32 guid = *i;
        std::string name;
        if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name))
        {
            sLog.outError("[AhBot] GetRandomBidder: GUID %u has no character record (AH %u faction %u) — skipping", guid, auctionHouse, faction);
            continue;
        }

        online.push_back(guid);
    }

    if (online.empty())
    {
        sLog.outError("[AhBot] GetRandomBidder: all %zu bidder GUID(s) for AH %u failed character lookup", guids.size(), auctionHouse);
        return 0;
    }

    int index = urand(0, online.size() - 1);
    return online[index];
}

void AhBot::LoadRandomBots()
{
    sLog.outString("[AhBot] LoadRandomBots: scanning %zu random bot account(s)", sPlayerbotAIConfig.randomBotAccounts.size());

    for (std::list<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin(); i != sPlayerbotAIConfig.randomBotAccounts.end(); i++)
    {
        uint32 accountId = *i;
        if (!sAccountMgr.GetCharactersCount(accountId))
            continue;

        auto result = CharacterDatabase.PQuery("SELECT guid, race FROM characters WHERE account = '%u'", accountId);
        std::unique_ptr<QueryResult> result_guard(result);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint8 race = fields[1].GetUInt8();
            uint32 auctionHouse = PlayerbotAI::IsOpposing(race, RACE_HUMAN) ? 2 : 1;
            bidders[auctionHouse].push_back(guid);
            bidders[3].push_back(guid);
            allBidders.insert(guid);
        } while (result->NextRow());
    }

    if (allBidders.empty() && sAhBotConfig.guid)
    {
        sLog.outString("[AhBot] No bot-account bidders found — falling back to AhBot.GUID=%llu", (unsigned long long)sAhBotConfig.guid);
        uint32 guid = sAhBotConfig.guid;
        allBidders.insert(guid);
        for (int i = 1; i <= 3; i++)
        {
            bidders[i].push_back(guid);
        }
    }

    sLog.outString("[AhBot] Bidders ready: Alliance=%zu Horde=%zu Neutral=%zu (total unique=%zu)",
        bidders[1].size(), bidders[2].size(), bidders[3].size(), allBidders.size());
}

int32 AhBot::GetSellPrice(ItemPrototype const* proto)
{
    if (!sAhBotConfig.enabled)
        return 0;

    int32 maxPrice = 0;
    for (int i=0; i<CategoryList::instance.size(); i++)
    {
        Category* category = CategoryList::instance[i];
        if (!category->Contains(proto))
            continue;

        std::vector<uint32> items = availableItems.Get(category);
        if (std::find(items.begin(), items.end(), proto->ItemId) == items.end())
            continue;

        for (int auction = 0; auction < MAX_AUCTIONS; auction++)
        {
            int32 price = (int32)category->GetPricingStrategy()->GetSellPrice(proto, auctionIds[auction]);
            if (!price)
                price = (int32)category->GetPricingStrategy()->GetBuyPrice(proto, auctionIds[auction]);

            if (price > maxPrice)
                maxPrice = price;
        }
    }

    return maxPrice;
}

int32 AhBot::GetBuyPrice(ItemPrototype const* proto)
{
    if (!sAhBotConfig.enabled)
        return 0;

    int32 maxPrice = 0;
    for (int i=0; i<CategoryList::instance.size(); i++)
    {
        Category* category = CategoryList::instance[i];
        if (!category->Contains(proto))
            continue;

        std::vector<uint32> items = availableItems.Get(category);
        if (std::find(items.begin(), items.end(), proto->ItemId) == items.end())
            continue;

        for (int auction = 0; auction < MAX_AUCTIONS; auction++)
        {
            int32 price = (int32)category->GetPricingStrategy()->GetBuyPrice(proto, auctionIds[auction]);
            if (!price)
                continue;

            if (price > maxPrice)
                maxPrice = price;
        }
    }

    return maxPrice;
}

double AhBot::GetRarityPriceMultiplier(const ItemPrototype* proto)
{
    if (!sAhBotConfig.enabled)
        return 1.0;

    for (int i=0; i<CategoryList::instance.size(); i++)
    {
        Category* category = CategoryList::instance[i];
        if (!category->Contains(proto))
            continue;

        return category->GetPricingStrategy()->GetRarityPriceMultiplier(proto->ItemId);
    }

    return 1.0;

}

bool AhBot::IsUsedBySkill(const ItemPrototype* proto, uint32 skillId)
{
    if (!sAhBotConfig.enabled)
        return false;

    for (int i=0; i<CategoryList::instance.size(); i++)
    {
        Category* category = CategoryList::instance[i];
        if (category->GetSkillId() == skillId && category->Contains(proto))
            return true;
    }

    return false;
}

void AhBot::CheckSendMail(uint32 bidder, uint32 price, const AuctionSnapshot& entry)
{
    if (!sAhBotConfig.sendmail || dryRun)
        return;

    time_t entryTime = GetTime("entry", entry.Id, entry.houseId, AHBOT_SENDMAIL);
    if (entryTime > time(0))
        return;

    const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(entry.houseId);
    if (!ahEntry)
        return;

    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(ahEntry);
    std::vector<AuctionSnapshot> auctionEntryMap = auctionHouse->GetAuctionsSnapshot();
    for (std::vector<AuctionSnapshot>::const_iterator itr = auctionEntryMap.begin(); itr != auctionEntryMap.end(); ++itr)
    {
        const AuctionSnapshot& otherEntry = *itr;
        if (otherEntry.owner == entry.owner && otherEntry.Id != entry.Id && otherEntry.itemTemplate == entry.itemTemplate)
        {
            time_t otherEntryTime = GetTime("entry", otherEntry.Id, entry.houseId, AHBOT_SENDMAIL);
            if (otherEntryTime > time(0))
                return;
        }
    }

    // Sending resolves the receiver through ObjectAccessor and can reach their
    // live Player, so hand it to the world thread rather than doing it here.
    PendingProposition proposition;
    proposition.auctionId   = entry.Id;
    proposition.owner       = entry.owner;
    proposition.itemGuidLow = entry.itemGuidLow;
    proposition.bidder      = bidder;
    proposition.price       = price;
    proposition.houseId     = entry.houseId;
    proposition.expireTime  = entry.expireTime;

    std::lock_guard<std::mutex> g(queuedWorkMutex);
    queuedPropositions.push_back(proposition);
}

void AhBot::RunQueuedWork()
{
    std::vector<PendingPurchase> purchases;
    std::vector<PendingProposition> propositions;
    std::vector<PendingListing> listings;
    {
        std::lock_guard<std::mutex> g(queuedWorkMutex);
        if (queuedPurchases.empty() && queuedPropositions.empty() && queuedListings.empty())
            return;
        purchases.swap(queuedPurchases);
        propositions.swap(queuedPropositions);
        listings.swap(queuedListings);
    }

    for (std::vector<PendingListing>::const_iterator i = listings.begin(); i != listings.end(); ++i)
        ExecuteListing(*i);

    for (std::vector<PendingPurchase>::const_iterator i = purchases.begin(); i != purchases.end(); ++i)
        ExecutePurchase(*i);

    for (std::vector<PendingProposition>::const_iterator i = propositions.begin(); i != propositions.end(); ++i)
        ExecuteProposition(*i);
}

void AhBot::ExecutePurchase(const PendingPurchase& p)
{
    const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[p.houseIndex]);
    if (!ahEntry)
        return;

    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(ahEntry);
    if (!auctionHouse)
        return;

    // Hold the auctions lock for the whole read-act-remove sequence below, not
    // just the initial lookup. GetAuction()/RemoveAuction() only lock for their
    // own instant (the mutex is recursive specifically so callers can wrap a
    // larger critical section around them, per the comment on m_auctionsLock) -
    // entry is a raw pointer into the live map, and everything from here
    // through RemoveAuction()+delete needs to be atomic with respect to the
    // world thread's own packet handlers and AhBot::Update()'s background scan,
    // both of which can also touch this same entry. This is what the class
    // comment already documents as the intended pattern ("re-resolve the id
    // under the lock and re-check that the entry is still there") - this
    // function just never actually did it, leaving entry->bidder/bid writes,
    // SendAuctionSuccessfulMail, RemoveAuction and delete entry all racing
    // against concurrent access to the same AuctionsMap/AuctionEntry.
    AuctionHouseObject::Guard g(auctionHouse->GetLock());

    // The seller may have cancelled, or the auction may have expired or sold,
    // between the bot deciding and us getting here.
    AuctionEntry* entry = auctionHouse->GetAuction(p.auctionId);
    if (!entry)
        return;

    Item* item = sAuctionMgr.GetAItem(entry->itemGuidLow);
    if (!item || !item->GetCount())
        return;

    ItemPrototype const* proto = item->GetProto();
    if (!proto)
        return;

    entry->bidder = p.bidder;
    entry->bid = p.bidAmount;

    if ((entry->buyout && (entry->bid >= entry->buyout ||
            (uint64)100 * (entry->buyout - entry->bid) / std::max(p.unitPrice, 1u) < 25)) &&
            !(p.minBuyout && entry->buyout && p.minBuyout < entry->buyout))
    {
        entry->bid = entry->buyout;
        sLog.outString("[AhBot] Bought: %dx %s on AH %u for %u (bidder guid=%u)",
                item->GetCount(), proto->Name1.c_str(), auctionIds[p.houseIndex], entry->buyout, p.bidder);
    }
    else
    {
        sLog.outString("[AhBot] Bought (at bid): %dx %s on AH %u for %u (bidder guid=%u)",
                item->GetCount(), proto->Name1.c_str(), auctionIds[p.houseIndex], entry->bid, p.bidder);
    }

    updateMarketPrice(proto->ItemId, entry->buyout / item->GetCount(), auctionIds[p.houseIndex]);

    // Pay the seller immediately and finalize the auction.
    // If the item is an upgrade for the bidder bot, equip it directly in the DB.
    // Otherwise discard it - letting items go through the normal mail/login path
    // causes inventory corruption when bot inventories are full.
    uint32 itemGuidLow = entry->itemGuidLow;
    sAuctionMgr.SendAuctionSuccessfulMail(entry);
    if (!TryEquipItem(entry->bidder, itemGuidLow, proto))
        CharacterDatabase.PExecute("DELETE FROM item_instance WHERE guid='%u'", itemGuidLow);
    sAuctionMgr.RemoveAItem(itemGuidLow);
    delete item;
    AddToHistory(entry, AHBOT_WON_BID);
    entry->DeleteFromDB();
    auctionHouse->RemoveAuction(entry);
    delete entry;

    CharacterDatabase.PExecute("DELETE FROM ahbot_history WHERE item = '%u' AND won = 4 AND auction_house = '%u' ",
            proto->ItemId, factions[auctionIds[p.houseIndex]]);
}

void AhBot::ExecuteProposition(const PendingProposition& p)
{
    Item* item = sAuctionMgr.GetAItem(p.itemGuidLow);
    if (!item || !item->GetProto())
        return;

    std::string name;
    if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, p.bidder), name))
        return;

    std::ostringstream body;
    body << "Hello,\n";
    body << "\n";
    body << "I see you posted " << ChatHelper::formatItem(item, item->GetCount());
    body << " to the AH and I really need that at the moment. Could you lower your price at least to ";
    body << ChatHelper::formatMoney(PricingStrategy::RoundPrice(p.price)) << "? I'll buy it then.\n";
    body << "\n";
    body << "Regards,\n";
    body << name << "\n";

    std::ostringstream title; title << "AH Proposition: " << item->GetProto()->Name1.c_str();
    MailDraft draft(title.str(), body.str());
    ObjectGuid receiverGuid(HIGHGUID_PLAYER, p.owner);
    draft.SendMailTo(MailReceiver(receiverGuid), MailSender(MAIL_NORMAL, p.bidder));

    SetTime("entry", p.auctionId, p.houseId, AHBOT_SENDMAIL, p.expireTime);
}

void AhBot::Dump()
{
    for (uint32 itemId = 0; itemId < sItemStorage.GetMaxEntry(); ++itemId)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto)
            continue;

        bool first = true;
        for (int i=0; i<CategoryList::instance.size(); i++)
        {
            Category* category = CategoryList::instance[i];
            if (category->Contains(proto))
            {
                std::vector<uint32> items = availableItems.Get(category);
                if (find(items.begin(), items.end(), proto->ItemId) == items.end())
                    continue;

                std::ostringstream out;
                if (first)
                {
                    out << proto->ItemId << " (" << proto->Name1.c_str() << ") x" << category->GetStackCount(proto) << " - ";
                    first = false;
                }

                int auction = 0;
                const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[auction]);
                out << "SELL: "
                    << ChatHelper::formatMoney(category->GetPricingStrategy()->GetSellPrice(proto, auctionIds[auction], true))
                    << ", BUY: "
                    << ChatHelper::formatMoney(category->GetPricingStrategy()->GetBuyPrice(proto, auctionIds[auction]))
                    << " (" << category->GetDisplayName() << ")";
                sLog.outString("%s",out.str().c_str());
            }
        }
    }
}

void AhBot::CleanupPropositions()
{
    uint32 deliverTime = time(0) - 3600 * 24 * 2;
    auto result = CharacterDatabase.PQuery("select id, receiver from mail where subject like 'AH Proposition%%' and deliver_time <= '%u'", deliverTime);
    std::unique_ptr<QueryResult> result_guard(result);
    if (!result)
        return;

    int count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 id = fields[0].GetUInt32();
        uint32 receiver = fields[1].GetUInt32();
        Player *player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, receiver));
        if (player) player->RemoveMail(id);
        count++;
    } while (result->NextRow());

    if (count > 0)
    {
        CharacterDatabase.PExecute("delete from mail where subject like 'AH Proposition%%' and deliver_time <= '%u'", deliverTime);
        sLog.outBasic("%d old AH propositions removed", count);
    }
}

void AhBot::DeleteMail(std::list<uint32> buffer)
{
    std::ostringstream sql;
    sql << "delete from mail where id in ( ";
    bool first = true;
    for (std::list<uint32>::iterator j = buffer.begin(); j != buffer.end(); ++j)
    {
        if (first) first = false; else sql << ",";
        sql << "'" << *j << "'";
    }
    sql << ")";
    CharacterDatabase.Execute(sql.str().c_str());
}

uint64 AhBot::CacheKey(uint32 a, uint32 b)
{
    return (uint64(a) << 32) | uint64(b);
}

std::string AhBot::HistoryTimeKey(const std::string& category, uint32 id, uint32 faction, uint32 type)
{
    std::ostringstream out;
    out << category << '|' << id << '|' << faction << '|' << type;
    return out.str();
}

void AhBot::CommandReply(ChatHandler* handler, const std::string& line)
{
    sLog.outString("%s", line.c_str());
    if (handler)
        handler->SendSysMessage(line.c_str());
}

void AhBot::PrintStatus(ChatHandler* handler)
{
    std::ostringstream head;
    head << "AhBot enabled=" << (sAhBotConfig.enabled ? 1 : 0)
         << " seller=" << (sAhBotConfig.sellerEnabled ? 1 : 0)
         << " buyer=" << (sAhBotConfig.buyerEnabled ? 1 : 0)
         << " guid=" << (unsigned long long)sAhBotConfig.guid
         << " interval=" << sAhBotConfig.updateInterval << "s"
         << " itemsPerCycle=" << sAhBotConfig.itemsPerCycle;
    CommandReply(handler, head.str());

    std::ostringstream flags;
    flags << "stats=" << (sAhBotConfig.customPriceStatsEnabled ? 1 : 0)
          << " personas=" << (sAhBotConfig.sellerPersonasEnabled ? 1 : 0)
          << " undercut=" << (sAhBotConfig.undercuttingEnabled ? 1 : 0)
          << " maxActive=" << (sAhBotConfig.maxActiveEnabled ? 1 : 0)
          << " dynSupply=" << (sAhBotConfig.dynamicSupplyEnabled ? 1 : 0)
          << " vendorFloor=" << (sAhBotConfig.vendorFloorEnabled ? 1 : 0)
          << " stackRules=" << (sAhBotConfig.stackRulesEnabled ? 1 : 0);
    CommandReply(handler, flags.str());

    {
        std::lock_guard<std::mutex> g(cacheMutex);
        std::ostringstream cache;
        cache << "cache valid=" << (cycleCache.valid ? 1 : 0)
              << " market=" << cycleCache.marketRows
              << " priceStats=" << cycleCache.priceStatRows
              << " listingStats=" << cycleCache.listingStatRows
              << " historyTimes=" << cycleCache.historyTimes.size();
        CommandReply(handler, cache.str());
    }

    for (int i = 0; i < MAX_AUCTIONS; ++i)
    {
        const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[i]);
        uint32 count = 0;
        if (ahEntry)
            count = sAuctionMgr.GetAuctionsMap(ahEntry)->GetCount();
        uint32 target = 0;
        {
            std::lock_guard<std::mutex> g(cacheMutex);
            if (houseTargets.count(auctionIds[i]))
                target = houseTargets[auctionIds[i]];
        }
        std::ostringstream house;
        house << "house " << auctionIds[i] << " listings=" << count << " dailyTarget=" << target
              << " min=" << GetHouseMinItems(i) << " max=" << GetHouseMaxItems(i);
        CommandReply(handler, house.str());
        PrintStats(i, handler);
    }

    std::string errors;
    if (SelfCheck(errors))
        CommandReply(handler, "economy self-check: ok");
    else
        CommandReply(handler, std::string("economy self-check: ") + errors);
}

void AhBot::LoadCycleCache()
{
    CycleCache next;
    next.valid = true;

    if (auto results = CharacterDatabase.PQuery("SELECT item, price, auction_house FROM ahbot_price"))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            Field* fields = results->Fetch();
            uint32 itemId = fields[0].GetUInt32();
            double price = fields[1].GetFloat();
            uint32 house = fields[2].GetUInt32();
            next.marketPrices[CacheKey(itemId, house)] = price;
        } while (results->NextRow());
        next.marketRows = next.marketPrices.size();
    }

    if (auto results = CharacterDatabase.PQuery(
            "SELECT item, won, auction_house, category, MAX(buytime) FROM ahbot_history GROUP BY item, won, auction_house, category"))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            Field* fields = results->Fetch();
            uint32 itemId = fields[0].GetUInt32();
            uint32 won = fields[1].GetUInt32();
            uint32 faction = fields[2].GetUInt32();
            std::string category = fields[3].GetCppString();
            uint32 buytime = fields[4].GetUInt32();
            next.historyTimes[HistoryTimeKey(category, itemId, faction, won)] = buytime;
        } while (results->NextRow());
    }

    uint32 answerSince = time(0) - sAhBotConfig.itemBuyMaxInterval;
    if (auto results = CharacterDatabase.PQuery(
            "SELECT item, auction_house, COUNT(*) FROM ahbot_history WHERE won IN (2, 3) AND buytime > '%u' GROUP BY item, auction_house",
            answerSince))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            Field* fields = results->Fetch();
            next.answerCounts[CacheKey(fields[0].GetUInt32(), fields[1].GetUInt32())] = fields[2].GetUInt32();
        } while (results->NextRow());
    }

    if (auto results = CharacterDatabase.PQuery(
            "SELECT category, auction_house, COUNT(*) FROM (SELECT category, auction_house, ROUND(buytime/3600/24/5) AS days FROM ahbot_history WHERE won = '1' GROUP BY category, auction_house, days) q GROUP BY category, auction_house"))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            Field* fields = results->Fetch();
            std::ostringstream key;
            key << fields[0].GetCppString() << '|' << fields[1].GetUInt32();
            next.categoryDayCounts[key.str()] = fields[2].GetUInt32();
        } while (results->NextRow());
    }

    if (auto results = CharacterDatabase.PQuery(
            "SELECT item, auction_house, COUNT(*) FROM (SELECT item, auction_house, ROUND(buytime/3600/24/5) AS days FROM ahbot_history WHERE won = '1' GROUP BY item, auction_house, days) q GROUP BY item, auction_house"))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            Field* fields = results->Fetch();
            next.itemDayCounts[CacheKey(fields[0].GetUInt32(), fields[1].GetUInt32())] = fields[2].GetUInt32();
        } while (results->NextRow());
    }

    if (auto results = CharacterDatabase.PQuery(
            "SELECT auction_house, won, SUM(bid), MAX(buytime) FROM ahbot_history GROUP BY auction_house, won"))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            Field* fields = results->Fetch();
            uint32 faction = fields[0].GetUInt32();
            uint32 won = fields[1].GetUInt32();
            next.historyBidSum[faction * 10 + won] = fields[2].GetUInt64();
            if (won == AHBOT_WON_SELF)
                next.lastSelfBuyTime[faction] = fields[3].GetUInt32();
        } while (results->NextRow());
    }

    if (auto results = WorldDatabase.PQuery("SELECT item FROM npc_vendor WHERE maxcount = 0"))
    {
        std::unique_ptr<QueryResult> guard(results);
        do
        {
            uint32 itemId = results->Fetch()[0].GetUInt32();
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
            if (proto && proto->BuyPrice)
                next.vendorBuyPrice[itemId] = proto->BuyPrice;
        } while (results->NextRow());
    }

    if (sAhBotConfig.customPriceStatsEnabled)
    {
        if (auto results = CharacterDatabase.PQuery(
                "SELECT item_id, suffix_id, auction_house, sample_count, price_min, price_p10, price_p25, price_median, price_p75, price_p90, price_max FROM ahbot_price_stats"))
        {
            std::unique_ptr<QueryResult> guard(results);
            do
            {
                Field* fields = results->Fetch();
                PricePercentiles stats;
                ItemStatKey key;
                key.itemId = fields[0].GetUInt32();
                key.suffixId = fields[1].GetInt32();
                key.auctionHouse = fields[2].GetUInt32();
                stats.sampleCount = fields[3].GetUInt32();
                stats.priceMin = (uint32)std::min<uint64>(fields[4].GetUInt64(), 4294967295ull);
                stats.priceP10 = (uint32)std::min<uint64>(fields[5].GetUInt64(), 4294967295ull);
                stats.priceP25 = (uint32)std::min<uint64>(fields[6].GetUInt64(), 4294967295ull);
                stats.priceMedian = (uint32)std::min<uint64>(fields[7].GetUInt64(), 4294967295ull);
                stats.priceP75 = (uint32)std::min<uint64>(fields[8].GetUInt64(), 4294967295ull);
                stats.priceP90 = (uint32)std::min<uint64>(fields[9].GetUInt64(), 4294967295ull);
                stats.priceMax = (uint32)std::min<uint64>(fields[10].GetUInt64(), 4294967295ull);
                next.priceStats[key] = stats;
            } while (results->NextRow());
            next.priceStatRows = next.priceStats.size();
        }
    }

    if (sAhBotConfig.listingStatsEnabled)
    {
        if (auto results = CharacterDatabase.PQuery(
                "SELECT item_id, suffix_id, auction_house, snapshot_count, seen_count FROM ahbot_listing_stats"))
        {
            std::unique_ptr<QueryResult> guard(results);
            do
            {
                Field* fields = results->Fetch();
                ItemStatKey key;
                key.itemId = fields[0].GetUInt32();
                key.suffixId = fields[1].GetInt32();
                key.auctionHouse = fields[2].GetUInt32();
                next.listingSnapshots[key] = fields[3].GetUInt32();
                next.listingSeen[key] = fields[4].GetUInt32();
            } while (results->NextRow());
            next.listingStatRows = next.listingSeen.size();
        }
    }

    {
        std::lock_guard<std::mutex> g(cacheMutex);
        cycleCache = std::move(next);
    }
}

void AhBot::AssignSellerPersonas()
{
    sellerPersonas.clear();
    std::vector<uint32> ids(allBidders.begin(), allBidders.end());
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); ++i)
        sellerPersonas[ids[i]] = StablePersonaForIndex(i, sAhBotConfig.sellerPersonasEnabled);
}

SellerPersona AhBot::GetPersonaForBidder(uint32 guid) const
{
    std::map<uint32, SellerPersona>::const_iterator it = sellerPersonas.find(guid);
    if (it == sellerPersonas.end())
        return SellerPersona::Normal;
    return it->second;
}

HouseSnapshotIndex AhBot::BuildHouseIndex(const std::vector<AuctionSnapshot>& snaps) const
{
    HouseSnapshotIndex index;
    index.totalCount = (uint32)snaps.size();
    for (std::vector<AuctionSnapshot>::const_iterator itr = snaps.begin(); itr != snaps.end(); ++itr)
    {
        if (itr->itemCount && itr->buyout)
        {
            uint32 unit = PerUnitPrice(itr->buyout, itr->itemCount);
            std::map<uint32, uint32>::iterator existing = index.lowestBuyoutPerUnit.find(itr->itemTemplate);
            if (existing == index.lowestBuyoutPerUnit.end() || unit < existing->second)
                index.lowestBuyoutPerUnit[itr->itemTemplate] = unit;
        }
        if (IsBotAuction(itr->owner))
            index.botActiveCount[itr->itemTemplate]++;
    }
    return index;
}

uint32 AhBot::GetHouseMinItems(int auction) const
{
    if (auction == 0) return sAhBotConfig.allianceMinItems;
    if (auction == 1) return sAhBotConfig.hordeMinItems;
    return sAhBotConfig.neutralMinItems;
}

uint32 AhBot::GetHouseMaxItems(int auction) const
{
    if (auction == 0) return sAhBotConfig.allianceMaxItems;
    if (auction == 1) return sAhBotConfig.hordeMaxItems;
    return sAhBotConfig.neutralMaxItems;
}

uint32 AhBot::GetHouseTargetPercent(int auction) const
{
    if (auction == 0) return sAhBotConfig.allianceTargetPercent;
    if (auction == 1) return sAhBotConfig.hordeTargetPercent;
    return sAhBotConfig.neutralTargetPercent;
}

uint32 AhBot::ResolveHouseTarget(int auction, uint32 currentCount)
{
    (void)currentCount;
    uint32 minItems = GetHouseMinItems(auction);
    uint32 maxItems = GetHouseMaxItems(auction);
    if (minItems == 0 && maxItems == 0)
        return 0;

    uint32 houseId = auctionIds[auction];
    int32 today = (int32)(time(0) / 86400);
    uint32 savedTarget = 0;
    int32 savedDay = -1;

    if (auto results = CharacterDatabase.PQuery(
            "SELECT last_roll_day, target_items FROM ahbot_house_target WHERE auction_house = '%u'", houseId))
    {
        std::unique_ptr<QueryResult> guard(results);
        Field* fields = results->Fetch();
        savedDay = fields[0].GetInt32();
        savedTarget = fields[1].GetUInt32();
    }

    uint32 target;
    if (savedDay == today && savedTarget > 0)
        target = savedTarget;
    else
        target = RollDailyHouseTarget(minItems, maxItems, GetHouseTargetPercent(auction), urand(0, 0x7fffffff));

    {
        std::lock_guard<std::mutex> g(cacheMutex);
        houseTargets[houseId] = target;
    }
    if (!dryRun)
        CharacterDatabase.PExecute(
            "INSERT INTO ahbot_house_target (auction_house, last_roll_day, target_items) VALUES ('%u', '%d', '%u') "
            "ON DUPLICATE KEY UPDATE last_roll_day = VALUES(last_roll_day), target_items = VALUES(target_items)",
            houseId, today, target);
    return target;
}

uint32 AhBot::ChooseListingStack(const ItemPrototype* proto, Category* category)
{
    if (!sAhBotConfig.stackRulesEnabled)
        return urand(1, category->GetStackCount(proto));

    std::string key = ItemClassKey(proto->Class);
    uint32 ratio = (uint32)sAhBotConfig.GetStackRatio(key);
    uint32 increment = (uint32)sAhBotConfig.GetStackIncrement(key);
    uint32 stackMax = (uint32)sAhBotConfig.GetStackMax(key);
    return ChooseStackCount(proto->GetMaxStackSize(), ratio, increment, stackMax, urand(0, 99), urand(0, 0x7fffffff));
}

uint32 AhBot::ApplySellPriceAdjustments(const ItemPrototype* proto, uint32 unitPrice, uint32 owner, const HouseSnapshotIndex& index, Category* category, uint32 auctionHouse)
{
    (void)category;
    uint32 price = unitPrice ? unitPrice : 1;
    PricePercentiles stats;
    const PricePercentiles* statsPtr = TryGetPriceStats(proto->ItemId, auctionHouse, stats) ? &stats : nullptr;
    uint32 floorPrice = SellerPriceFloor(proto->SellPrice, statsPtr, sAhBotConfig.customPriceStatsMinSampleCount, sAhBotConfig.priceFloorStatsMultiplier);

    uint32 lowest = 0;
    std::map<uint32, uint32>::const_iterator lowIt = index.lowestBuyoutPerUnit.find(proto->ItemId);
    if (lowIt != index.lowestBuyoutPerUnit.end())
        lowest = lowIt->second;

    uint32 greedyLow = 0, greedyHigh = 0;
    if (statsPtr)
    {
        greedyLow = stats.priceP75;
        greedyHigh = stats.priceP90;
    }

    price = ApplySellerPersonaPrice(GetPersonaForBidder(owner), price, lowest, floorPrice,
        sAhBotConfig.undercuttingEnabled, sAhBotConfig.undercutPercentMin, sAhBotConfig.undercutPercentMax,
        urand(0, 0x7fffffff), urand(1, 100), urand(80, 95), greedyLow, greedyHigh, sAhBotConfig.maxBuyoutPrice);

    if (sAhBotConfig.vendorFloorEnabled || sAhBotConfig.maxBuyoutPrice)
        price = ApplyVendorFloorAndMax(price, proto->SellPrice, sAhBotConfig.vendorFloorEnabled,
            sAhBotConfig.vendorFloorAddPercent, sAhBotConfig.maxBuyoutPrice);

    return price ? price : 1;
}

bool AhBot::HasWeightedProportions() const
{
    for (int i = 0; i < CategoryList::instance.size(); ++i)
    {
        if (sAhBotConfig.GetListProportion(CategoryList::instance[i]->GetDisplayName()) > 0)
            return true;
    }
    return false;
}

Category* AhBot::PickWeightedCategory()
{
    std::vector<uint32> weights;
    weights.reserve(CategoryList::instance.size());
    for (int i = 0; i < CategoryList::instance.size(); ++i)
    {
        int32 p = sAhBotConfig.GetListProportion(CategoryList::instance[i]->GetDisplayName());
        weights.push_back(p > 0 ? (uint32)p : 0);
    }
    uint32 idx = WeightedPick(weights, urand(0, 0x7fffffff));
    return CategoryList::instance[idx];
}

std::string AhBot::ItemClassKey(uint32 itemClass) const
{
    switch (itemClass)
    {
        case ITEM_CLASS_CONSUMABLE: return "consumable";
        case ITEM_CLASS_CONTAINER: return "container";
        case ITEM_CLASS_WEAPON: return "weapon";
        case ITEM_CLASS_ARMOR: return "armor";
        case ITEM_CLASS_REAGENT: return "reagent";
        case ITEM_CLASS_PROJECTILE: return "projectile";
        case ITEM_CLASS_TRADE_GOODS: return "trade";
        case ITEM_CLASS_RECIPE: return "recipe";
        case ITEM_CLASS_QUIVER: return "quiver";
        case ITEM_CLASS_QUEST: return "quest";
        default: return "misc";
    }
}

bool AhBot::HasCycleCache()
{
    std::lock_guard<std::mutex> g(cacheMutex);
    return cycleCache.valid;
}

bool AhBot::TryGetCachedMarketPrice(uint32 itemId, uint32 auctionHouse, double& outPrice)
{
    std::lock_guard<std::mutex> g(cacheMutex);
    if (!cycleCache.valid)
        return false;
    std::map<uint64, double>::const_iterator it = cycleCache.marketPrices.find(CacheKey(itemId, auctionHouse));
    if (it == cycleCache.marketPrices.end())
        return false;
    outPrice = it->second;
    return true;
}

uint32 AhBot::GetCachedCategoryDayCount(const std::string& category, uint32 faction)
{
    std::lock_guard<std::mutex> g(cacheMutex);
    if (!cycleCache.valid)
        return 0;
    std::ostringstream key;
    key << category << '|' << faction;
    std::map<std::string, uint32>::const_iterator it = cycleCache.categoryDayCounts.find(key.str());
    return it == cycleCache.categoryDayCounts.end() ? 0 : it->second;
}

uint32 AhBot::GetCachedItemDayCount(uint32 itemId, uint32 faction)
{
    std::lock_guard<std::mutex> g(cacheMutex);
    if (!cycleCache.valid)
        return 0;
    std::map<uint64, uint32>::const_iterator it = cycleCache.itemDayCounts.find(CacheKey(itemId, faction));
    return it == cycleCache.itemDayCounts.end() ? 0 : it->second;
}

bool AhBot::TryGetPriceStats(uint32 itemId, uint32 auctionHouse, PricePercentiles& outStats, int32 suffixId)
{
    std::lock_guard<std::mutex> g(cacheMutex);
    if (!cycleCache.valid)
        return false;
    ItemStatKey exact = MakeStatKey(itemId, suffixId, auctionHouse);
    std::map<ItemStatKey, PricePercentiles>::const_iterator it = cycleCache.priceStats.find(exact);
    if (it == cycleCache.priceStats.end() && suffixId != 0)
        it = cycleCache.priceStats.find(MakeStatKey(itemId, 0, auctionHouse));
    if (it == cycleCache.priceStats.end())
        return false;
    outStats = it->second;
    return true;
}

ItemStatKey AhBot::MakeStatKey(uint32 itemId, int32 suffixId, uint32 auctionHouse)
{
    ItemStatKey key;
    key.itemId = itemId;
    key.suffixId = suffixId;
    key.auctionHouse = auctionHouse;
    return key;
}

void AhBot::InvalidateCycleCache()
{
    std::lock_guard<std::mutex> g(cacheMutex);
    cycleCache = CycleCache();
}

void AhBot::ExecuteListing(const PendingListing& p)
{
    const AuctionHouseEntry* ahEntry = sAuctionHouseStore.LookupEntry(auctionIds[p.houseIndex]);
    if (!ahEntry)
        return;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(p.itemId);
    if (!proto)
        return;

    Item* item = Item::CreateItem(p.itemId, p.stackCount);
    if (!item)
        return;

    uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(p.itemId);
    if (randomPropertyId)
        item->SetItemRandomProperties(randomPropertyId);
    item->ClearUpdateMask(false);

    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(ahEntry);
    AuctionEntry* auctionEntry = new AuctionEntry;
    auctionEntry->Id = sObjectMgr.GenerateAuctionID();
    auctionEntry->itemGuidLow = item->GetObjectGuid().GetCounter();
    auctionEntry->itemTemplate = item->GetEntry();
    auctionEntry->itemCount = item->GetCount();
    auctionEntry->itemRandomPropertyId = item->GetItemRandomPropertyId();
    auctionEntry->owner = p.owner;
    auctionEntry->startbid = p.bidPrice;
    auctionEntry->bidder = 0;
    auctionEntry->bid = 0;
    auctionEntry->buyout = p.buyoutPrice;
    auctionEntry->expireTime = time(nullptr) + p.auctionTime;
    auctionEntry->deposit = 0;
    auctionEntry->auctionHouseEntry = ahEntry;

    {
        AuctionHouseObject::Guard g(auctionHouse->GetLock());
        auctionHouse->AddAuction(auctionEntry);
        sAuctionMgr.AddAItem(item);
    }

    item->SaveToDB();
    auctionEntry->SaveToDB();

    std::string name;
    sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, p.owner), name);
    sLog.outString("[AhBot] Listed: %dx %s on AH %u for %ug%us..%ug%us (owner: %s guid=%u)",
        p.stackCount, proto->Name1.c_str(), auctionIds[p.houseIndex],
        p.bidPrice / 10000, (p.bidPrice % 10000) / 100,
        p.buyoutPrice / 10000, (p.buyoutPrice % 10000) / 100,
        name.c_str(), p.owner);
}

uint32 AhBot::GetVendorBuyPrice(uint32 itemId)
{
    std::lock_guard<std::mutex> g(cacheMutex);
    std::map<uint32, uint32>::const_iterator it = cycleCache.vendorBuyPrice.find(itemId);
    return it == cycleCache.vendorBuyPrice.end() ? 0 : it->second;
}

INSTANTIATE_SINGLETON_1( ahbot::AhBot );
