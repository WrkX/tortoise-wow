#pragma once

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "Category.h"
#include "ItemBag.h"
#include "AhBotEconomy.h"
#include "playerbot/PlayerbotAIBase.h"
#include "AuctionHouse/AuctionHouseMgr.h"
#include "ObjectGuid.h"
#include "WorldSession.h"


#define MAX_AUCTIONS 3
#define AHBOT_WON_EXPIRE 0
#define AHBOT_WON_PLAYER 1
#define AHBOT_WON_SELF 2
#define AHBOT_WON_BID 3
#define AHBOT_WON_DELAY 4
#define AHBOT_SELL_DELAY 5
#define AHBOT_SENDMAIL 6

class ChatHandler;

namespace ahbot
{
    struct ItemStatKey
    {
        uint32 itemId = 0;
        int32 suffixId = 0;
        uint32 auctionHouse = 0;

        bool operator<(const ItemStatKey& other) const
        {
            if (itemId != other.itemId)
                return itemId < other.itemId;
            if (suffixId != other.suffixId)
                return suffixId < other.suffixId;
            return auctionHouse < other.auctionHouse;
        }
    };

    struct CycleCache
    {
        bool valid = false;
        std::map<uint64, double> marketPrices;
        std::map<std::string, uint32> historyTimes;
        std::map<uint64, uint32> answerCounts;
        std::map<std::string, uint32> categoryDayCounts;
        std::map<uint64, uint32> itemDayCounts;
        std::map<uint32, uint32> vendorBuyPrice;
        std::map<ItemStatKey, PricePercentiles> priceStats;
        std::map<ItemStatKey, uint32> listingSeen;
        std::map<ItemStatKey, uint32> listingSnapshots;
        std::map<uint32, uint64> historyBidSum; // key: faction * 10 + won
        std::map<uint32, uint32> lastSelfBuyTime; // faction
        std::map<uint32, int64> availableMoney;
        size_t priceStatRows = 0;
        size_t listingStatRows = 0;
        size_t marketRows = 0;
    };

    struct HouseSnapshotIndex
    {
        std::map<uint32, uint32> lowestBuyoutPerUnit;
        std::map<uint32, uint32> botActiveCount;
        uint32 totalCount = 0;
    };

    class AhBot
    {
    public:
        AhBot() : nextAICheckTime(0), updating(false), dryRun(false), pendingSimulate(false) {}
        virtual ~AhBot();
        static AhBot& instance()
        {
            static AhBot instance;
            return instance;
        }

    public:
        static bool HandleAhBotCommand(ChatHandler* handler, char const* args);
        ObjectGuid GetAHBplayerGUID();
        void Init();
        void Update();
        void ForceUpdate(bool simulate = false);
        void RequestUpdate(bool simulate = false);
        void HandleCommand(std::string command, ChatHandler* handler = nullptr);
        void Won(AuctionEntry* entry) { AddToHistory(entry); }
        void Expired(AuctionEntry* entry) {}

        double GetCategoryMultiplier(std::string category)
        {
            return categoryMultipliers[category] ? categoryMultipliers[category] : 1;
        }

        int32 GetSellPrice(const ItemPrototype* proto);
        int32 GetBuyPrice(const ItemPrototype* proto);
        double GetRarityPriceMultiplier(const ItemPrototype* proto);
        bool IsUsedBySkill(const ItemPrototype* proto, uint32 skillId);

        bool TryGetCachedMarketPrice(uint32 itemId, uint32 auctionHouse, double& outPrice);
        bool HasCycleCache();
        uint32 GetCachedCategoryDayCount(const std::string& category, uint32 faction);
        uint32 GetCachedItemDayCount(uint32 itemId, uint32 faction);
        bool TryGetPriceStats(uint32 itemId, uint32 auctionHouse, PricePercentiles& outStats, int32 suffixId = 0);
        uint32 GetVendorBuyPrice(uint32 itemId);
        bool IsDryRun() const { return dryRun; }

    private:
        int Answer(int auction, Category* category, ItemBag* inAuctionItems, const HouseSnapshotIndex& index);
        int AddAuctions(int auction, Category* category, ItemBag* inAuctionItems, const HouseSnapshotIndex& index, int& remainingCycle);
        int AddAuction(int auction, Category* category, const ItemPrototype* proto, const HouseSnapshotIndex& index);
        void Expire(int auction);
        void PrintStats(int auction, ChatHandler* handler);
        void AddToHistory(AuctionEntry* entry, uint32 won = 0);
        void CleanupHistory();
        uint32 GetAvailableMoney(uint32 auctionHouse);
        void CheckCategoryMultipliers();
        void updateMarketPrice(uint32 itemId, double price, uint32 auctionHouse);
        bool IsBotAuction(uint32 bidder) const;
        uint32 GetRandomBidder(uint32 auctionHouse);
        void LoadRandomBots();
        uint32 GetAnswerCount(uint32 itemId, uint32 auctionHouse, uint32 withinTime);
        // These work off AuctionSnapshot rather than live AuctionEntry pointers:
        // the bot runs on its own thread and the world thread deletes entries
        // underneath it. See the comment on AuctionSnapshot in AuctionHouseMgr.h.
        std::vector<AuctionSnapshot> LoadAuctions(const std::vector<AuctionSnapshot>& auctionEntryMap, Category*& category,
                int& auction);
        void FindMinPrice(const std::vector<AuctionSnapshot>& auctionEntryMap, const AuctionSnapshot& entry, uint32 itemId, uint32 itemCount, uint32* minBid,
                uint32* minBuyout);
        uint32 GetBuyTime(uint32 entry, uint32 itemId, uint32 auctionHouse, Category*& category, double priceLevel);
        uint32 GetTime(std::string category, uint32 id, uint32 auctionHouse, uint32 type);
        void SetTime(std::string category, uint32 id, uint32 auctionHouse, uint32 type, uint32 value);
        uint32 GetSellTime(uint32 itemId, uint32 auctionHouse, Category*& category);
        void CheckSendMail(uint32 bidder, uint32 price, const AuctionSnapshot& entry);
        bool TryEquipItem(uint32 bidder, uint32 itemGuidLow, ItemPrototype const* proto);
        void Dump();
        void CleanupPropositions();
        void DeleteMail(std::list<uint32> buffer);

