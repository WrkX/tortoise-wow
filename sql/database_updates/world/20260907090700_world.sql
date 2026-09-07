-- Empty runtime-consumed price/listing stat tables. The Aux importer fills
-- rows later; this migration never inserts item IDs. Idempotent.
CREATE TABLE IF NOT EXISTS `ahbot_price_stats` (
  `item_id` int(10) unsigned NOT NULL,
  `sample_count` smallint(5) unsigned NOT NULL DEFAULT 0,
  `price_min` bigint(20) unsigned NOT NULL DEFAULT 0,
  `price_p10` bigint(20) unsigned NOT NULL DEFAULT 0,
  `price_p25` bigint(20) unsigned NOT NULL DEFAULT 0,
  `price_median` bigint(20) unsigned NOT NULL DEFAULT 0,
  `price_p75` bigint(20) unsigned NOT NULL DEFAULT 0,
  `price_p90` bigint(20) unsigned NOT NULL DEFAULT 0,
  `price_max` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`item_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;

CREATE TABLE IF NOT EXISTS `ahbot_listing_stats` (
  `item_id` int(10) unsigned NOT NULL,
  `snapshot_count` int(10) unsigned NOT NULL DEFAULT 0,
  `seen_count` int(10) unsigned NOT NULL DEFAULT 0,
  `total_occurrences` int(10) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`item_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;
