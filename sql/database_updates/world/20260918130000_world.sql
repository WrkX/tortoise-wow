-- Weekly battleground and raid-boss quests
-- Alliance quest entries: 9000000-9000001
-- Horde quest entries: 9000002-9000003
-- Internal objective-credit creature entries: 8000100-8000101
-- Alliance quest giver entry: 9000102

DELETE FROM `creature_questrelation`
WHERE `id` IN (9, 9000102) AND `quest` IN (9000000, 9000001, 9000002, 9000003);

DELETE FROM `creature_involvedrelation`
WHERE `id` IN (9, 9000102) AND `quest` IN (9000000, 9000001, 9000002, 9000003);

DELETE FROM `quest_template`
WHERE `entry` IN (9000000, 9000001, 9000002, 9000003);

DELETE FROM `creature_template`
-- Also remove the invalid IDs from the failed pre-fix migration attempt.
WHERE `entry` IN (8000100, 8000101, 9000100, 9000101, 9000102);

-- Keep entry 9 for the Horde quest giver while preserving its gossip,
-- vendor services, and refund script. Horde model is display 18728.
UPDATE `creature_template`
SET `name` = 'Dark Ranger Seena',
    `subname` = 'Weekly Quests',
    `display_id1` = 18728,
    `display_id2` = 0,
    `display_id3` = 0,
    `display_id4` = 0,
    `faction` = 85,
    `npc_flags` = `npc_flags` | 2,
    `vendor_id` = 0,
    `script_name` = 'npc_shop_refund'
WHERE `entry` = 9;

-- Alliance quest giver: display 20532, Stormwind faction, same services and refund script.
INSERT INTO `creature_template`
VALUES
    (9000102, 20532, 0, 0, 0, 0, 'Ranger Arisa', 'Weekly Quests', 0, 50, 50, 2768, 2768, 0, 0, 2999, 11, 3, 1, 1.14286, 0, 18, 5, 0, 0, 1, 76, 90, 0, 226, 1, 2000, 2000, 1, 768, 0, 0, 0, 0, 0, 0, 61.3, 82.9, 100, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '0', 0, 0, '', 0, 3, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 'npc_shop_refund');

UPDATE `creature`
SET `id` = 9000102
WHERE `guid` = 2582802 AND `id` = 9;

-- These never spawn; the core grants their credit when the relevant event occurs.
INSERT INTO `creature_template`
    (`entry`, `display_id1`, `name`, `level_min`, `level_max`, `health_min`, `health_max`, `faction`, `unit_class`)
VALUES
    (8000100, 1244, 'Battleground Match', 1, 1, 1, 1, 35, 1),
    (8000101, 1244, 'Raid Boss', 1, 1, 1, 1, 35, 1);

INSERT INTO `quest_template`
    (`entry`, `Method`, `MinLevel`, `QuestLevel`, `RequiredRaces`, `Type`, `QuestFlags`, `SpecialFlags`,
     `Title`, `Details`, `Objectives`, `OfferRewardText`, `RequestItemsText`, `ObjectiveText1`,
     `ReqCreatureOrGOId1`, `ReqCreatureOrGOCount1`, `RewItemId1`, `RewItemCount1`)
VALUES
(9000000, 2, 1, 60, 589, 41, 64, 2049,
     'Weekly: Arisa''s Call',
     'I have received word from Valeera Windrunner: the Alliance fronts are under pressure, and the Horde is pushing hard. The battlemasters need fighters they can rely upon; soldiers who will stand their ground and see each clash through, whether it ends in victory or defeat.',
     'Complete five battleground matches for the Alliance.',
     'The Alliance battlemasters speak well of your resolve.$b$bTake these three marks with my thanks. You have earned them.',
     'The Alliance fronts still need you, $N. Have you seen five battleground matches through to their end?',
     'Battleground matches completed',
     8000100, 5, 1985500, 3),
    (9000001, 2, 60, 60, 589, 62, 64, 2049,
     'Weekly: Mighty Threats',
     'Valeera Windrunner has told me that powerful enemies are gathering strength in the deepest strongholds of Azeroth. Their leaders must be struck down before their schemes bear fruit.$b$bBring down five mighty threats, $N, and return to me.',
     'Defeat five raid bosses.',
     'Five mighty foes lie defeated. The Alliance will remember your service.$b$bTake these three marks. You have served our cause well.',
     'Five mighty foes must fall before my task is done. How many still draw breath, $N?',
     'Raid bosses defeated',
     8000101, 5, 1985500, 3),
    (9000002, 2, 1, 60, 434, 41, 64, 2049,
     'Weekly: Seena''s Call',
     'I have received word from Sylvanas Windrunner: the Alliance is pressing hard, and the battlemasters need fighters they can rely upon. Stand your ground and see each clash through, whether it ends in victory or defeat.',
     'Complete five battleground matches for the Horde.',
     'The Horde battlemasters speak well of your resolve.',
     'The Horde fronts still need you, $N. Have you seen five battleground matches through to their end?',
     'Battleground matches completed',
     8000100, 5, 1985500, 3),
    (9000003, 2, 60, 60, 434, 62, 64, 2049,
     'Weekly: Mighty Threats',
     'I have heard dark tidings from the deepest strongholds of Azeroth. Powerful enemies gather behind their walls, and the Horde needs them struck down before their schemes bear fruit.',
     'Defeat five raid bosses.',
     'Five mighty foes lie defeated. The Horde will remember your service.',
     'Five mighty foes must fall before my task is done. How many still draw breath, $N?',
     'Raid bosses defeated',
     8000101, 5, 1985500, 3);

-- Give the Alliance copy the same donation-mark vendor inventory as entry 9.
DELETE FROM `npc_vendor`
WHERE `entry` = 9000102;

INSERT INTO `npc_vendor`
    (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `itemflags`, `condition_id`)
VALUES
    (9000102, 1, 1985500, 0, 0, 0, 0),
    (9000102, 2, 1985501, 0, 0, 0, 0),
    (9000102, 3, 1985502, 0, 0, 0, 0);

INSERT INTO `creature_questrelation` (`id`, `quest`)
VALUES
    (9000102, 9000000),
    (9000102, 9000001),
    (9, 9000002),
    (9, 9000003);

INSERT INTO `creature_involvedrelation` (`id`, `quest`)
VALUES
    (9000102, 9000000),
    (9000102, 9000001),
    (9, 9000002),
    (9, 9000003);
