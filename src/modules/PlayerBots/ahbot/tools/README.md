# AHBot market stats from daily Aux snapshots

For a short, implementation-neutral handoff to another agent, see
[`DATA_CONTRACT.md`](DATA_CONTRACT.md).

Builds percentile and availability SQL for Turtle WoW (Vanilla) auction houses
from daily Aux-derived dumps. Apply the generated file to the **characters**
database (`tw_char`). The C++ AhBot runtime reads the same tables from
`CharacterDatabase`.

Two Turtle servers are supported in one build: **Nordanaar** and **Tel'Abim**.
Alliance, Horde, and Neutral (goblin) houses stay separate (`auction_house` 1 / 6 / 7,
matching `AhBot.cpp`). Prices from both servers are pooled per house so sample
size grows; each input file is still recorded in `ahbot_market_snapshot_source`.

## Tables (characters database)

| Table | Role |
| --- | --- |
| `ahbot_price_stats` | Per `(item_id, suffix_id, auction_house)` percentiles |
| `ahbot_listing_stats` | Presence vs listing counts for the same key |
| `ahbot_market_snapshot_source` | Provenance for each snapshot file |
| `ahbot_price` | Optional unsuffixed medians for the existing `GetMarketPrice` query |
| `ahbot_house_target` | Runtime daily population targets (not written by this tool) |

`suffix_id` 0 is an item with no random property. Suffixed rows are stats-only;
they are not copied into `ahbot_price`.

Do not create a second copy of these tables in the world database.

## Daily workflow (fresh Aux DB, both servers)

Keep a history directory. Each day, dump that day's Aux auction database to SQL
and **leave previous days in place** — the builder aggregates the whole window.

Suggested layout:

```
snapshots/
  nordanaar/alliance/2026-09-07.sql
  nordanaar/horde/2026-09-07.sql
  nordanaar/neutral/2026-09-07.sql
  telabim/alliance/2026-09-07.sql
  telabim/horde/2026-09-07.sql
  telabim/neutral/2026-09-07.sql
```

Server, faction, and date are taken from `AHBOT_SNAPSHOT` metadata when present,
otherwise from the path/filename (`nordanaar`, `telabim`, `alliance`, `horde`,
`neutral`, and `YYYY-MM-DD` or `YYYYMMDD`).

Rebuild and load:

```sh
python3 src/modules/PlayerBots/ahbot/tools/build_ahbot_price_stats.py \
  snapshots/nordanaar snapshots/telabim \
  --recursive \
  -o ahbot_market_stats.generated.sql

mysql -u mangos -p tw_char < src/modules/PlayerBots/sql/characters/ai_playerbot_ahbot_market_stats.sql
mysql -u mangos -p tw_char < ahbot_market_stats.generated.sql
```

Auto-updater also applies `sql/database_updates/character/20260907090700_character.sql`
to the characters database. The generated file's `CREATE TABLE IF NOT EXISTS`
statements match that schema exactly.

`--no-truncate` upserts only keys present in this run and leaves other stats
rows untouched. The default emits `TRUNCATE` for the three stats/provenance
tables, then the same upserts (idempotent if re-applied).

Unsuffixed medians are also written to `ahbot_price` (DELETE + INSERT per
item/house) so the current runtime `GetMarketPrice` query can use them. Pass
`--no-ahbot-price` to skip that. Suffixed items are stats-only.

## Snapshot SQL

Header (optional, validated when present):

```sql
-- AHBOT_SNAPSHOT server=nordanaar faction=alliance date=2026-09-07 complete=1 expected_listings=3
```

If `complete=0` or `expected_listings` does not match the number of accepted
rows, the build exits unless you pass `--allow-incomplete`.

The builder also refuses files with an unknown faction or zero parsed listings,
because the default refresh truncates the existing stats. To represent a
genuinely empty auction-house scan, declare `expected_listings=0` explicitly.

Turtle listing rows (per-unit copper). `buyout` + `quantity` is accepted and
converted with integer division:

```sql
INSERT INTO `ahbot_aux_snapshot` (`item_id`, `suffix_id`, `unit_price`) VALUES (2580, 0, 3999);
INSERT INTO `ahbot_aux_listings` (`item`, `buyout`, `quantity`) VALUES ('754:5', 800, 2);
```

Legacy one-line dumps from WoWGreymane/mod-ah-Bot-Plus also parse:

```sql
INSERT INTO `ahbot_custom_prices` (`item_id`, `price`) VALUES (2580, 3999) ON DUPLICATE KEY UPDATE price = (price + VALUES(price)) / 2;
```

Do not feed that project's WotLK snapshot corpus. Item ids are kept by default,
including Turtle custom ids in 24284-49999 (for example 42287) and >= 50000
(for example 55371). A numeric "WotLK gap" filter cannot tell those Turtle
customs from TBC/WotLK ids, so it is opt-in: `--reject-expansion-ids` drops
24284-49999 and will also drop legitimate Turtle items in that band.
`--allow-expansion-ids` is a deprecated no-op kept for older command lines.

## Availability vs listings

`listing_count` is how many listing rows were observed (dump size / stacks).
`seen_count` is how many snapshot files contained the item (presence).
`days_seen` is how many distinct calendar days it appeared. A missing day in
the middle of the window does not create a phantom snapshot; `snapshot_count`
is the number of files for that auction house.

## Tests

```sh
python3 -m unittest discover -s src/modules/PlayerBots/ahbot/tools/tests -v
```
