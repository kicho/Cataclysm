UPDATE `item_template` SET `name` = entry WHERE `name` = '';

UPDATE `item_template` SET `AllowableRace` = 946 WHERE `AllowableRace` = 690;
UPDATE `item_template` SET `AllowableRace` = 33869 WHERE `AllowableRace` = 1101;
UPDATE `item_template` SET `AllowableRace` = -1 WHERE `AllowableRace` = 32767;

UPDATE `creature_spawns` SET `spawnMask` = 0 WHERE `map` = 0;
DELETE FROM `instance_template` WHERE `map` = 29;