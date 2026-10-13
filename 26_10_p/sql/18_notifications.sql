-- 알림함 (로그인한 회원에게 온 알림. 지금은 참여 권유가 쓴다)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 표가 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   notifications  받는 회원(user_id)의 알림. read_at 이 NULL 이면 안 읽음. 90일 뒤 자동 정리(cleanup.c)

USE sku_contest;

CREATE TABLE IF NOT EXISTS notifications (
  id         INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  user_id    INT UNSIGNED  NOT NULL,                   -- 받는 회원
  actor_id   INT UNSIGNED  NULL,                       -- 알림을 일으킨 회원 (탈퇴하면 NULL)
  kind       VARCHAR(20)   NOT NULL,                   -- 종류 (invite = 참여 권유)
  title      VARCHAR(300)  NOT NULL,                   -- 한 줄 제목
  body       VARCHAR(1000) NOT NULL DEFAULT '',        -- 내용
  link       VARCHAR(200)  NOT NULL DEFAULT '',        -- 누르면 갈 화면 주소 (#/recruits/3 처럼, 없으면 '')
  read_at    DATETIME      NULL,                       -- 읽은 시각 (NULL = 안 읽음)
  created_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_notifications_user (user_id, read_at, created_at),
  CONSTRAINT fk_noti_user  FOREIGN KEY (user_id)  REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_noti_actor FOREIGN KEY (actor_id) REFERENCES users (id) ON DELETE SET NULL
) ENGINE=InnoDB;
