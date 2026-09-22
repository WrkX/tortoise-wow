
#include "AhBotConfig.h"
#include "SystemConfig.h"
#include <algorithm>
std::vector<std::string> split(const std::string &s, char delim);

INSTANTIATE_SINGLETON_1(AhBotConfig);

AhBotConfig::AhBotConfig()
{
}

template <class T>
void LoadSet(std::string value, T &res)
{
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        uint32 id = atoi((*i).c_str());
        if (!id)
            continue;

        res.insert(id);
    }
}

bool AhBotConfig::Initialize()
{
    ignoreItemIds.clear();
    ignoreVendorItemIds.clear();

    if (!config.SetSource(SYSCONFDIR"ahbot.conf", "AHBot_"))
    {
        sLog.outString("AhBot is Disabled. Unable to open configuration file ahbot.conf");
        enabled = false;
        sellerEnabled = false;
        buyerEnabled = false;
        return false;
    }

    enabled = config.GetBoolDefault("AhBot.Enabled", true);

    if (!enabled)
        sLog.outString("AhBot is Disabled in ahbot.conf");

    updateInterval = 900;
    historyDays = 30;
    itemBuyMinInterval = 600;
    itemBuyMaxInterval = 7200;
    if (itemBuyMaxInterval && itemBuyMinInterval > itemBuyMaxInterval)
        std::swap(itemBuyMinInterval, itemBuyMaxInterval);
    itemSellMinInterval = 60;
    itemSellMaxInterval = 300;
    if (itemSellMaxInterval && itemSellMinInterval > itemSellMaxInterval)
        std::swap(itemSellMinInterval, itemSellMaxInterval);
    maxSellInterval = 3600 * 8;
    alwaysAvailableMoney = 2000000;
    priceMultiplier = 0.35f;
    defaultMinPrice = 20;
    maxItemLevel = 500;
    maxRequiredLevel = 80;
    stackReducePrice = 1000000;
    priceQualityMultiplier = 1.0f;
    underPriceProbability = 0.05f;
    LoadSet<std::set<uint32> >("49283,52200,8494,6345,6891,2460,37164,34835,17,2248", ignoreItemIds);
    LoadSet<std::set<uint32> >("755,858,4592,4593,1710,3827,2455,3385", ignoreVendorItemIds);
    sendmail = true;

    // These are operational controls; all market-policy settings are fixed.
    sellerEnabled = config.GetBoolDefault("AhBot.Seller.Enabled", true);
    buyerEnabled = config.GetBoolDefault("AhBot.Buyer.Enabled", true);

    itemsPerCycle = (uint32)std::max(0, config.GetIntDefault("AhBot.ItemsPerCycle", 100));

    // The maintained market-stat import is the source of truth. These are
    // deliberately fixed so pricing and scarcity never drift from the SQL
    // generator through per-server configuration.
    customPriceStatsMinSampleCount = 3;
    customPriceStatsBuyerMaxAcceptedPercentile = 90;
    listingStatsMinSeenCount = 2;
    listingStatsFallbackWeight = 1.0f;
    listingStatsScarcityExponent = 1.25f;

    maxActiveEnabled = false;
    maxActiveDefault = 2;
    dynamicSupplyEnabled = false;
    dynamicSupplyDebug = false;

    sellerPersonasEnabled = false;
    undercuttingEnabled = false;
    undercutPercentMin = 1.0f;
    undercutPercentMax = 5.0f;
    priceFloorStatsMultiplier = 0.65f;

    maxBuyoutPrice = 0;
    buyoutVariationReducePercent = 0.0f;
    buyoutVariationAddPercent = 0.0f;
    bidVariationHighReducePercent = 0.0f;
    bidVariationLowReducePercent = 0.0f;
    vendorFloorEnabled = false;
    vendorFloorAddPercent = 0.25f;
    stackRulesEnabled = false;

    buyerCandidatesMin = 0;
    buyerCandidatesMax = 0;
    buyerAcceptablePriceModifier = 1.0f;
    buyerAlwaysBidMax = false;
    buyerPreventOverpayVendor = false;
    buyerWillBidAgainstPlayers = true;

    listingExpireMinSeconds = 0;
    listingExpireMaxSeconds = 0;

    realismDebug = config.GetBoolDefault("AhBot.DEBUG", false);

    return enabled;
}

bool AhBotConfig::Reload()
{
    return Initialize();
}
