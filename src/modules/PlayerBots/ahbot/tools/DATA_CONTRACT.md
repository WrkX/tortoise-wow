# AHBot market-data contract

Use this document when building or replacing the Python converter for scraped
Turtle WoW auction-house data.

## What the data powers

The server uses historical listings to produce realistic per-item prices and
availability. All generated market statistics are shared across servers, dates,
and factions using `auction_house=0`. Random-property/suffix variants may also
have their own price statistics; listing frequency is item-level because the
runtime selects item templates.

The converter must generate SQL for the **characters database**, never the
world database.

## Required input per snapshot

Each snapshot represents one server, faction, and time. Prefer complete auction
listings instead of only the cheapest price. Legacy Aux daily-minimum history is
accepted as a fallback when a complete snapshot is unavailable.

Required for every price-bearing listing:

- `item_id`: positive item-template ID
- `suffix_id`: signed random-property/suffix ID; use `0` when absent
- either `unit_price` in copper, or `buyout` plus a positive `quantity`. A
  complete raw scan may also contain no-buyout rows; those count toward
  `listing_count` but are omitted from price percentiles.

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

| Generated market | `auction_house` |
| --- | ---: |
| Shared | 0 |

The source table retains faction provenance, but faction is not a generated
statistics dimension. Physical houses 1/6/7 remain live runtime houses.

## Required output tables

The generated SQL must create/upsert these character-database tables using the
schema in `ai_playerbot_ahbot_market_stats.sql`:

- `ahbot_price_stats`: nearest-rank price percentiles per shared
  `(item_id, suffix_id, 0)`
- `ahbot_listing_stats`: shared item snapshot presence and listing volume
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
- Write only shared market rows with house `0`; retain source faction for provenance.
- Reject incomplete scans by default. For completed raw Aux scans, retain and
  warn about listing-count changes caused by the live auction house changing
  during pagination.
- Reject an empty full snapshot unless it explicitly declares
  `expected_listings=0`; empty legacy faction blocks are ignored.
- Do not silently discard malformed rows; report their count.
- Preserve negative suffix IDs.
- Keep previous daily snapshots so percentiles describe a time window rather
  than one moment.
- If emitting `TRUNCATE`, validate every input before writing output SQL.

The current reference implementation and tests are
`build_ahbot_price_stats.py` and `tests/test_build_ahbot_price_stats.py`.
