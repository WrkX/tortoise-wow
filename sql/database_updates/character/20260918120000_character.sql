-- Generic weekly quest completion ledger.
-- Rows are keyed by character and quest; completed_period is the deterministic
-- start timestamp of the weekly period in which the reward was granted.
CREATE TABLE IF NOT EXISTS `character_weekly_quest` (
  `guid` int(10) unsigned NOT NULL DEFAULT 0 COMMENT 'Global Unique Identifier',
  `quest` int(10) unsigned NOT NULL DEFAULT 0 COMMENT 'Quest Identifier',
  `completed_period` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci ROW_FORMAT=DYNAMIC COMMENT='Per-character weekly quest completion';
