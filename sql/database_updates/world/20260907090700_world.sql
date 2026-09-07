-- AHBot market stats belong in the characters database next to ahbot_price.
-- Drop the empty world copies if an earlier revision of this migration created
-- them. Idempotent.
DROP TABLE IF EXISTS `ahbot_price_stats`;
DROP TABLE IF EXISTS `ahbot_listing_stats`;