        void LoadCycleCache();
        void AssignSellerPersonas();
        SellerPersona GetPersonaForBidder(uint32 guid) const;
        HouseSnapshotIndex BuildHouseIndex(const std::vector<AuctionSnapshot>& snaps) const;
        uint32 ResolveHouseTarget(int auction, uint32 currentCount);
        uint32 GetHouseMinItems(int auction) const;
        uint32 GetHouseMaxItems(int auction) const;
        uint32 GetHouseTargetPercent(int auction) const;
        uint32 ChooseListingStack(const ItemPrototype* proto, Category* category);
        uint32 ApplySellPriceAdjustments(const ItemPrototype* proto, uint32 unitPrice, uint32 owner, const HouseSnapshotIndex& index, Category* category, uint32 auctionHouse);
        void CommandReply(ChatHandler* handler, const std::string& line);
        void PrintStatus(ChatHandler* handler);
        bool HasWeightedProportions() const;
        Category* PickWeightedCategory();
        std::string ItemClassKey(uint32 itemClass) const;
        static uint64 CacheKey(uint32 a, uint32 b);
        static std::string HistoryTimeKey(const std::string& category, uint32 id, uint32 faction, uint32 type);
        static ItemStatKey MakeStatKey(uint32 itemId, int32 suffixId, uint32 auctionHouse);
        void InvalidateCycleCache();
        void StartWorker();

    public:
        // Work the bot thread decides on but must not carry out itself.
        // Completing a purchase sends mail, posting a listing mutates the
        // live auction map, and both belong to the world thread.
        // AhBot::Update() already runs there (World::UpdatePlayerbotsTick), so
        // the bot thread only records the decision and RunQueuedWork() carries
        // it out.
        struct PendingPurchase
        {
            uint32 auctionId;
            uint32 bidder;
            uint32 bidAmount;
            uint32 unitPrice;       // for the buyout heuristic
            uint32 minBuyout;       // cheapest comparable listing, 0 if none
            int    houseIndex;      // index into auctionIds[]
        };

        struct PendingProposition
        {
            uint32 auctionId;
            uint32 owner;
            uint32 itemGuidLow;
            uint32 bidder;
            uint32 price;
            uint32 houseId;
            time_t expireTime;
        };

        struct PendingListing
        {
            int    houseIndex;
            uint32 owner;
            uint32 itemId;
            uint32 stackCount;
            uint32 bidPrice;
            uint32 buyoutPrice;
            uint32 auctionTime;
        };

        void RunQueuedWork();                                   // world thread only
        void ExecutePurchase(const PendingPurchase& p);         // world thread only
        void ExecuteProposition(const PendingProposition& p);   // world thread only
        void ExecuteListing(const PendingListing& p);           // world thread only

        static uint32 auctionIds[MAX_AUCTIONS];
        static uint32 auctioneers[MAX_AUCTIONS];
        static std::map<uint32, uint32> factions;

    private:
        AvailableItemsBag availableItems;
        time_t nextAICheckTime;
        std::map<std::string, double> categoryMultipliers;
        std::map<std::string, uint32> categoryMaxAuctionCount;
        std::map<std::string, uint32> categoryMaxItemAuctionCount;
        std::map<std::string, uint64> categoryMultiplierExpireTimes;
        std::map<uint32, std::vector<uint32>> bidders;
        std::set<uint32> allBidders;
        std::atomic<bool> updating;
        std::mutex queuedWorkMutex;
        std::vector<PendingPurchase> queuedPurchases;
        std::vector<PendingProposition> queuedPropositions;
        std::vector<PendingListing> queuedListings;
        std::mutex cacheMutex;
        CycleCache cycleCache;
        std::map<uint32, SellerPersona> sellerPersonas;
        std::map<uint32, uint32> houseTargets;
        bool dryRun;
        std::atomic<bool> pendingSimulate;
        std::mutex workerMutex;
        std::thread workerThread;
    };
};

#define auctionbot MaNGOS::Singleton<ahbot::AhBot>::Instance()
