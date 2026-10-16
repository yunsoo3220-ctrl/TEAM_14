-- 채팅방 (팀원 모집 팀 · 그룹 스터디 · 멘토링에 들어간 사람끼리 대화)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 표가 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   recruit_members  (모집글, 회원) - 모집글 작성자가 팀원으로 받은 회원 (작성자 자신은 넣지 않는다)
--   chat_messages    방(종류, id)에 쓴 메시지. 방 = 모집글 팀(recruit) 또는 스터디·멘토링(study)
--   chat_reads       (방, 회원) 마지막으로 읽은 메시지 id - 안 읽은 메시지 수를 셀 때 쓴다
-- 방이 두 종류라 chat_messages·chat_reads 에는 방 외래 키를 걸 수 없어서,
-- 방이 지워진 기록은 자동 정리(cleanup.c)가 지운다.

USE sku_contest;

CREATE TABLE IF NOT EXISTS recruit_members (
  recruit_id INT UNSIGNED NOT NULL,
  user_id    INT UNSIGNED NOT NULL,
  joined_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (recruit_id, user_id),                 -- 한 팀에 한 번만
  KEY ix_recruit_members_user (user_id),
  CONSTRAINT fk_rm_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rm_user    FOREIGN KEY (user_id)    REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS chat_messages (
  id         INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  room_kind  ENUM('recruit','study') NOT NULL,         -- recruit = 모집글 팀, study = 그룹 스터디·멘토링
  room_id    INT UNSIGNED  NOT NULL,                   -- recruits.id 또는 study_groups.id
  user_id    INT UNSIGNED  NOT NULL,                   -- 쓴 회원
  body       VARCHAR(1000) NOT NULL,
  created_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_chat_messages_room (room_kind, room_id, id),
  CONSTRAINT fk_cm_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS chat_reads (
  room_kind    ENUM('recruit','study') NOT NULL,
  room_id      INT UNSIGNED NOT NULL,
  user_id      INT UNSIGNED NOT NULL,
  last_read_id INT UNSIGNED NOT NULL DEFAULT 0,       -- 이 id 까지 읽었다
  PRIMARY KEY (room_kind, room_id, user_id),
  KEY ix_chat_reads_user (user_id),
  CONSTRAINT fk_cr_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;
