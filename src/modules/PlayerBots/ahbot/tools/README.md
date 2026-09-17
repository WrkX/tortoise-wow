# AHBot market stats from daily Aux snapshots

For a short, implementation-neutral handoff to another agent, see
[`DATA_CONTRACT.md`](DATA_CONTRACT.md).

Builds percentile and availability SQL for Turtle WoW (Vanilla) auction houses
from raw Aux SavedVariables or legacy SQL dumps. Apply the generated file to the **characters**
database (`tw_char`). The C++ AhBot runtime reads the same tables from
`CharacterDatabase`.

Every input server, date, and faction is pooled into one faction-neutral market
(`auction_house = 0`). Physical houses 1/6/7 remain operational; the runtime
falls back to shared rows for prices and listing statistics. Each source is
still recorded with server/faction provenance.

## Tables (characters database)

| Table | Role |
| --- | --- |
| `ahbot_price_stats` | Shared `(item_id, suffix_id, 0)` percentiles |
| `ahbot_listing_stats` | Shared item presence and listing counts |
| `ahbot_market_snapshot_source` | Provenance for each snapshot file |
| `ahbot_price` | Optional unsuffixed medians for the existing `GetMarketPrice` query |
| `ahbot_house_target` | Runtime daily population targets (not written by this tool) |

`suffix_id` 0 is an item with no random property. Suffixed rows are stats-only;
they are not copied into `ahbot_price`.

Do not create a second copy of these tables in the world database.

## Daily workflow (fresh Aux DB, both servers)

Put this script in `D:\twmoa_1181_cn` and run it without arguments. It recursively
finds every `WTF\Account\*\SavedVariables\aux-addon*.lua` and `.lua.bak` file
across both clients. Plain `.lua` files are renamed after successful generation
to `aux-addon_YYYYMMDD_NN.lua.bak`; existing backups remain inputs. All inputs
are intentionally retained, including duplicates from different clients.

Suggested layout:

```
clients/
  twmoa_1181/WTF/Account/<account>/SavedVariables/aux-addon.lua
  twmoa_1181 - Copy/WTF/Account/<account>/SavedVariables/aux-addon_*.lua.bak
```

Server, faction, and date are taken from `AHBOT_SNAPSHOT` metadata when present,
otherwise from the path/filename (`nordanaar`, `telabim`, `alliance`, `horde`,
`neutral`, and `YYYY-MM-DD` or `YYYYMMDD`).

Rebuild and load:

```sh
python build_ahbot_price_stats.py -o ahbot_market_stats.generated.sql

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

Raw Aux full scans are accepted only when marked complete and their expected
auction count matches the captured rows. Incomplete scans are rejected unless
`--allow-incomplete` is supplied. Legacy Aux daily-minimum history is accepted
as the fallback when no full scan is present.

The addon command `/aux ahbot scan` performs an unfiltered, paginated full scan
and stores it in `aux.ahbot_snapshot` for the builder.

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

`listing_count` is how many individual listing rows were observed, including
rows without a buyout (which contribute to frequency but not price statistics).
`seen_count` is how many snapshot files contained the item (presence).
`days_seen` is how many distinct calendar days it appeared. A missing day in
the middle of the window does not create a phantom snapshot; `snapshot_count`
is the number of source snapshots in the shared market.

## Daily population target

Set `AhBot.Shared.MinItems` and `AhBot.Shared.MaxItems` to configure one daily
market target. The server rolls and persists one target in
`ahbot_house_target` (house `0`), then fills the combined physical houses
toward it. `ItemsPerCycle`, category/item caps, expiration, and seller cooldowns
remain additional safeguards. Legacy per-house settings are used only when
the shared range is left at `0/0`.

## Tests

```sh
python3 -m unittest discover -s src/modules/PlayerBots/ahbot/tools/tests -v
```
