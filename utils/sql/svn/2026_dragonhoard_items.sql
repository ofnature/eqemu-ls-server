-- Dragon's Hoard storage (account-based, max 500 slots). Laurion's Song.
--
-- sharedbank table structure (from SharedbankRepository) for reference:
--   acctid      INT
--   slotid      INT
--   itemid      INT
--   charges     SMALLINT
--   augslot1..augslot6 INT
--   custom_data TEXT
-- (sharedbank has no serial column; client uses 8-byte item serial for Dragon's Hoard)
--
-- dragonhoard_items: same fields as sharedbank + serial (BIGINT for client 8-byte serial).

CREATE TABLE IF NOT EXISTS `dragonhoard_items` (
    `account_id` INT UNSIGNED NOT NULL,
    `slot_id` SMALLINT UNSIGNED NOT NULL COMMENT '0..499, max 500 slots',
    `item_id` INT UNSIGNED NOT NULL,
    `charges` SMALLINT NOT NULL DEFAULT 0,
    `serial` BIGINT NOT NULL COMMENT '8-byte item serial from client',
    `augslot1` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot2` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot3` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot4` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot5` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot6` INT UNSIGNED NOT NULL DEFAULT 0,
    `custom_data` TEXT NULL,
    PRIMARY KEY (`account_id`, `slot_id`),
    UNIQUE KEY `account_serial` (`account_id`, `serial`),
    INDEX `ix_account_id` (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
