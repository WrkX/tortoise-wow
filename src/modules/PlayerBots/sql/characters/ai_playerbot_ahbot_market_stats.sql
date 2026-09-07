-- AHBot market-stats tables consumed by the Aux snapshot importer
-- (src/modules/PlayerBots/ahbot/tools/build_ahbot_price_stats.py).
--
-- These sit in the characters database next to ahbot_price / ahbot_history.
-- auction_house uses the same house ids the current AhBot runtime writes
-- (1 Alliance, 6 Horde, 7 Neutral). suffix_id 0 is an item with no random
-- property. Presence frequency (seen_count / days_seen) is stored separately
-- from listing_count so scarcity is not confused with stack size or dump size.
--
-- Safe to re-apply: does not DROP populated tables.

CREATE TABLE IF NOT EXISTS `ahbot_custom_price_stats` (
  `item_id` INT UNSIGNED NOT NULL,
  `suffix_id` INT NOT NULL DEFAULT 0,
  `auction_house` BIGINT(20) NOT NULL,
  `sample_count` INT UNSIGNED NOT NULL,
  `price_min` BIGINT(20) UNSIGNED NOT NULL,
  `price_p10` BIGINT(20) UNSIGNED NOT NULL,
  `price_p25` BIGINT(20) UNSIGNED NOT NULL,
  `price_median` BIGINT(20) UNSIGNED NOT NULL,
  `price_p75` BIGINT(20) UNSIGNED NOT NULL,
  `price_p90` BIGINT(20) UNSIGNED NOT NULL,
  `price_max` BIGINT(20) UNSIGNED NOT NULL,
  PRIMARY KEY (`item_id`, `suffix_id`, `auction_house`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;

CREATE TABLE IF NOT EXISTS `ahbot_custom_listing_stats` (
  `item_id` INT UNSIGNED NOT NULL,
  `suffix_id` INT NOT NULL DEFAULT 0,
  `auction_house` BIGINT(20) NOT NULL,
  `snapshot_count` INT UNSIGNED NOT NULL COMMENT 'Snapshots processed for this auction house',
  `days_seen` INT UNSIGNED NOT NULL COMMENT 'Distinct calendar days this item appeared',
  `seen_count` INT UNSIGNED NOT NULL COMMENT 'Distinct snapshots this item appeared in (presence, not listing count)',
  `listing_count` INT UNSIGNED NOT NULL COMMENT 'Total listing observations across those snapshots',
  PRIMARY KEY (`item_id`, `suffix_id`, `auction_house`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;

CREATE TABLE IF NOT EXISTS `ahbot_market_snapshot_source` (
  `source_id` INT UNSIGNED NOT NULL,
  `source_path` VARCHAR(512) NOT NULL,
  `server` VARCHAR(64) NOT NULL DEFAULT '',
  `faction` VARCHAR(16) NOT NULL DEFAULT '',
  `auction_house` BIGINT(20) NOT NULL,
  `snapshot_date` DATE DEFAULT NULL,
  `complete` TINYINT(1) NOT NULL DEFAULT 1,
  `expected_listings` INT UNSIGNED DEFAULT NULL,
  `parsed_listings` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`source_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;
