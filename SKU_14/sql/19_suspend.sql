-- 회원 정지 (관리자가 회원 관리에서 며칠 동안 정지)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 열이 없으면 스스로 추가하므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   suspended_until  이 시각까지 정지 (NULL 이거나 지난 시각이면 정지 아님 - 기간이 지나면 저절로 풀린다)
--   suspend_reason   정지 사유 (회원 알림함으로도 보낸다)
-- 정지 중인 회원은 보기만 할 수 있고, 쓰기 요청은 서버 입구(src/api.c 의 suspended_blocked)에서 막힌다.

USE sku_contest;

-- 이미 있으면 "Duplicate column" 오류가 나는데 무시해도 된다.
ALTER TABLE users
  ADD COLUMN suspended_until DATETIME     NULL               AFTER role,
  ADD COLUMN suspend_reason  VARCHAR(300) NOT NULL DEFAULT '' AFTER suspended_until;
