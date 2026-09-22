#pragma once

#include "Config/Config.h"
#include "AhBotEconomy.h"
#include <set>
#include <string>
#include <vector>

class AhBotConfig
{
public:
    AhBotConfig();
    static AhBotConfig& instance()
    {
        static AhBotConfig instance;
        return instance;
    }

public:
    bool Initialize();
    bool Reload();

    bool enabled;
    bool sellerEnabled;
    bool buyerEnabled;
    uint32 updateInterval;
    uint32 historyDays, maxSellInterval;
    uint32 itemBuyMinInterval, itemBuyMaxInterval;
    uint32 itemSellMinInterval, itemSellMaxInterval;
    uint32 alwaysAvailableMoney;
    float priceMultiplier, priceQualityMultiplier;
    uint32 defaultMinPrice, stackReducePrice;
    uint32 maxItemLevel, maxRequiredLevel;
    float underPriceProbability;
    std::set<uint32> ignoreItemIds;
    std::set<uint32> ignoreVendorItemIds;
    bool sendmail;

    uint32 itemsPerCycle;

    uint32 customPriceStatsMinSampleCount;
    uint32 customPriceStatsBuyerMaxAcceptedPercentile;

    uint32 listingStatsMinSeenCount;
    float listingStatsFallbackWeight;
    float listingStatsScarcityExponent;

    bool maxActiveEnabled;
    uint32 maxActiveDefault;
    bool dynamicSupplyEnabled;
    bool dynamicSupplyDebug;

    bool sellerPersonasEnabled;
    bool undercuttingEnabled;
    float undercutPercentMin;
    float undercutPercentMax;
    float priceFloorStatsMultiplier;

    uint32 maxBuyoutPrice;
    float buyoutVariationReducePercent;
    float buyoutVariationAddPercent;
    float bidVariationHighReducePercent;
    float bidVariationLowReducePercent;
    bool vendorFloorEnabled;
    float vendorFloorAddPercent;
    bool stackRulesEnabled;

    uint32 buyerCandidatesMin;
    uint32 buyerCandidatesMax;
    float buyerAcceptablePriceModifier;
    bool buyerAlwaysBidMax;
    bool buyerPreventOverpayVendor;
    bool buyerWillBidAgainstPlayers;

    uint32 listingExpireMinSeconds;
    uint32 listingExpireMaxSeconds;
    bool realismDebug;

    float GetSellPriceMultiplier(std::string)
    {
        return 1.0f;
    }

    float GetBuyPriceMultiplier(std::string)
    {
        return 1.0f;
    }

    float GetItemPriceMultiplier(std::string)
    {
        return 1.0f;
    }

    int32 GetMaxAllowedAuctionCount(std::string, int32)
    {
        // Total market population is the only listing-volume control. A fixed
        // per-bucket ceiling keeps a single category from consuming it all.
        return 30;
    }

    int32 GetMaxAllowedItemAuctionCount(std::string, int32 default_value)
    {
        return default_value;
    }

    int32 GetListProportion(std::string)
    {
        return 0;
    }

    int32 GetMaxActiveForCategory(std::string)
    {
        return maxActiveDefault;
    }

    int32 GetEmptyMarketChance(std::string)
    {
        return 0;
    }

    int32 GetStackRatio(std::string)
    {
        return 0;
    }

    int32 GetStackIncrement(std::string)
    {
        return 1;
    }

    int32 GetStackMax(std::string)
    {
        return 0;
    }

private:
    Config config;
};

#define sAhBotConfig MaNGOS::Singleton<AhBotConfig>::Instance()
