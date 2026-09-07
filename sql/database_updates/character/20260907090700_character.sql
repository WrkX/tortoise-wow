-- AHBot character-database tables. Idempotent. Stats rows are filled later by
-- src/modules/PlayerBots/ahbot/tools/build_ahbot_price_stats.py; this
-- migration never inserts item IDs.
--
-- auction_house uses the same house ids AhBot.cpp writes (1 Alliance, 6 Horde,
-- 7 Neutral). suffix_id 0 is an item with no random property.

CREATE TABLE IF NOT EXISTS `ahbot_house_target` (
  `auction_house` int(10) unsigned NOT NULL,
  `last_roll_day` int(11) NOT NULL DEFAULT 0,
  `target_items` int(10) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`auction_house`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;

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
