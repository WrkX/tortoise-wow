
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

void AhBotConfig::ParseMinMax(const std::string& value, uint32& outMin, uint32& outMax, uint32 defaultValue)
{
    outMin = defaultValue;
    outMax = defaultValue;
    if (value.empty())
        return;

    std::string::size_type colon = value.find(':');
    if (colon == std::string::npos)
        colon = value.find('-');

    if (colon == std::string::npos)
    {
        int32 parsed = atoi(value.c_str());
        if (parsed < 0)
            parsed = 0;
        outMin = outMax = (uint32)parsed;
        return;
    }

    int32 lo = atoi(value.substr(0, colon).c_str());
    int32 hi = atoi(value.substr(colon + 1).c_str());
    if (lo < 0)
        lo = 0;
    if (hi < 0)
        hi = 0;
    if ((uint32)hi < (uint32)lo)
        std::swap(lo, hi);
    outMin = (uint32)lo;
    outMax = (uint32)hi;
}

bool AhBotConfig::Initialize()
{
    ClearCategoryCaches();
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

    guid = (uint64)config.GetIntDefault("AhBot.GUID", 0);
    updateInterval = config.GetIntDefault("AhBot.UpdateIntervalInSeconds", 900);
    historyDays = config.GetIntDefault("AhBot.History.Days", 30);
    itemBuyMinInterval = config.GetIntDefault("AhBot.ItemBuyMinInterval", 600);
    itemBuyMaxInterval = config.GetIntDefault("AhBot.ItemBuyMaxInterval", 7200);
    itemSellMinInterval = config.GetIntDefault("AhBot.ItemSellMinInterval", 600);
    itemSellMaxInterval = config.GetIntDefault("AhBot.ItemSellMaxInterval", 7200);
    maxSellInterval = config.GetIntDefault("AhBot.MaxSellInterval", 3600 * 8);
    alwaysAvailableMoney = config.GetIntDefault("AhBot.AlwaysAvailableMoney", 200000);
    priceMultiplier = config.GetFloatDefault("AhBot.PriceMultiplier", 1.0f);
    defaultMinPrice = config.GetIntDefault("AhBot.DefaultMinPrice", 20);
    maxItemLevel = config.GetIntDefault("AhBot.MaxItemLevel", 199);
    maxRequiredLevel = config.GetIntDefault("AhBot.MaxRequiredLevel", 80);
    stackReducePrice = config.GetIntDefault("AhBot.StackReducePrice", 1000000);
    priceQualityMultiplier = config.GetFloatDefault("AhBot.PriceQualityMultiplier", 1.0f);
    underPriceProbability = config.GetFloatDefault("AhBot.UnderPriceProbability", 0.05f);
    LoadSet<std::set<uint32> >(config.GetStringDefault("AhBot.IgnoreItemIds", "49283,52200,8494,6345,6891,2460,37164,34835,17,2248"), ignoreItemIds);
    LoadSet<std::set<uint32> >(config.GetStringDefault("AhBot.IgnoreVendorItemIds", "755,858,4592,4593,1710,3827,2455,3385"), ignoreVendorItemIds);
    sendmail = config.GetBoolDefault("AhBot.SendMail", true);

    // Existing Enabled=1 still sells and buys. The split switches default on so
    // a pre-existing ahbot.conf without the new keys keeps current behaviour.
    sellerEnabled = config.GetBoolDefault("AhBot.Seller.Enabled", true);
    buyerEnabled = config.GetBoolDefault("AhBot.Buyer.Enabled", true);

    itemsPerCycle = (uint32)std::max(0, config.GetIntDefault("AhBot.ItemsPerCycle", 0));
    allianceMinItems = (uint32)std::max(0, config.GetIntDefault("AhBot.Alliance.MinItems", 0));
    allianceMaxItems = (uint32)std::max(0, config.GetIntDefault("AhBot.Alliance.MaxItems", 0));
    allianceTargetPercent = (uint32)std::max(0, config.GetIntDefault("AhBot.Alliance.TargetPercent", 100));
    hordeMinItems = (uint32)std::max(0, config.GetIntDefault("AhBot.Horde.MinItems", 0));
    hordeMaxItems = (uint32)std::max(0, config.GetIntDefault("AhBot.Horde.MaxItems", 0));
    hordeTargetPercent = (uint32)std::max(0, config.GetIntDefault("AhBot.Horde.TargetPercent", 100));
    neutralMinItems = (uint32)std::max(0, config.GetIntDefault("AhBot.Neutral.MinItems", 0));
    neutralMaxItems = (uint32)std::max(0, config.GetIntDefault("AhBot.Neutral.MaxItems", 0));
    neutralTargetPercent = (uint32)std::max(0, config.GetIntDefault("AhBot.Neutral.TargetPercent", 100));

    customPriceStatsEnabled = config.GetBoolDefault("AhBot.CustomPriceStats.Enabled", false);
    customPriceStatsMinSampleCount = (uint32)std::max(1, config.GetIntDefault("AhBot.CustomPriceStats.MinSampleCount", 3));
    customPriceStatsBuyerMaxAcceptedPercentile = ahbot::NormalizeBuyerPercentile(
        (uint32)std::max(0, config.GetIntDefault("AhBot.CustomPriceStats.Buyer.MaxAcceptedPercentile", 90)));

    listingStatsEnabled = config.GetBoolDefault("AhBot.ListingStats.Enabled", false);
    listingStatsMinSeenCount = (uint32)std::max(0, config.GetIntDefault("AhBot.ListingStats.MinSeenCount", 2));
    listingStatsFallbackWeight = config.GetFloatDefault("AhBot.ListingStats.FallbackWeight", 1.0f);
    if (listingStatsFallbackWeight < 0.001f)
        listingStatsFallbackWeight = 0.001f;
    listingStatsScarcityExponent = config.GetFloatDefault("AhBot.ListingStats.ScarcityExponent", 1.25f);
    if (listingStatsScarcityExponent < 0.0f)
        listingStatsScarcityExponent = 1.25f;

    maxActiveEnabled = config.GetBoolDefault("AhBot.MaxActive.Enabled", false);
    maxActiveDefault = (uint32)std::max(0, config.GetIntDefault("AhBot.MaxActive.Default", 2));
    dynamicSupplyEnabled = config.GetBoolDefault("AhBot.DynamicSupply.Enabled", false);
    dynamicSupplyDebug = config.GetBoolDefault("AhBot.DynamicSupply.DEBUG", false);

    sellerPersonasEnabled = config.GetBoolDefault("AhBot.Seller.Personas.Enabled", false);
    undercuttingEnabled = config.GetBoolDefault("AhBot.Seller.UndercuttingEnabled", false);
    undercutPercentMin = config.GetFloatDefault("AhBot.Seller.UndercutPercentMin", 1.0f);
    undercutPercentMax = config.GetFloatDefault("AhBot.Seller.UndercutPercentMax", 5.0f);
    priceFloorStatsMultiplier = config.GetFloatDefault("AhBot.Seller.PriceFloorStatsMultiplier", 0.65f);

    maxBuyoutPrice = (uint32)std::max(0, config.GetIntDefault("AhBot.MaxBuyoutPriceInCopper", 0));
    buyoutVariationReducePercent = config.GetFloatDefault("AhBot.BuyoutVariationReducePercent", 0.0f);
    buyoutVariationAddPercent = config.GetFloatDefault("AhBot.BuyoutVariationAddPercent", 0.0f);
    bidVariationHighReducePercent = config.GetFloatDefault("AhBot.BidVariationHighReducePercent", 0.0f);
    bidVariationLowReducePercent = config.GetFloatDefault("AhBot.BidVariationLowReducePercent", 0.0f);
    vendorFloorEnabled = config.GetBoolDefault("AhBot.VendorFloor.Enabled", false);
    vendorFloorAddPercent = config.GetFloatDefault("AhBot.VendorFloor.AddPercent", 0.25f);
    stackRulesEnabled = config.GetBoolDefault("AhBot.Stack.RulesEnabled", false);

    ParseMinMax(config.GetStringDefault("AhBot.Buyer.CandidatesPerCycle", "0"),
        buyerCandidatesMin, buyerCandidatesMax, 0);
    buyerAcceptablePriceModifier = config.GetFloatDefault("AhBot.Buyer.AcceptablePriceModifier", 1.0f);
    if (buyerAcceptablePriceModifier <= 0.0f)
        buyerAcceptablePriceModifier = 1.0f;
    buyerAlwaysBidMax = config.GetBoolDefault("AhBot.Buyer.AlwaysBidMax", false);
    buyerPreventOverpayVendor = config.GetBoolDefault("AhBot.Buyer.PreventOverpayingForVendorItems", false);
    buyerWillBidAgainstPlayers = config.GetBoolDefault("AhBot.Buyer.WillBidAgainstPlayers", true);

    listingExpireMinSeconds = (uint32)std::max(0, config.GetIntDefault("AhBot.ListingExpireTimeInSecondsMin", 0));
    listingExpireMaxSeconds = (uint32)std::max(0, config.GetIntDefault("AhBot.ListingExpireTimeInSecondsMax", 0));
    if (listingExpireMaxSeconds && listingExpireMinSeconds > listingExpireMaxSeconds)
        std::swap(listingExpireMinSeconds, listingExpireMaxSeconds);

    realismDebug = config.GetBoolDefault("AhBot.DEBUG", false);

    return enabled;
}

bool AhBotConfig::Reload()
{
    return Initialize();
}
