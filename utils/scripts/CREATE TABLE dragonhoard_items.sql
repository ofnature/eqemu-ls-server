CREATE TABLE `dragonhoard_items` (
  `account_id` int(11) NOT NULL,
  `slot_id` int(11) NOT NULL,
  `item_id` int(11) NOT NULL DEFAULT 0,
  `charges` int(11) NOT NULL DEFAULT 0,
  `serial` bigint(20) NOT NULL DEFAULT 0,
  `augslot1` int(11) NOT NULL DEFAULT 0,
  `augslot2` int(11) NOT NULL DEFAULT 0,
  `augslot3` int(11) NOT NULL DEFAULT 0,
  `augslot4` int(11) NOT NULL DEFAULT 0,
  `augslot5` int(11) NOT NULL DEFAULT 0,
  `augslot6` int(11) NOT NULL DEFAULT 0,
  `custom_data` text,
  PRIMARY KEY (`account_id`, `slot_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;