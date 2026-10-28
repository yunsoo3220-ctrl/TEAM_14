-- 이미 만들어 둔 DB 를 새 가입 방식에 맞춘다.
-- (01_schema.sql 을 처음부터 다시 돌리는 경우에는 필요 없다.)
--
-- 바뀐 점
--   * 가입할 때 닉네임을 직접 받지 않는다. 대신 전화번호를 받는다.
--   * 게시판 표시 이름은 서버가 이름과 전화번호로 만든다: 손동권 + 010-9948-9687 -> 손*권_9948
--   * 표시 이름은 파생값이라 서로 겹칠 수 있으므로 유일 키를 뺀다.

USE sku_contest;

-- ALTER TABLE ... ADD COLUMN: 기존 표에 열을 추가한다. AFTER name 은 name 열 바로 뒤에 놓으라는 뜻.
-- 기존 행은 DEFAULT '' 로 채워진다.
ALTER TABLE users
  ADD COLUMN phone VARCHAR(20) NOT NULL DEFAULT '' AFTER name;

-- 유일 키가 없을 수도 있으므로 있을 때만 지운다.
-- MySQL 의 DROP INDEX 에는 IF EXISTS 가 없어서, information_schema(DB 의 메타 정보)로 존재 여부를 세고
-- 실행할 문장을 문자열로 고른 뒤 PREPARE/EXECUTE 로 동적 실행한다. 'DO 0' 은 아무 일도 안 하는 문장이다.
SET @has := (SELECT COUNT(*) FROM information_schema.STATISTICS
             WHERE TABLE_SCHEMA = 'sku_contest'
               AND TABLE_NAME = 'users'
               AND INDEX_NAME = 'uq_users_nickname');
SET @sql := IF(@has > 0, 'ALTER TABLE users DROP INDEX uq_users_nickname', 'DO 0');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
