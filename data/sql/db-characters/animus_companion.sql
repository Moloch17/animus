-- mod-animus: one companion per player character. The companion is a character of its own (`guid`) on an account
-- made for it (`account`); the rest is what the module remembers about it between summons.
CREATE TABLE IF NOT EXISTS `animus_companion` (
  `owner` INT UNSIGNED NOT NULL COMMENT 'characters.guid of the player character that owns it',
  `account` INT UNSIGNED NOT NULL COMMENT 'account.id the companion character belongs to',
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid of the companion',
  `spec` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'index into the class profile''s specs',
  `edited` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 once the owner edited its talents, pet talents or gear',
  `owner_gear` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bit per equipment slot holding an item the owner gave it',
  PRIMARY KEY (`owner`),
  UNIQUE KEY `guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
