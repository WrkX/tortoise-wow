REPLACE INTO `item_template`
(
    `entry`,
    `class`,
    `subclass`,
    `name`,
    `description`,
    `display_id`,
    `quality`,
    `flags`,
    `buy_count`,
    `buy_price`,
    `sell_price`,
    `inventory_type`,
    `allowable_class`,
    `allowable_race`,
    `item_level`,
    `required_level`,
    `max_count`,
    `stackable`,
    `spellid_1`,
    `spelltrigger_1`,
    `bonding`,
    `script_name`
)
VALUES
(1985500, 0, 0, 'Mark of the Ranger General', 'Redeems for 1 Donation Point.', 69174, 3, 0, 1, 30000, 0, 0, -1, -1, 1, 0, 0, 100, 61003, 0, 1, 'item_donation_mark'),
(1985501, 0, 0, 'Mark of the Ranger General', 'Redeems for 10 Donation Points.', 69174, 3, 0, 1, 300000, 0, 0, -1, -1, 1, 0, 0, 100, 61004, 0, 1, 'item_donation_mark'),
(1985502, 0, 0, 'Mark of the Ranger General', 'Redeems for 100 Donation Points.', 69174, 3, 0, 1, 3000000, 0, 0, -1, -1, 1, 0, 0, 100, 61005, 0, 1, 'item_donation_mark');

UPDATE `creature_template`
SET `subname` = 'Windrunner Associate',
    `npc_flags` = 5,
    `vendor_id` = 0,
    `script_name` = 'npc_shop_refund'
WHERE `entry` = 9;

REPLACE INTO `npc_vendor`
(
    `entry`,
    `slot`,
    `item`,
    `maxcount`,
    `incrtime`,
    `itemflags`,
    `condition_id`
)
VALUES
(9, 1, 1985500, 0, 0, 0, 0),
(9, 2, 1985501, 0, 0, 0, 0),
(9, 3, 1985502, 0, 0, 0, 0);
