-- MySQL schema and seed data for the chat module. scripts/init-mysql.sh creates
-- and selects the target database before importing this file. The application
-- uses the MySQL classic protocol; initialization also uses the classic mysql client.

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

DROP TABLE IF EXISTS `groupApplys`;
DROP TABLE IF EXISTS `groupMembers`;
DROP TABLE IF EXISTS `groupInfo`;
DROP TABLE IF EXISTS `groups`;
DROP TABLE IF EXISTS `messageAttachments`;
DROP TABLE IF EXISTS `conversationDeviceStates`;
DROP TABLE IF EXISTS `devices`;
DROP TABLE IF EXISTS `messages`;
DROP TABLE IF EXISTS `fileUploads`;
DROP TABLE IF EXISTS `fileObjects`;
DROP TABLE IF EXISTS `conversationUserStates`;
DROP TABLE IF EXISTS `conversations`;
DROP TABLE IF EXISTS `friends`;
DROP TABLE IF EXISTS `friendApplys`;
DROP TABLE IF EXISTS `userInfo`;
DROP TABLE IF EXISTS `users`;

CREATE TABLE `users` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `uid` BIGINT UNSIGNED NOT NULL,
  `username` VARCHAR(64) NOT NULL,
  `password` VARCHAR(128) NOT NULL,
  `email` VARCHAR(255) NOT NULL,
  `createTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_users_uid` (`uid`),
  UNIQUE KEY `uk_users_username` (`username`),
  UNIQUE KEY `uk_users_email` (`email`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `userInfo` (
  `uid` BIGINT UNSIGNED NOT NULL,
  `name` VARCHAR(64) NOT NULL DEFAULT '',
  `age` SMALLINT NOT NULL DEFAULT 0,
  `sex` VARCHAR(16) NOT NULL DEFAULT '',
  `headImageURL` VARCHAR(512) NOT NULL DEFAULT '',
  PRIMARY KEY (`uid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `friendApplys` (
  `fromUid` BIGINT UNSIGNED NOT NULL,
  `toUid` BIGINT UNSIGNED NOT NULL,
  `content` VARCHAR(512) NOT NULL DEFAULT '',
  `status` SMALLINT NOT NULL DEFAULT 0 COMMENT '0 wait, 1 agree, 2 refuse',
  `createTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`fromUid`, `toUid`),
  KEY `idx_friendApplys_from_status` (`fromUid`, `status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `friends` (
  `friendshipId` BIGINT UNSIGNED NOT NULL,
  `uidA` BIGINT UNSIGNED NOT NULL,
  `uidB` BIGINT UNSIGNED NOT NULL,
  `createTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`friendshipId`),
  UNIQUE KEY `uk_friends_pair` (`uidA`, `uidB`),
  KEY `idx_friends_uidB` (`uidB`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `conversations` (
  `conversationId` BIGINT UNSIGNED NOT NULL,
  `type` SMALLINT NOT NULL COMMENT '1 direct, 2 group',
  `businessId` BIGINT UNSIGNED NOT NULL,
  `latestSeq` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `createTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`conversationId`),
  UNIQUE KEY `uk_conversations_type_business` (`type`, `businessId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `conversationUserStates` (
  `conversationId` BIGINT UNSIGNED NOT NULL,
  `uid` BIGINT UNSIGNED NOT NULL,
  `joinedSeq` BIGINT UNSIGNED NOT NULL DEFAULT 1,
  `leftSeq` BIGINT UNSIGNED NULL,
  `deliveredSeq` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `readSeq` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`conversationId`, `uid`),
  KEY `idx_conversationUserStates_uid` (`uid`, `conversationId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `messages` (
  `messageId` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `senderId` BIGINT UNSIGNED NOT NULL,
  `receiverId` BIGINT UNSIGNED NOT NULL,
  `conversationId` BIGINT UNSIGNED NOT NULL,
  `conversationSeq` BIGINT UNSIGNED NOT NULL,
  `clientMessageId` VARCHAR(128) NULL,
  `commandHash` VARCHAR(128) NULL,
  `type` SMALLINT NOT NULL DEFAULT 1 COMMENT '1 text, 2 image, 3 audio, 4 video, 5 file',
  `content` TEXT NOT NULL,
  `status` SMALLINT NOT NULL DEFAULT 1 COMMENT '0 withdraw, 1 accepted, 2 delivered, 3 read',
  `sendDateTime` VARCHAR(32) NOT NULL DEFAULT '',
  `readDateTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`messageId`),
  UNIQUE KEY `uk_messages_conversation_seq` (`conversationId`, `conversationSeq`),
  UNIQUE KEY `uk_messages_sender_client` (`senderId`, `clientMessageId`),
  KEY `idx_messages_conversation_pull` (`conversationId`, `conversationSeq`),
  KEY `idx_messages_file_content` (`content`(36)),
  KEY `idx_messages_participants_pull` (`senderId`, `receiverId`, `messageId`),
  KEY `idx_messages_receiver_pull` (`receiverId`, `messageId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `groupInfo` (
  `groupId` BIGINT UNSIGNED NOT NULL,
  `masterUid` BIGINT UNSIGNED NOT NULL,
  `name` VARCHAR(128) NOT NULL DEFAULT '',
  `conversationId` BIGINT UNSIGNED NOT NULL,
  `createTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`groupId`),
  UNIQUE KEY `uk_groupInfo_conversation` (`conversationId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `fileObjects` (
  `fileId` VARCHAR(36) NOT NULL,
  `ownerUid` BIGINT UNSIGNED NOT NULL,
  `fileName` VARCHAR(255) NOT NULL,
  `fileType` VARCHAR(128) NOT NULL,
  `fileSize` BIGINT UNSIGNED NOT NULL,
  `sha256` BINARY(32) NOT NULL,
  `state` SMALLINT NOT NULL DEFAULT 1 COMMENT '1 ready, 2 expired',
  `createdTime` VARCHAR(32) NOT NULL DEFAULT '',
  `expiresTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`fileId`),
  KEY `idx_fileObjects_owner_state` (`ownerUid`, `state`, `expiresTime`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `fileUploads` (
  `ownerUid` BIGINT UNSIGNED NOT NULL,
  `fileId` VARCHAR(36) NOT NULL,
  `nextOffset` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `complete` TINYINT NOT NULL DEFAULT 0,
  `updateTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`ownerUid`, `fileId`),
  KEY `idx_fileUploads_update` (`complete`, `updateTime`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `groupMembers` (
  `groupId` BIGINT UNSIGNED NOT NULL,
  `uid` BIGINT UNSIGNED NOT NULL,
  `joinTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`groupId`, `uid`),
  KEY `idx_groupMembers_uid` (`uid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `groupApplys` (
  `groupId` BIGINT UNSIGNED NOT NULL,
  `requestorUid` BIGINT UNSIGNED NOT NULL,
  `message` VARCHAR(512) NOT NULL DEFAULT '',
  `createTime` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`groupId`, `requestorUid`),
  KEY `idx_groupApplys_group` (`groupId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `devices` (
  `deviceId` VARCHAR(36) NOT NULL,
  `uid` BIGINT UNSIGNED NOT NULL,
  `platform` VARCHAR(32) NOT NULL DEFAULT '',
  `deviceName` VARCHAR(128) NOT NULL DEFAULT '',
  PRIMARY KEY (`deviceId`),
  KEY `idx_devices_uid` (`uid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `conversationDeviceStates` (
  `conversationId` BIGINT UNSIGNED NOT NULL,
  `deviceId` VARCHAR(36) NOT NULL,
  `persistedSeq` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`conversationId`, `deviceId`),
  KEY `idx_conversationDeviceStates_device` (`deviceId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

SET FOREIGN_KEY_CHECKS = 1;

-- Minimal seed data for local smoke testing.
-- Default local smoke users.
INSERT INTO `users` (`id`, `uid`, `username`, `password`, `email`, `createTime`) VALUES
  (NULL, 1001, 'zorjen', '123456', 'zorjen@example.com', '2026-05-19 00:00:00'),
  (NULL, 1002, 'alice', '123456', 'alice@example.com', '2026-05-19 00:00:00'),
  (NULL, 1003, 'bob', '123456', 'bob@example.com', '2026-05-19 00:00:00');

INSERT INTO `userInfo` (`uid`, `name`, `age`, `sex`, `headImageURL`) VALUES
  (1001, 'Zorjen', 24, 'male', '/images/default-zorjen.png'),
  (1002, 'Alice', 23, 'female', '/images/default-alice.png'),
  (1003, 'Bob', 25, 'male', '/images/default-bob.png');

INSERT INTO `friends` (`friendshipId`, `uidA`, `uidB`, `createTime`) VALUES
  (9001002, 1001, 1002, '2026-05-19 00:00:00');

INSERT INTO `conversations` (`conversationId`, `type`, `businessId`, `latestSeq`, `createTime`) VALUES
  (92001, 1, 9001002, 2, '2026-05-19 00:00:00'),
  (93001, 2, 3001, 0, '2026-05-19 00:00:00');

INSERT INTO `conversationUserStates` (`conversationId`, `uid`, `joinedSeq`, `leftSeq`, `deliveredSeq`, `readSeq`) VALUES
  (92001, 1001, 1, NULL, 2, 2),
  (92001, 1002, 1, NULL, 2, 2),
  (93001, 1001, 1, NULL, 0, 0),
  (93001, 1002, 1, NULL, 0, 0);

INSERT INTO `friendApplys` (`fromUid`, `toUid`, `content`, `status`, `createTime`) VALUES
  (1001, 1002, 'hello', 1, '2026-05-19 00:00:00'),
  (1002, 1001, 'hello', 1, '2026-05-19 00:00:00');

INSERT INTO `messages` (`messageId`, `senderId`, `receiverId`, `conversationId`, `conversationSeq`, `clientMessageId`, `commandHash`, `type`, `content`, `status`, `sendDateTime`, `readDateTime`) VALUES
  (1000001, 1001, 1002, 92001, 1, NULL, NULL, 1, 'Hello Alice', 2, '2026-05-19 00:00:00', '2026-05-19 00:00:01'),
  (1000002, 1002, 1001, 92001, 2, NULL, NULL, 1, 'Hello Zorjen', 2, '2026-05-19 00:00:02', '2026-05-19 00:00:03');

INSERT INTO `groupInfo` (`groupId`, `masterUid`, `name`, `conversationId`, `createTime`) VALUES
  (3001, 1001, 'test-group', 93001, '2026-05-19 00:00:00');

INSERT INTO `groupMembers` (`groupId`, `uid`, `joinTime`) VALUES
  (3001, 1001, '2026-05-19 00:00:00'),
  (3001, 1002, '2026-05-19 00:00:00');

INSERT INTO `groupApplys` (`groupId`, `requestorUid`, `message`, `createTime`) VALUES
  (3001, 1003, 'join test group', '2026-05-19 00:00:00');
