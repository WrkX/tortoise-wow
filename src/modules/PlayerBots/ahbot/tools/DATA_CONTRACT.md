# AHBot market-data contract

Use this document when building or replacing the Python converter for scraped
Turtle WoW auction-house data.

## What the data powers

The server uses historical listings to produce realistic per-item prices and
availability. Prices and availability remain separate for Alliance, Horde, and
Neutral auction houses. Random-property/suffix variants may also have their own
statistics.

The converter must generate SQL for the **characters database**, never the
world database.

## Required input per snapshot

Each snapshot represents one server, faction, and time. Prefer complete auction
listings instead of only the cheapest price.

Required for every listing:

- `item_id`: positive item-template ID
- `suffix_id`: signed random-property/suffix ID; use `0` when absent
- either `unit_price` in copper, or `buyout` plus a positive `quantity`

Required once per snapshot:

- `server`: for example `nordanaar` or `telabim`
- `faction`: `alliance`, `horde`, or `neutral`
- `date`: `YYYY-MM-DD`
- whether the scan is complete
- expected listing count, when known

Recommended SQL input:

```sql
-- AHBOT_SNAPSHOT server=nordanaar faction=alliance date=2026-09-07 complete=1 expected_listings=2
INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES
  (2580, 0, 3999),
  (754, 5, 400);
```

An equivalent full-listing row is:

```sql
INSERT INTO `ahbot_aux_listings` (`item`, `buyout`, `quantity`)
VALUES ('754:5', 800, 2);
```

All prices are copper. `unit_price = buyout / quantity` using integer division.
Turtle custom item IDs must be retained; numeric ID ranges cannot reliably
distinguish them from expansion items.

## Auction-house mapping

| Faction | `auction_house` |
| --- | ---: |
| Alliance | 1 |
| Horde | 6 |
| Neutral | 7 |

Servers may be pooled to increase sample size, but factions must never be
pooled together.

## Required output tables

The generated SQL must create/upsert these character-database tables using the
schema in `ai_playerbot_ahbot_market_stats.sql`:

- `ahbot_price_stats`: nearest-rank price percentiles per
  `(item_id, suffix_id, auction_house)`
- `ahbot_listing_stats`: snapshot presence and listing volume for the same key
- `ahbot_market_snapshot_source`: one provenance row per input snapshot
- `ahbot_price`: optional unsuffixed median price for the legacy runtime lookup

Required price columns are `sample_count`, `price_min`, `price_p10`,
`price_p25`, `price_median`, `price_p75`, `price_p90`, and `price_max`.

Required availability columns are:

- `snapshot_count`: number of snapshots processed for that auction house
- `days_seen`: distinct dates containing the item
- `seen_count`: distinct snapshots containing the item
- `listing_count`: total individual listing rows observed

## Converter safety rules

- Produce deterministic output from the same inputs.
- Reject unknown/missing factions instead of writing house `0`.
- Reject incomplete scans or listing-count mismatches by default.
- Reject a zero-row parse by default; allow it only when the snapshot explicitly
  declares `expected_listings=0`.
- Do not silently discard malformed rows; report their count.
- Preserve negative suffix IDs.
- Keep previous daily snapshots so percentiles describe a time window rather
  than one moment.
- If emitting `TRUNCATE`, validate every input before writing output SQL.

The current reference implementation and tests are
`build_ahbot_price_stats.py` and `tests/test_build_ahbot_price_stats.py`.
