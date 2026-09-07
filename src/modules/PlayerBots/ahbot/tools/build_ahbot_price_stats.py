#!/usr/bin/env python3
"""Build Turtle/Vanilla AHBot market-stats SQL from daily Aux snapshots.

Reads daily SQL dumps produced from Aux (or legacy ahbot_custom_prices INSERT
lines), aggregates nearest-rank percentiles and availability, and emits
idempotent SQL for the characters-database tables next to ahbot_price.

Presence frequency (days_seen / seen_count) is tracked separately from
listing_count. Item ids are kept by default so Turtle custom entries in
24284-49999 are not discarded; pass --reject-expansion-ids only when ingesting
a known WotLK dump.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from datetime import date, datetime
from pathlib import Path

# House ids written by AhBot.cpp (auctionIds = {1, 6, 7}).
AUCTION_HOUSE_ALLIANCE = 1
AUCTION_HOUSE_HORDE = 6
AUCTION_HOUSE_NEUTRAL = 7

FACTION_HOUSES = {
    "alliance": AUCTION_HOUSE_ALLIANCE,
    "ally": AUCTION_HOUSE_ALLIANCE,
    "a": AUCTION_HOUSE_ALLIANCE,
    "horde": AUCTION_HOUSE_HORDE,
    "h": AUCTION_HOUSE_HORDE,
    "neutral": AUCTION_HOUSE_NEUTRAL,
    "goblin": AUCTION_HOUSE_NEUTRAL,
    "n": AUCTION_HOUSE_NEUTRAL,
}

FACTION_NAMES = {
    AUCTION_HOUSE_ALLIANCE: "alliance",
    AUCTION_HOUSE_HORDE: "horde",
    AUCTION_HOUSE_NEUTRAL: "neutral",
}

# Classic 1.12 Blizzard ids cap around 24283. Turtle custom items in this
# repository also occupy 24284-49999 (for example 36616, 42287) and 50000+
# (for example 55371, 83274). A numeric TBC/WotLK gap filter therefore cannot
# tell Turtle customs from expansion ids; default is to keep every positive id.
# --reject-expansion-ids restores the 24284-49999 drop for known WotLK dumps.
CLASSIC_ITEM_ID_MAX = 24283
EXPANSION_ITEM_ID_MAX = 49999

SNAPSHOT_TABLES = {
    "ahbot_custom_prices",
    "ahbot_aux_snapshot",
    "ahbot_aux_listings",
}

ITEM_COLUMNS = {"item_id", "item", "entry"}
SUFFIX_COLUMNS = {"suffix_id", "suffix", "random_property", "random_suffix"}
UNIT_PRICE_COLUMNS = {"price", "unit_price", "buyout_per_unit", "copper"}
BUYOUT_COLUMNS = {"buyout", "buyout_price"}
QUANTITY_COLUMNS = {"quantity", "count", "stack", "stack_count"}

INSERT_HEAD_RE = re.compile(
    r"INSERT\s+(?:IGNORE\s+)?INTO\s+`?(?P<table>\w+)`?"
    r"(?:\s*\((?P<cols>[^)]+)\))?"
    r"\s*VALUES\s*(?P<values>.*)$",
    re.IGNORECASE | re.DOTALL,
)

META_RE = re.compile(r"AHBOT_SNAPSHOT\b(.*)$", re.IGNORECASE)
DATE_RE = re.compile(r"(?<!\d)(\d{4}-\d{2}-\d{2}|\d{8})(?!\d)")
ITEM_KEY_RE = re.compile(r"^(\d+)(?:[:\-](\-?\d+))?$")
TRUE_VALUES = {"1", "true", "yes", "on", "complete"}

CREATE_PRICE_STATS_SQL = """\
CREATE TABLE IF NOT EXISTS `ahbot_price_stats` (
  `item_id` int(10) unsigned NOT NULL,
  `suffix_id` int(11) NOT NULL DEFAULT 0,
  `auction_house` bigint(20) NOT NULL,
  `sample_count` int(10) unsigned NOT NULL,
  `price_min` bigint(20) unsigned NOT NULL,
  `price_p10` bigint(20) unsigned NOT NULL,
  `price_p25` bigint(20) unsigned NOT NULL,
  `price_median` bigint(20) unsigned NOT NULL,
  `price_p75` bigint(20) unsigned NOT NULL,
  `price_p90` bigint(20) unsigned NOT NULL,
  `price_max` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`item_id`, `suffix_id`, `auction_house`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;
"""

CREATE_LISTING_STATS_SQL = """\
CREATE TABLE IF NOT EXISTS `ahbot_listing_stats` (
  `item_id` int(10) unsigned NOT NULL,
  `suffix_id` int(11) NOT NULL DEFAULT 0,
  `auction_house` bigint(20) NOT NULL,
  `snapshot_count` int(10) unsigned NOT NULL COMMENT 'Snapshots processed for this auction house',
  `days_seen` int(10) unsigned NOT NULL COMMENT 'Distinct calendar days this item appeared',
  `seen_count` int(10) unsigned NOT NULL COMMENT 'Distinct snapshots this item appeared in (presence, not listing count)',
  `listing_count` int(10) unsigned NOT NULL COMMENT 'Total listing observations across those snapshots',
  PRIMARY KEY (`item_id`, `suffix_id`, `auction_house`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;
"""

CREATE_SOURCE_SQL = """\
CREATE TABLE IF NOT EXISTS `ahbot_market_snapshot_source` (
  `source_id` int(10) unsigned NOT NULL,
  `source_path` varchar(512) NOT NULL,
  `server` varchar(64) NOT NULL DEFAULT '',
  `faction` varchar(16) NOT NULL DEFAULT '',
  `auction_house` bigint(20) NOT NULL,
  `snapshot_date` date DEFAULT NULL,
  `complete` tinyint(1) NOT NULL DEFAULT 1,
  `expected_listings` int(10) unsigned DEFAULT NULL,
  `parsed_listings` int(10) unsigned NOT NULL,
  PRIMARY KEY (`source_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;
"""


class SnapshotError(Exception):
    """Fatal snapshot validation error."""


@dataclass(frozen=True)
class ItemKey:
    item_id: int
    suffix_id: int
    auction_house: int


@dataclass
class SnapshotMeta:
    server: str = ""
    faction: str = ""
    snapshot_date: date | None = None
    complete: bool | None = None
    expected_listings: int | None = None


@dataclass
class Observation:
    item_id: int
    suffix_id: int
    auction_house: int
    unit_price: int
    source_id: int
    snapshot_date: date | None


@dataclass
class SourceRecord:
    source_id: int
    source_path: str
    server: str
    faction: str
    auction_house: int
    snapshot_date: date | None
    complete: bool
    expected_listings: int | None
    parsed_listings: int
    skipped_listings: int = 0
    rejected_expansion_ids: int = 0


def observed_percentile(sorted_values: list[int], percentile: int) -> int:
    """Nearest-rank percentile of already-sorted observed prices (no interpolation)."""
    if not sorted_values:
        raise ValueError("cannot calculate percentile without values")
    if percentile < 0 or percentile > 100:
        raise ValueError(f"percentile out of range: {percentile}")
    index = max(0, math.ceil((percentile / 100.0) * len(sorted_values)) - 1)
    return sorted_values[index]


def is_turtle_compatible_item_id(item_id: int, reject_expansion_ids: bool) -> bool:
    if item_id <= 0:
        return False
    if not reject_expansion_ids:
        return True
    return item_id <= CLASSIC_ITEM_ID_MAX or item_id > EXPANSION_ITEM_ID_MAX


def normalize_faction(value: str | None) -> str:
    if not value:
        return ""
    text = value.strip().lower().replace(" ", "")
    if text in {"alliance", "ally", "a"}:
        return "alliance"
    if text in {"horde", "h"}:
        return "horde"
    if text in {"neutral", "goblin", "n"}:
        return "neutral"
    return text


def faction_to_auction_house(faction: str) -> int:
    if not faction:
        return 0
    house = FACTION_HOUSES.get(faction)
    if house is None:
        raise SnapshotError(f"Unknown faction '{faction}'")
    return house


def auction_house_to_faction(auction_house: int) -> str:
    return FACTION_NAMES.get(auction_house, "")


def parse_bool(value: str) -> bool:
    return value.strip().lower() in TRUE_VALUES


def parse_date(value: str) -> date | None:
    text = value.strip()
    for fmt in ("%Y-%m-%d", "%Y%m%d"):
        try:
            return datetime.strptime(text, fmt).date()
        except ValueError:
            continue
    return None


def parse_int(value: str) -> int | None:
    text = value.strip().strip("'\"")
    if not text:
        return None
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if re.fullmatch(r"-?\d+\.0+", text):
        return int(text.split(".", 1)[0])
    return None


def sql_string(value: str) -> str:
    return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"


def sql_date(value: date | None) -> str:
    if value is None:
        return "NULL"
    return f"'{value.isoformat()}'"


def sql_optional_int(value: int | None) -> str:
    return "NULL" if value is None else str(int(value))


def collect_sql_files(inputs: list[Path], pattern: str, recursive: bool) -> list[Path]:
    files: list[Path] = []
    for input_path in inputs:
        if input_path.is_file():
            files.append(input_path)
            continue
        if input_path.is_dir():
            iterator = input_path.rglob(pattern) if recursive else input_path.glob(pattern)
            files.extend(path for path in iterator if path.is_file())
            continue
        raise FileNotFoundError(f"Input path does not exist: {input_path}")

    unique: dict[Path, Path] = {}
    for path in files:
        unique[path.resolve()] = path
    return sorted(unique.values(), key=lambda path: path.resolve().as_posix())


def infer_meta_from_path(path: Path, defaults: SnapshotMeta) -> SnapshotMeta:
    parts = [part.lower() for part in path.parts]
    stem = path.stem.lower()
    tokens = parts + re.split(r"[_\-.]+", stem)

    server = defaults.server
    for token in tokens:
        cleaned = token.replace("'", "")
        if cleaned in {"nordanaar", "telabim"}:
            server = "nordanaar" if cleaned == "nordanaar" else "telabim"
            break

    faction = defaults.faction
    for token in tokens:
        if token in FACTION_HOUSES:
            faction = "alliance" if token in {"alliance", "ally", "a"} else (
                "horde" if token in {"horde", "h"} else "neutral"
            )
            break

    snapshot_date = defaults.snapshot_date
    match = DATE_RE.search(path.name)
    if match:
        snapshot_date = parse_date(match.group(1)) or snapshot_date

    return SnapshotMeta(
        server=server,
        faction=faction,
        snapshot_date=snapshot_date,
        complete=defaults.complete,
        expected_listings=defaults.expected_listings,
    )


def parse_metadata_line(line: str) -> SnapshotMeta | None:
    match = META_RE.search(line)
    if not match:
        return None
    meta = SnapshotMeta()
    for token in match.group(1).split():
        if "=" not in token:
            continue
        key, raw = token.split("=", 1)
        key = key.strip().lower()
        raw = raw.strip().strip("'\"")
        if key == "server":
            meta.server = raw.replace("'", "").lower()
            if meta.server in {"tel'abim", "tel_abim", "tel-abim"}:
                meta.server = "telabim"
        elif key == "faction":
            meta.faction = normalize_faction(raw)
        elif key == "date":
            meta.snapshot_date = parse_date(raw)
        elif key in {"complete", "completed"}:
            meta.complete = parse_bool(raw)
        elif key in {"expected_listings", "expected_rows", "expected"}:
            parsed = parse_int(raw)
            if parsed is not None:
                meta.expected_listings = parsed
    return meta


def merge_meta(base: SnapshotMeta, override: SnapshotMeta) -> SnapshotMeta:
    return SnapshotMeta(
        server=override.server or base.server,
        faction=override.faction or base.faction,
        snapshot_date=override.snapshot_date or base.snapshot_date,
        complete=base.complete if override.complete is None else override.complete,
        expected_listings=(
            base.expected_listings
            if override.expected_listings is None
            else override.expected_listings
        ),
    )


def split_sql_tuples(values_sql: str) -> list[str]:
    text = values_sql.strip()
    cut = re.search(r"\bON\s+DUPLICATE\s+KEY\b", text, re.IGNORECASE)
    if cut:
        text = text[: cut.start()]
    text = text.rstrip().rstrip(";").strip()
    tuples: list[str] = []
    depth = 0
    start = -1
    for index, char in enumerate(text):
        if char == "(":
            if depth == 0:
                start = index + 1
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0 and start >= 0:
                tuples.append(text[start:index])
                start = -1
            if depth < 0:
                return []
    if depth != 0:
        return []
    return tuples


def split_sql_values(tuple_sql: str) -> list[str] | None:
    values: list[str] = []
    current: list[str] = []
    in_string: str | None = None
    escaped = False
    for char in tuple_sql:
        if in_string:
            current.append(char)
            if escaped:
                escaped = False
            elif char == "\\" and in_string == "'":
                escaped = True
            elif char == in_string:
                in_string = None
            continue
        if char in {"'", '"'}:
            in_string = char
            current.append(char)
            continue
        if char == ",":
            values.append("".join(current).strip())
            current = []
            continue
        current.append(char)
    if in_string:
        return None
    values.append("".join(current).strip())
    return values


def parse_item_identifier(raw: str) -> tuple[int, int] | None:
    text = raw.strip().strip("'\"")
    match = ITEM_KEY_RE.fullmatch(text)
    if not match:
        return None
    item_id = int(match.group(1))
    suffix_id = int(match.group(2)) if match.group(2) is not None else 0
    return item_id, suffix_id


def observation_from_columns(
    columns: list[str] | None,
    values: list[str],
    auction_house: int,
    source_id: int,
    snapshot_date: date | None,
    reject_expansion_ids: bool,
) -> Observation | None:
    mapped: dict[str, str] = {}
    if columns:
        if len(columns) != len(values):
            return None
        mapped = {name: values[index] for index, name in enumerate(columns)}
    elif len(values) == 2:
        mapped = {"item_id": values[0], "price": values[1]}
    elif len(values) == 3:
        mapped = {"item_id": values[0], "suffix_id": values[1], "price": values[2]}
    elif len(values) >= 4:
        mapped = {
            "item_id": values[0],
            "suffix_id": values[1],
            "price": values[2],
            "quantity": values[3],
        }
    else:
        return None

    item_raw = next((mapped[name] for name in mapped if name in ITEM_COLUMNS), "")
    parsed_item = parse_item_identifier(item_raw) if item_raw else None
    if parsed_item is None:
        return None
    item_id, suffix_from_key = parsed_item

    suffix_id = suffix_from_key
    suffix_raw = next((mapped[name] for name in mapped if name in SUFFIX_COLUMNS), None)
    if suffix_raw is not None:
        parsed_suffix = parse_int(suffix_raw)
        if parsed_suffix is None:
            return None
        suffix_id = parsed_suffix

    unit_price = None
    unit_raw = next((mapped[name] for name in mapped if name in UNIT_PRICE_COLUMNS), None)
    if unit_raw is not None:
        unit_price = parse_int(unit_raw)

    buyout_raw = next((mapped[name] for name in mapped if name in BUYOUT_COLUMNS), None)
    quantity_raw = next((mapped[name] for name in mapped if name in QUANTITY_COLUMNS), None)
    quantity = parse_int(quantity_raw) if quantity_raw is not None else None
    if unit_price is None and buyout_raw is not None:
        buyout = parse_int(buyout_raw)
        if buyout is None:
            return None
        if quantity is None or quantity <= 0:
            return None
        unit_price = buyout // quantity

    if unit_price is None or unit_price <= 0:
        return None
    if not is_turtle_compatible_item_id(item_id, reject_expansion_ids):
        return None

    return Observation(
        item_id=item_id,
        suffix_id=suffix_id,
        auction_house=auction_house,
        unit_price=unit_price,
        source_id=source_id,
        snapshot_date=snapshot_date,
    )


def parse_insert_line(
    line: str,
    auction_house: int,
    source_id: int,
    snapshot_date: date | None,
    reject_expansion_ids: bool,
) -> tuple[list[Observation], int, int]:
    match = INSERT_HEAD_RE.search(line.strip())
    if not match:
        return [], 0, 0
    table = match.group("table").lower()
    if table not in SNAPSHOT_TABLES:
        return [], 0, 0

    columns = None
    raw_cols = match.group("cols")
    if raw_cols:
        columns = [col.strip().strip("`").lower() for col in raw_cols.split(",")]

    tuples = split_sql_tuples(match.group("values"))
    if not tuples:
        return [], 1, 0

    observations: list[Observation] = []
    skipped = 0
    rejected = 0
    for raw_tuple in tuples:
        values = split_sql_values(raw_tuple)
        if values is None:
            skipped += 1
            continue
        item_raw = values[0] if (not columns and values) else None
        if columns:
            for index, name in enumerate(columns):
                if name in ITEM_COLUMNS and index < len(values):
                    item_raw = values[index]
                    break
        parsed_item = parse_item_identifier(item_raw) if item_raw else None
        if parsed_item and parsed_item[0] > 0 and not is_turtle_compatible_item_id(
            parsed_item[0], reject_expansion_ids
        ):
            rejected += 1
            continue
        observation = observation_from_columns(
            columns,
            values,
            auction_house,
            source_id,
            snapshot_date,
            reject_expansion_ids,
        )
        if observation is None:
            skipped += 1
            continue
        observations.append(observation)
    return observations, skipped, rejected


def read_snapshot_file(
    path: Path,
    source_id: int,
    defaults: SnapshotMeta,
    allow_incomplete: bool,
    reject_expansion_ids: bool,
) -> tuple[SourceRecord, list[Observation]]:
    meta = infer_meta_from_path(path, defaults)
    observations: list[Observation] = []
    skipped = 0
    rejected = 0

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            parsed_meta = parse_metadata_line(line)
            if parsed_meta is not None:
                meta = merge_meta(meta, parsed_meta)
                continue
            if line.startswith("--") or line.startswith("#"):
                continue
            parsed, skip_count, reject_count = parse_insert_line(
                line,
                faction_to_auction_house(meta.faction),
                source_id,
                meta.snapshot_date,
                reject_expansion_ids,
            )
            observations.extend(parsed)
            skipped += skip_count
            rejected += reject_count

    complete = True if meta.complete is None else meta.complete
    if meta.expected_listings is not None and meta.expected_listings != len(observations):
        complete = False
        message = (
            f"{path}: expected {meta.expected_listings} listings, parsed {len(observations)}"
        )
        if not allow_incomplete:
            raise SnapshotError(message)
        print(f"warning: {message}", file=sys.stderr)

    if meta.complete is False:
        message = f"{path}: snapshot metadata marked incomplete"
        if not allow_incomplete:
            raise SnapshotError(message)
        print(f"warning: {message}", file=sys.stderr)
        complete = False

    display_path = path.as_posix()
    record = SourceRecord(
        source_id=source_id,
        source_path=display_path,
        server=meta.server,
        faction=meta.faction,
        auction_house=faction_to_auction_house(meta.faction),
        snapshot_date=meta.snapshot_date,
        complete=complete,
        expected_listings=meta.expected_listings,
        parsed_listings=len(observations),
        skipped_listings=skipped,
        rejected_expansion_ids=rejected,
    )
    return record, observations


def aggregate_price_rows(
    observations: list[Observation],
) -> list[tuple[int, int, int, int, int, int, int, int, int, int, int]]:
    grouped: dict[ItemKey, list[int]] = defaultdict(list)
    for observation in observations:
        key = ItemKey(observation.item_id, observation.suffix_id, observation.auction_house)
        grouped[key].append(observation.unit_price)

    rows = []
    for key in sorted(grouped, key=lambda item: (item.item_id, item.suffix_id, item.auction_house)):
        prices = sorted(grouped[key])
        rows.append(
            (
                key.item_id,
                key.suffix_id,
                key.auction_house,
                len(prices),
                prices[0],
                observed_percentile(prices, 10),
                observed_percentile(prices, 25),
                observed_percentile(prices, 50),
                observed_percentile(prices, 75),
                observed_percentile(prices, 90),
                prices[-1],
            )
        )
    return rows


def aggregate_listing_rows(
    observations: list[Observation],
    snapshot_count_by_house: dict[int, int],
) -> list[tuple[int, int, int, int, int, int, int]]:
    seen_snapshots: dict[ItemKey, set[int]] = defaultdict(set)
    seen_days: dict[ItemKey, set[date]] = defaultdict(set)
    listing_counts: dict[ItemKey, int] = defaultdict(int)

    for observation in observations:
        key = ItemKey(observation.item_id, observation.suffix_id, observation.auction_house)
        seen_snapshots[key].add(observation.source_id)
        if observation.snapshot_date is not None:
            seen_days[key].add(observation.snapshot_date)
        listing_counts[key] += 1

    rows = []
    for key in sorted(seen_snapshots, key=lambda item: (item.item_id, item.suffix_id, item.auction_house)):
        rows.append(
            (
                key.item_id,
                key.suffix_id,
                key.auction_house,
                snapshot_count_by_house.get(key.auction_house, 0),
                len(seen_days[key]),
                len(seen_snapshots[key]),
                listing_counts[key],
            )
        )
    return rows


def write_generated_sql(
    output_path: Path,
    sources: list[SourceRecord],
    price_rows: list[tuple[int, int, int, int, int, int, int, int, int, int, int]],
    listing_rows: list[tuple[int, int, int, int, int, int, int]],
    truncate: bool,
    emit_ahbot_price: bool,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = [
        "-- Generated by build_ahbot_price_stats.py",
        "-- Turtle/Vanilla AHBot market stats. Do not import WotLK datasets.",
        f"-- Snapshots: {len(sources)}",
        "-- Sources:",
    ]
    for source in sources:
        date_text = source.snapshot_date.isoformat() if source.snapshot_date else "undated"
        faction_text = source.faction or "unknown"
        server_text = source.server or "unknown"
        lines.append(
            f"--   {source.source_id}. {source.source_path} "
            f"({server_text} {faction_text} {date_text}, {source.parsed_listings} listings)"
        )
    lines.append("")
    lines.append(CREATE_PRICE_STATS_SQL.rstrip())
    lines.append(CREATE_LISTING_STATS_SQL.rstrip())
    lines.append(CREATE_SOURCE_SQL.rstrip())
    lines.append("")

    if truncate:
        lines.append("TRUNCATE TABLE `ahbot_price_stats`;")
        lines.append("TRUNCATE TABLE `ahbot_listing_stats`;")
        lines.append("TRUNCATE TABLE `ahbot_market_snapshot_source`;")
        lines.append("")

    for source in sources:
        lines.append(
            "INSERT INTO `ahbot_market_snapshot_source` "
            "(`source_id`, `source_path`, `server`, `faction`, `auction_house`, "
            "`snapshot_date`, `complete`, `expected_listings`, `parsed_listings`) VALUES "
            f"({source.source_id}, {sql_string(source.source_path)}, "
            f"{sql_string(source.server)}, {sql_string(source.faction)}, "
            f"{source.auction_house}, {sql_date(source.snapshot_date)}, "
            f"{1 if source.complete else 0}, {sql_optional_int(source.expected_listings)}, "
            f"{source.parsed_listings}) "
            "ON DUPLICATE KEY UPDATE "
            "`source_path` = VALUES(`source_path`), "
            "`server` = VALUES(`server`), "
            "`faction` = VALUES(`faction`), "
            "`auction_house` = VALUES(`auction_house`), "
            "`snapshot_date` = VALUES(`snapshot_date`), "
            "`complete` = VALUES(`complete`), "
            "`expected_listings` = VALUES(`expected_listings`), "
            "`parsed_listings` = VALUES(`parsed_listings`);"
        )
    if sources:
        lines.append("")

    for row in price_rows:
        lines.append(
            "INSERT INTO `ahbot_price_stats` "
            "(`item_id`, `suffix_id`, `auction_house`, `sample_count`, `price_min`, "
            "`price_p10`, `price_p25`, `price_median`, `price_p75`, `price_p90`, `price_max`) "
            "VALUES "
            f"({row[0]}, {row[1]}, {row[2]}, {row[3]}, {row[4]}, {row[5]}, {row[6]}, "
            f"{row[7]}, {row[8]}, {row[9]}, {row[10]}) "
            "ON DUPLICATE KEY UPDATE "
            "`sample_count` = VALUES(`sample_count`), "
            "`price_min` = VALUES(`price_min`), "
            "`price_p10` = VALUES(`price_p10`), "
            "`price_p25` = VALUES(`price_p25`), "
            "`price_median` = VALUES(`price_median`), "
            "`price_p75` = VALUES(`price_p75`), "
            "`price_p90` = VALUES(`price_p90`), "
            "`price_max` = VALUES(`price_max`);"
        )
    if price_rows:
        lines.append("")

    for row in listing_rows:
        lines.append(
            "INSERT INTO `ahbot_listing_stats` "
            "(`item_id`, `suffix_id`, `auction_house`, `snapshot_count`, `days_seen`, "
            "`seen_count`, `listing_count`) VALUES "
            f"({row[0]}, {row[1]}, {row[2]}, {row[3]}, {row[4]}, {row[5]}, {row[6]}) "
            "ON DUPLICATE KEY UPDATE "
            "`snapshot_count` = VALUES(`snapshot_count`), "
            "`days_seen` = VALUES(`days_seen`), "
            "`seen_count` = VALUES(`seen_count`), "
            "`listing_count` = VALUES(`listing_count`);"
        )
    if listing_rows:
        lines.append("")

    if emit_ahbot_price:
        for row in price_rows:
            if row[1] != 0:
                continue
            item_id, _, auction_house, _, _, _, _, median, _, _, _ = row
            lines.append(
                f"DELETE FROM `ahbot_price` WHERE `item` = '{item_id}' "
                f"AND `auction_house` = '{auction_house}';"
            )
            lines.append(
                "INSERT INTO `ahbot_price` (`item`, `price`, `auction_house`) VALUES "
                f"('{item_id}', '{median}.00', '{auction_house}');"
            )

    text = "\n".join(lines)
    if not text.endswith("\n"):
        text += "\n"
    output_path.write_text(text, encoding="utf-8", newline="\n")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build Turtle/Vanilla AHBot market-stats SQL from daily Aux-derived "
            "snapshots across realms, servers, and factions."
        ),
        epilog=(
            "Example (two Turtle servers, keep every daily dump):\n"
            "  python3 build_ahbot_price_stats.py snapshots/nordanaar snapshots/telabim "
            "--recursive -o sql/ahbot_market_stats.generated.sql"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="SQL snapshot files or directories.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("ahbot_market_stats.generated.sql"),
        help="Output SQL file. Default: ahbot_market_stats.generated.sql",
    )
    parser.add_argument(
        "--pattern",
        default="*.sql",
        help="Glob used when an input is a directory. Default: *.sql",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Recursively search directory inputs.",
    )
    parser.add_argument(
        "--no-truncate",
        action="store_true",
        help="Do not emit TRUNCATE TABLE; upsert only keys present in this build.",
    )
    parser.add_argument(
        "--default-server",
        default="",
        help="Server name used when a file path/metadata does not name one.",
    )
    parser.add_argument(
        "--default-faction",
        default="",
        help="Faction (alliance/horde/neutral) used when a file does not name one.",
    )
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="Include snapshots whose completeness metadata fails instead of exiting.",
    )
    parser.add_argument(
        "--reject-expansion-ids",
        action="store_true",
        help=(
            "Drop item ids in 24284-49999. Off by default because Turtle custom "
            "items in this repository occupy that range as well as 50000+."
        ),
    )
    parser.add_argument(
        "--allow-expansion-ids",
        action="store_true",
        help="Deprecated no-op: expansion-range ids are kept unless --reject-expansion-ids is set.",
    )
    parser.add_argument(
        "--no-ahbot-price",
        action="store_true",
        help="Do not emit ahbot_price DELETE/INSERT rows for unsuffixed medians.",
    )
    return parser.parse_args(argv)


def build_from_files(
    files: list[Path],
    defaults: SnapshotMeta,
    allow_incomplete: bool,
    reject_expansion_ids: bool,
) -> tuple[list[SourceRecord], list[Observation]]:
    sources: list[SourceRecord] = []
    observations: list[Observation] = []
    for index, path in enumerate(files, start=1):
        record, parsed = read_snapshot_file(
            path,
            index,
            defaults,
            allow_incomplete,
            reject_expansion_ids,
        )
        sources.append(record)
        observations.extend(parsed)
    return sources, observations


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    files = collect_sql_files(args.inputs, args.pattern, args.recursive)
    if not files:
        print("No SQL files found.", file=sys.stderr)
        return 1

    defaults = SnapshotMeta(
        server=args.default_server.strip().lower(),
        faction=normalize_faction(args.default_faction),
    )
    if defaults.faction and defaults.faction not in FACTION_NAMES.values():
        raise SnapshotError(f"Unknown default faction '{args.default_faction}'")

    try:
        sources, observations = build_from_files(
            files,
            defaults,
            args.allow_incomplete,
            args.reject_expansion_ids,
        )
    except SnapshotError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    snapshot_count_by_house: dict[int, int] = defaultdict(int)
    for source in sources:
        snapshot_count_by_house[source.auction_house] += 1

    price_rows = aggregate_price_rows(observations)
    listing_rows = aggregate_listing_rows(observations, dict(snapshot_count_by_house))
    write_generated_sql(
        args.output,
        sources,
        price_rows,
        listing_rows,
        truncate=not args.no_truncate,
        emit_ahbot_price=not args.no_ahbot_price,
    )

    skipped = sum(source.skipped_listings for source in sources)
    rejected = sum(source.rejected_expansion_ids for source in sources)
    print(f"Read {len(files)} SQL file(s).")
    print(f"Collected {len(observations)} listing observation(s).")
    if skipped:
        print(f"Skipped {skipped} malformed listing row(s).")
    if rejected:
        print(f"Rejected {rejected} TBC/WotLK-range item id(s).")
    print(f"Wrote {len(price_rows)} price stat row(s) to {args.output}.")
    print(f"Wrote {len(listing_rows)} listing stat row(s) to {args.output}.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SnapshotError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
