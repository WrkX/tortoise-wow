-- Runtime-owned daily house population targets. Idempotent.
CREATE TABLE IF NOT EXISTS `ahbot_house_target` (
  `auction_house` int(10) unsigned NOT NULL,
  `last_roll_day` int(11) NOT NULL DEFAULT 0,
  `target_items` int(10) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`auction_house`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COLLATE=utf8mb3_general_ci;
