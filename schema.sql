-- ============================================================
--  云存储文件系统 · 数据库初始化脚本
--  数据库：ai_cloud_storage
--  用法：mysql -u root -p < schema.sql
--
--  共 5 张表：
--    user_info        用户账号
--    file_info        物理文件元信息（以 MD5 为主键，内容寻址 + 引用计数）
--    user_file_list   用户与文件的关联（多对多）
--    user_file_count  用户文件数（用于列表页快速取数）
--    share_file_list  分享记录（提取码 / 有效期 / 下载量 pv）
-- ============================================================

CREATE DATABASE IF NOT EXISTS ai_cloud_storage
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_general_ci;

USE ai_cloud_storage;

-- ------------------------------------------------------------
-- 1. 用户表
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS user_info (
    id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_name     VARCHAR(64)  NOT NULL COMMENT '登录名',
    password_md5  CHAR(32)     NOT NULL COMMENT '密码 MD5（32 位十六进制）',
    nickname      VARCHAR(64)  DEFAULT NULL COMMENT '昵称',
    create_time   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_user_name (user_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户账号表';

-- ------------------------------------------------------------
-- 2. 文件元信息表
--    以 file_md5 作为主键实现内容寻址：同一份物理文件只保留一行，
--    ref_count 记录被多少个用户引用，删除时归零才真正删除 FastDFS 文件。
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS file_info (
    file_md5     CHAR(32)       NOT NULL COMMENT '文件 MD5，主键（内容寻址）',
    file_size    BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '文件字节数',
    file_id      VARCHAR(255)   NOT NULL DEFAULT '' COMMENT 'FastDFS file_id',
    file_url     VARCHAR(255)   NOT NULL DEFAULT '' COMMENT '下载地址（/groupX/M00/...）',
    ref_count    INT UNSIGNED   NOT NULL DEFAULT 1  COMMENT '引用计数',
    create_time  DATETIME       NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (file_md5)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='物理文件元信息表';

-- ------------------------------------------------------------
-- 3. 用户-文件关联表
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS user_file_list (
    id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_name     VARCHAR(64)  NOT NULL,
    file_md5      CHAR(32)     NOT NULL,
    file_name     VARCHAR(255) NOT NULL DEFAULT '' COMMENT '用户侧显示文件名',
    share_status  TINYINT      NOT NULL DEFAULT 0  COMMENT '是否已分享：0 否 1 是',
    pv            INT UNSIGNED NOT NULL DEFAULT 0  COMMENT '下载次数',
    create_time   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_user_md5 (user_name, file_md5)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户与文件的关联表';

-- ------------------------------------------------------------
-- 4. 用户文件计数表
--    每个用户一行，上传 / 删除时同步增减，列表页无需 COUNT(*)。
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS user_file_count (
    user_name   VARCHAR(64)  NOT NULL,
    file_count  INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (user_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户文件数计数表';

-- ------------------------------------------------------------
-- 5. 分享记录表
--    expire_time 为 NULL 表示永久有效，否则由 DATE_ADD(NOW(), INTERVAL ...) 生成。
--    下载热度排行榜在 Redis ZSet 中维护，本表的 pv 作为权威数据用于回填。
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS share_file_list (
    id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    share_id      VARCHAR(64)  NOT NULL COMMENT '分享标识：share_<时间戳>_<随机数>',
    user_name     VARCHAR(64)  NOT NULL COMMENT '分享者',
    file_md5      CHAR(32)     NOT NULL,
    file_name     VARCHAR(255) NOT NULL DEFAULT '',
    extract_code  VARCHAR(16)  NOT NULL DEFAULT '' COMMENT '提取码，空为免提取码',
    expire_time   DATETIME     DEFAULT NULL COMMENT '过期时间，NULL 表示永久',
    share_url     VARCHAR(255) NOT NULL DEFAULT '',
    pv            INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '下载次数',
    create_time   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uk_share_id (share_id),
    KEY idx_user_name (user_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='文件分享记录表';
