#pragma once

// Pure deterministic AHBot economy helpers. No MaNGOS types, no SQL, no RNG.
// Runtime and the standalone test both include this header. Randomness is
// supplied by the caller so behaviour can be pinned in tests.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ahbot
{
    enum class SellerPersona : uint8_t
    {
        Undercutter = 0,
        Normal = 1,
        Greedy = 2,
        StockClearer = 3
    };

    struct PricePercentiles
    {
        uint32_t sampleCount = 0;
        uint32_t priceMin = 0;
        uint32_t priceP10 = 0;
        uint32_t priceP25 = 0;
        uint32_t priceMedian = 0;
        uint32_t priceP75 = 0;
        uint32_t priceP90 = 0;
        uint32_t priceMax = 0;
    };

    struct BuyerDecision
    {
        bool buyout = false;
        bool bid = false;
        uint32_t bidAmount = 0;
        bool blockedByVendor = false;
        bool blockedByPercentile = false;
        bool blockedByBudget = false;
    };

    inline uint32_t ClampU32(uint32_t value, uint32_t lo, uint32_t hi)
    {
        if (hi < lo)
            std::swap(lo, hi);
        if (value < lo)
            return lo;
        if (value > hi)
            return hi;
        return value;
    }

    inline uint32_t ScaleU32(uint32_t value, float factor)
    {
        if (factor <= 0.0f)
            return 0;
        double scaled = static_cast<double>(value) * static_cast<double>(factor);
        if (scaled < 1.0)
            return value ? 1 : 0;
        if (scaled > 4294967295.0)
            return 4294967295u;
        return static_cast<uint32_t>(scaled);
    }

    inline uint32_t RollInclusive(uint32_t lo, uint32_t hi, uint32_t roll)
    {
        if (hi < lo)
            std::swap(lo, hi);
        uint32_t span = hi - lo;
        if (span == 0)
            return lo;
        return lo + (roll % (span + 1));
    }

    // Daily house target. min=max=0 means "feature off" and returns 0.
    inline uint32_t RollDailyHouseTarget(uint32_t minItems, uint32_t maxItems, uint32_t targetPercent, uint32_t roll)
    {
        if (minItems == 0 && maxItems == 0)
            return 0;

        if (maxItems == 0)
            maxItems = minItems;
        if (minItems == 0)
            minItems = maxItems;
        if (minItems > maxItems)
            minItems = maxItems;

        uint32_t raw = RollInclusive(minItems, maxItems, roll);
        if (targetPercent >= 100)
            return raw;

        uint32_t scaled = (raw * targetPercent + 99) / 100;
        if (scaled < 1 && raw > 0)
            scaled = 1;
        return scaled;
    }

    inline uint32_t ItemsToPostThisCycle(uint32_t currentCount, uint32_t targetCount, uint32_t itemsPerCycle)
    {
        if (targetCount == 0)
            return itemsPerCycle;
        if (currentCount >= targetCount)
            return 0;
        uint32_t remaining = targetCount - currentCount;
        if (itemsPerCycle == 0)
            return remaining;
        return remaining < itemsPerCycle ? remaining : itemsPerCycle;
    }

    inline uint32_t BuyerMaxAcceptedPrice(const PricePercentiles& stats, uint32_t percentile)
    {
        if (percentile <= 10)
            return stats.priceP10;
        if (percentile <= 25)
            return stats.priceP25;
        if (percentile <= 50)
            return stats.priceMedian;
        if (percentile <= 75)
            return stats.priceP75;
        return stats.priceP90;
    }

    // Map an arbitrary percentile onto the next supported bucket (10/25/50/75/90).
    inline uint32_t NormalizeBuyerPercentile(uint32_t percentile)
    {
        if (percentile <= 10)
            return 10;
        if (percentile <= 25)
            return 25;
        if (percentile <= 50)
            return 50;
        if (percentile <= 75)
            return 75;
        return 90;
    }

    // Seller/buyer roll 1-100 selects a percentile band. rangeRoll picks inside it.
    // Returns 0 if stats are unusable so the caller can fall back to formula pricing.
    inline uint32_t RollPercentilePrice(const PricePercentiles& stats, uint32_t minSampleCount, bool buyer,
            uint32_t roll1to100, uint32_t rangeRoll)
    {
        if (stats.sampleCount < minSampleCount)
            return 0;

        uint32_t low = stats.priceP25;
        uint32_t high = stats.priceP75;
        uint32_t roll = roll1to100 ? roll1to100 : 1;
        if (roll > 100)
            roll = 100;

        if (buyer)
        {
            if (roll <= 15)
            {
                low = stats.priceP10;
                high = stats.priceMedian;
            }
            else if (roll <= 90)
            {
                low = stats.priceP25;
                high = stats.priceP75;
            }
            else
            {
                low = stats.priceP75;
                high = stats.priceP90;
            }
        }
        else
        {
            if (roll <= 15)
            {
                low = stats.priceP10;
                high = stats.priceP25;
            }
            else if (roll <= 85)
            {
                low = stats.priceP25;
                high = stats.priceP75;
            }
            else if (roll <= 95)
            {
                low = stats.priceP75;
                high = stats.priceP90;
            }
            else
            {
                low = stats.priceP90;
                high = stats.priceMax;
            }
        }

        if (low == 0 && high == 0)
            return 0;
        if (high < low)
            std::swap(low, high);
        if (low == 0)
            low = 1;
        return RollInclusive(low, high, rangeRoll);
    }

    inline uint32_t ApplyVendorFloorAndMax(uint32_t price, uint32_t vendorSell, bool vendorFloorEnabled, float vendorAddPercent, uint32_t maxPrice)
    {
        if (price == 0)
            price = 1;

        if (vendorFloorEnabled && vendorSell > 0 && price < vendorSell)
        {
            float add = vendorAddPercent < 0.0f ? 0.0f : vendorAddPercent;
            uint32_t raised = ScaleU32(vendorSell, 1.0f + add);
            if (raised < vendorSell)
                raised = vendorSell;
            price = raised;
        }

        if (maxPrice > 0 && price > maxPrice)
            price = maxPrice;
        if (price == 0)
            price = 1;
        return price;
    }

    inline uint32_t SellerPriceFloor(uint32_t vendorSell, const PricePercentiles* stats, uint32_t minSampleCount, float statsMultiplier)
    {
        uint32_t floorPrice = vendorSell > 0 ? vendorSell : 1;
        if (!stats || stats->sampleCount < minSampleCount)
            return floorPrice;

        uint32_t scaledMedian = ScaleU32(stats->priceMedian, statsMultiplier);
        uint32_t statsFloor = std::max(stats->priceP10, scaledMedian);
        return std::max(floorPrice, statsFloor);
    }

    // undercutRoll is used as 0..10000 hundredths of a percent when undercutting.
    // stockClearerRoll1to100 and discountRoll80to95 only apply to StockClearer.
    inline uint32_t ApplySellerPersonaPrice(SellerPersona persona, uint32_t buyout, uint32_t lowestUnit, uint32_t floorPrice,
            bool undercuttingEnabled, float undercutMinPercent, float undercutMaxPercent,
            uint32_t undercutRoll, uint32_t stockClearerRoll1to100, uint32_t discountRoll80to95,
            uint32_t greedyLow, uint32_t greedyHigh, uint32_t maxPrice)
    {
        uint32_t price = buyout ? buyout : 1;
        if (undercutMinPercent < 0.0f)
            undercutMinPercent = 0.0f;
        if (undercutMaxPercent < undercutMinPercent)
            undercutMaxPercent = undercutMinPercent;

        switch (persona)
        {
            case SellerPersona::Undercutter:
                if (undercuttingEnabled && lowestUnit > 0 && lowestUnit <= price)
                {
                    uint32_t minHundredths = static_cast<uint32_t>(undercutMinPercent * 100.0f);
                    uint32_t maxHundredths = static_cast<uint32_t>(undercutMaxPercent * 100.0f);
                    uint32_t hundredths = RollInclusive(minHundredths, maxHundredths, undercutRoll);
                    float undercutPct = 1.0f - (static_cast<float>(hundredths) / 10000.0f);
                    uint32_t cut = ScaleU32(lowestUnit, undercutPct);
                    if (cut < floorPrice)
                        cut = floorPrice;
                    if (cut > 0)
                        price = cut;
                }
                break;
            case SellerPersona::Greedy:
                if (greedyHigh > price)
                {
                    uint32_t lo = std::max(greedyLow, price);
                    price = RollInclusive(lo, greedyHigh, undercutRoll);
                }
                if (price <= floorPrice)
                    price = ScaleU32(floorPrice, 1.5f);
                break;
            case SellerPersona::StockClearer:
                if (undercuttingEnabled && lowestUnit > 0 && stockClearerRoll1to100 <= 15)
                {
                    uint32_t discount = ClampU32(discountRoll80to95, 80, 95);
                    uint32_t cut = ScaleU32(lowestUnit, static_cast<float>(discount) / 100.0f);
                    if (cut < floorPrice)
                        cut = floorPrice;
                    if (cut > 0)
                        price = cut;
                }
                break;
            case SellerPersona::Normal:
            default:
                break;
        }

        if (price < floorPrice)
            price = floorPrice;
        if (maxPrice > 0 && price > maxPrice)
            price = maxPrice;
        if (price == 0)
            price = 1;
        return price;
    }

    inline uint32_t BidFromBuyout(uint32_t buyout, float lowReducePercent, float highReducePercent, uint32_t roll)
    {
        if (buyout == 0)
            return 1;
        if (lowReducePercent < 0.0f)
            lowReducePercent = 0.0f;
        if (highReducePercent < 0.0f)
            highReducePercent = 0.0f;
        uint32_t lo = ScaleU32(buyout, 1.0f - lowReducePercent);
        uint32_t hi = ScaleU32(buyout, 1.0f - highReducePercent);
        uint32_t bid = RollInclusive(lo, hi, roll);
        if (bid == 0)
            bid = 1;
        if (bid > buyout)
            bid = buyout;
        return bid;
    }

    // ratioPercent: chance 0-100 of posting a stack > 1. 0 keeps a stack of 1
    // (legacy "feature off" when combined with the caller using Category::GetStackCount).
    inline uint32_t ChooseStackCount(uint32_t itemMaxStack, uint32_t ratioPercent, uint32_t increment, uint32_t configMax,
            uint32_t ratioRoll0to99, uint32_t stackChoiceRoll)
    {
        if (itemMaxStack <= 1)
            return 1;
        if (increment == 0)
            increment = 1;

        uint32_t maxPossible = itemMaxStack;
        if (configMax > 0 && configMax < maxPossible)
            maxPossible = configMax;

        if (ratioPercent <= ratioRoll0to99)
            return 1;

        uint32_t increments = (maxPossible + increment - 1) / increment;
        if (increments == 0)
            increments = 1;
        uint32_t numStacks = RollInclusive(1, increments, stackChoiceRoll);
        uint32_t size = numStacks * increment;
        if (size > maxPossible)
            size = maxPossible;
        if (size == 0)
            size = 1;
        return size;
    }

    inline uint32_t PerUnitPrice(uint32_t total, uint32_t count)
    {
        if (count == 0)
            return 0;
        return total / count;
    }

    inline uint32_t WeightedPick(const std::vector<uint32_t>& weights, uint32_t roll)
    {
        uint32_t total = 0;
        for (uint32_t w : weights)
            total += w;
        if (total == 0 || weights.empty())
            return 0;
        uint32_t pick = roll % total;
        uint32_t acc = 0;
        for (size_t i = 0; i < weights.size(); ++i)
        {
            acc += weights[i];
            if (pick < acc)
                return static_cast<uint32_t>(i);
        }
        return static_cast<uint32_t>(weights.size() - 1);
    }

    // Stable 4 undercutters / 4 greedies / 2 stock-clearers for the first 10
    // sorted seller ids. Remaining ids stay Normal.
    inline SellerPersona StablePersonaForIndex(size_t sortedIndex, bool personasEnabled)
    {
        if (!personasEnabled)
            return SellerPersona::Normal;

        static const SellerPersona kProfile[10] = {
            SellerPersona::Undercutter,
            SellerPersona::Greedy,
            SellerPersona::Undercutter,
            SellerPersona::Greedy,
            SellerPersona::Undercutter,
            SellerPersona::Greedy,
            SellerPersona::Undercutter,
            SellerPersona::Greedy,
            SellerPersona::StockClearer,
            SellerPersona::StockClearer
        };

        if (sortedIndex >= 10)
            return SellerPersona::Normal;
        return kProfile[sortedIndex];
    }

    inline BuyerDecision DecideBuyerOffer(uint32_t listingStartBid, uint32_t listingBid, uint32_t listingBuyout,
            uint32_t stackCount, uint32_t willingPerItem, uint32_t percentileCapPerItem, uint32_t vendorCap,
            uint32_t budget, bool alwaysBidMax)
    {
        BuyerDecision d;
        if (stackCount == 0)
            stackCount = 1;

        uint32_t willingStack = willingPerItem * stackCount;
        uint32_t percentileStack = percentileCapPerItem * stackCount;
        uint32_t currentBid = listingBid ? listingBid : listingStartBid;

        if (budget > 0 && currentBid > budget && (listingBuyout == 0 || listingBuyout > budget))
        {
            d.blockedByBudget = true;
            return d;
        }

        if (listingBuyout != 0 && listingBuyout <= willingStack)
            d.buyout = true;
        else if (listingBid == 0 && listingStartBid <= willingStack)
        {
            d.bid = true;
            d.bidAmount = alwaysBidMax ? willingStack : listingStartBid;
        }
        else if (listingBid != 0)
        {
            uint32_t nextBid = listingBid + std::max<uint32_t>(1, listingBid / 20);
            if (nextBid < willingStack)
            {
                d.bid = true;
                d.bidAmount = alwaysBidMax ? willingStack : nextBid;
            }
        }

        if (percentileStack > 0)
        {
            if (d.buyout && listingBuyout > percentileStack)
            {
                d.buyout = false;
                d.blockedByPercentile = true;
            }
            if (d.bid && d.bidAmount > percentileStack)
            {
                d.bid = false;
                d.blockedByPercentile = true;
            }
        }

        if (vendorCap > 0)
        {
            if (d.buyout && listingBuyout > vendorCap)
            {
                d.buyout = false;
                d.blockedByVendor = true;
            }
            if (d.bid && d.bidAmount > vendorCap)
            {
                d.bid = false;
                d.blockedByVendor = true;
            }
        }

        if (budget > 0)
        {
            if (d.buyout && listingBuyout > budget)
            {
                d.buyout = false;
                d.blockedByBudget = true;
            }
            if (d.bid && d.bidAmount > budget)
            {
                d.bid = false;
                d.blockedByBudget = true;
            }
        }

        return d;
    }

    // min=max=0 means "no bound, scan all" (legacy Answer() behaviour).
    inline uint32_t PickCandidateCount(uint32_t available, uint32_t minCandidates, uint32_t maxCandidates, uint32_t roll)
    {
        if (available == 0)
            return 0;
        uint32_t lo = minCandidates;
        uint32_t hi = maxCandidates;
        if (hi < lo)
            std::swap(lo, hi);
        if (hi == 0)
            return available;
        if (lo == 0)
            lo = 1;
        if (hi > available)
            hi = available;
        if (lo > hi)
            lo = hi;
        return RollInclusive(lo, hi, roll);
    }

    inline bool SelfCheck(std::string& errors)
    {
        errors.clear();
        auto fail = [&](const char* msg)
        {
            if (!errors.empty())
                errors += "; ";
            errors += msg;
        };

        if (RollDailyHouseTarget(0, 0, 100, 7) != 0)
            fail("disabled house target must be 0");
        if (RollDailyHouseTarget(10, 10, 100, 0) != 10)
            fail("fixed house target");
        if (RollDailyHouseTarget(100, 200, 10, 0) == 0)
            fail("percent target should round up to at least 1");
        if (ItemsToPostThisCycle(50, 100, 20) != 20)
            fail("items per cycle cap");
        if (ItemsToPostThisCycle(100, 100, 20) != 0)
            fail("at target posts nothing");
        if (ItemsToPostThisCycle(10, 15, 0) != 5)
            fail("unbounded cycle posts remaining");

        PricePercentiles stats;
        stats.sampleCount = 10;
        stats.priceP10 = 100;
        stats.priceP25 = 200;
        stats.priceMedian = 300;
        stats.priceP75 = 400;
        stats.priceP90 = 500;
        stats.priceMax = 900;
        if (RollPercentilePrice(stats, 3, false, 50, 0) != 200)
            fail("seller p25-p75 low bound");
        if (RollPercentilePrice(stats, 50, false, 50, 0) != 0)
            fail("untrusted sample must fallback");
        if (BuyerMaxAcceptedPrice(stats, 90) != 500)
            fail("buyer p90 cap");
        if (NormalizeBuyerPercentile(80) != 90)
            fail("percentile rounds up");

        if (ApplyVendorFloorAndMax(50, 100, true, 0.25f, 0) != 125)
            fail("vendor floor raise");
        if (ApplyVendorFloorAndMax(50, 100, false, 0.25f, 0) != 50)
            fail("vendor floor off");
        if (ApplyVendorFloorAndMax(5000, 1, false, 0.0f, 1000) != 1000)
            fail("max price clamp");

        uint32_t floor = SellerPriceFloor(80, &stats, 3, 0.65f);
        if (floor < 80)
            fail("price floor at least vendor");

        uint32_t undercut = ApplySellerPersonaPrice(SellerPersona::Undercutter, 1000, 1000, 100,
                true, 1.0f, 1.0f, 0, 100, 90, 400, 500, 0);
        if (undercut >= 1000)
            fail("undercutter must cut 1%");
        if (undercut < 100)
            fail("undercutter must honor floor");

        uint32_t greedy = ApplySellerPersonaPrice(SellerPersona::Greedy, 200, 0, 100,
                false, 1.0f, 5.0f, 0, 100, 90, 400, 500, 0);
        if (greedy < 400)
            fail("greedy should push toward p75-p90");

        uint32_t dump = ApplySellerPersonaPrice(SellerPersona::StockClearer, 1000, 1000, 100,
                true, 1.0f, 5.0f, 0, 10, 80, 0, 0, 0);
        if (dump >= 1000)
            fail("stock clearer dump");
        uint32_t noDump = ApplySellerPersonaPrice(SellerPersona::StockClearer, 1000, 1000, 100,
                true, 1.0f, 5.0f, 0, 50, 80, 0, 0, 0);
        if (noDump != 1000)
            fail("stock clearer usually holds");

        if (ChooseStackCount(20, 0, 5, 0, 0, 0) != 1)
            fail("zero ratio keeps stack 1");
        if (ChooseStackCount(20, 100, 5, 0, 0, 0) != 5)
            fail("full-ratio first increment");
        if (PerUnitPrice(150, 3) != 50)
            fail("per-unit snapshot price");

        std::vector<uint32_t> weights = { 0, 10, 0, 30 };
        if (WeightedPick(weights, 0) != 1)
            fail("weighted pick first mass");
        if (StablePersonaForIndex(0, true) != SellerPersona::Undercutter)
            fail("persona 0");
        if (StablePersonaForIndex(9, true) != SellerPersona::StockClearer)
            fail("persona 9");
        if (StablePersonaForIndex(10, true) != SellerPersona::Normal)
            fail("persona overflow");
        if (StablePersonaForIndex(0, false) != SellerPersona::Normal)
            fail("personas disabled");

        BuyerDecision cheap = DecideBuyerOffer(100, 0, 120, 1, 200, 0, 0, 1000, false);
        if (!cheap.buyout)
            fail("buyer buyout under willing price");
        BuyerDecision vendor = DecideBuyerOffer(100, 0, 120, 1, 200, 0, 50, 1000, false);
        if (vendor.buyout || !vendor.blockedByVendor)
            fail("vendor protection");
        BuyerDecision cap = DecideBuyerOffer(100, 0, 120, 1, 200, 50, 0, 1000, false);
        if (cap.buyout || !cap.blockedByPercentile)
            fail("percentile cap");
        BuyerDecision broke = DecideBuyerOffer(100, 0, 120, 1, 200, 0, 0, 10, false);
        if (broke.buyout || !broke.blockedByBudget)
            fail("budget cap");
        if (PickCandidateCount(100, 0, 0, 0) != 100)
            fail("unbounded candidates");
        if (PickCandidateCount(100, 3, 5, 0) != 3)
            fail("bounded candidate min");

        return errors.empty();
    }
}
