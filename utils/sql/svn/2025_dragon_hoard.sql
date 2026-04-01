-- Dragon's Hoard storage (account-based, max 500 slots). Laurion's Song.
-- Modeled after sharedbank; used by OP_DragonsHoard deposit/retrieve.

CREATE TABLE IF NOT EXISTS `dragon_hoard` (
    `account_id` INT UNSIGNED NOT NULL,
    `slot_id` SMALLINT UNSIGNED NOT NULL,
    `item_id` INT UNSIGNED NOT NULL,
    `charges` SMALLINT NOT NULL DEFAULT 0,
    `serial` BIGINT NOT NULL,
    `augslot1` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot2` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot3` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot4` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot5` INT UNSIGNED NOT NULL DEFAULT 0,
    `augslot6` INT UNSIGNED NOT NULL DEFAULT 0,
    `custom_data` TEXT NULL,
    PRIMARY KEY (`account_id`, `slot_id`),
    UNIQUE KEY `account_serial` (`account_id`, `serial`),
    INDEX `account_id` (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
