#pragma once

#include "Config/Config.h"
#include "AhBotEconomy.h"
#include <map>
#include <set>
#include <sstream>
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
    uint64 guid;
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
    uint32 allianceMinItems, allianceMaxItems, allianceTargetPercent;
    uint32 hordeMinItems, hordeMaxItems, hordeTargetPercent;
    uint32 neutralMinItems, neutralMaxItems, neutralTargetPercent;

    bool customPriceStatsEnabled;
    uint32 customPriceStatsMinSampleCount;
    uint32 customPriceStatsBuyerMaxAcceptedPercentile;

    bool listingStatsEnabled;
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

    float GetSellPriceMultiplier(std::string category)
    {
        return GetCategoryParameter(sellPriceMultipliers, "PriceMultiplier.Sell", category, 1.0f);
    }

    float GetBuyPriceMultiplier(std::string category)
    {
        return GetCategoryParameter(buyPriceMultipliers, "PriceMultiplier.Buy", category, 1.0f);
    }

    float GetItemPriceMultiplier(std::string name)
    {
        return GetCategoryParameter(itemPriceMultipliers, "PriceMultiplier.Item", name, 1.0f);
    }

    int32 GetMaxAllowedAuctionCount(std::string category, int32 default_value)
    {
        return (int32)GetCategoryParameter(maxAuctionCount, "MaxAuctionCount", category, default_value);
    }

    int32 GetMaxAllowedItemAuctionCount(std::string category, int32 default_value)
    {
        return (int32)GetCategoryParameter(maxItemAuctionCount, "MaxItemTypeCount", category, default_value);
    }

    int32 GetListProportion(std::string category)
    {
        return (int32)GetCategoryParameter(listProportions, "ListProportion", category, 0.0f);
    }

    int32 GetMaxActiveForCategory(std::string category)
    {
        return (int32)GetCategoryParameter(maxActiveByCategory, "MaxActive", category, (float)maxActiveDefault);
    }

    int32 GetEmptyMarketChance(std::string category)
    {
        return (int32)GetCategoryParameter(emptyMarketChance, "DynamicSupply.EmptyMarketChance", category, 0.0f);
    }

    int32 GetStackRatio(std::string classKey)
    {
        return (int32)GetCategoryParameter(stackRatio, "Stack.RandomRatio", classKey, 0.0f);
    }

    int32 GetStackIncrement(std::string classKey)
    {
        return (int32)GetCategoryParameter(stackIncrement, "Stack.Increment", classKey, 1.0f);
    }

    int32 GetStackMax(std::string classKey)
    {
        return (int32)GetCategoryParameter(stackMax, "Stack.Max", classKey, 0.0f);
    }

    std::string GetStringDefault(const char* name, const char* def)
    {
        return config.GetStringDefault(name, def);
    }

    bool GetBoolDefault(const char* name, const bool def = false)
    {
        return config.GetBoolDefault(name, def);
    }

    int32 GetIntDefault(const char* name, const int32 def)
    {
        return config.GetIntDefault(name, def);
    }

    float GetFloatDefault(const char* name, const float def)
    {
        return config.GetFloatDefault(name, def);
    }

    void ParseMinMax(const std::string& value, uint32& outMin, uint32& outMax, uint32 defaultValue);

private:
    float GetCategoryParameter(std::map<std::string, float>& cache, std::string type, std::string category, float defaultValue)
    {
        if (cache.find(category) == cache.end())
        {
            std::ostringstream out; out << "AhBot."<< type << "." << category;
            cache[category] = config.GetFloatDefault(out.str().c_str(), defaultValue);
        }

        return cache[category];
    }

    void ClearCategoryCaches()
    {
        sellPriceMultipliers.clear();
        buyPriceMultipliers.clear();
        itemPriceMultipliers.clear();
        maxAuctionCount.clear();
        maxItemAuctionCount.clear();
        listProportions.clear();
        maxActiveByCategory.clear();
        emptyMarketChance.clear();
        stackRatio.clear();
        stackIncrement.clear();
        stackMax.clear();
    }

private:
    Config config;
    std::map<std::string, float> sellPriceMultipliers;
    std::map<std::string, float> buyPriceMultipliers;
    std::map<std::string, float> itemPriceMultipliers;
    std::map<std::string, float> maxAuctionCount;
    std::map<std::string, float> maxItemAuctionCount;
    std::map<std::string, float> listProportions;
    std::map<std::string, float> maxActiveByCategory;
    std::map<std::string, float> emptyMarketChance;
    std::map<std::string, float> stackRatio;
    std::map<std::string, float> stackIncrement;
    std::map<std::string, float> stackMax;
};

#define sAhBotConfig MaNGOS::Singleton<AhBotConfig>::Instance()
