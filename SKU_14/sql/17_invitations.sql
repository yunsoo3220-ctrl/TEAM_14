-- 참여 권유 (팀원 모집글 · 그룹 스터디 · 멘토링에 회원을 메일로 초대)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 표가 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   invitations  (대상 종류, 대상 id, 받는 회원) - 같은 모임에서 같은 회원에게는 한 번만 (유일 키)
--   대상이 모집글(recruits)·스터디(study_groups) 두 종류라 외래 키를 걸 수 없어서,
--   대상이 지워진 기록은 자동 정리(cleanup.c)가 지운다.

USE sku_contest;

CREATE TABLE IF NOT EXISTS invitations (
  id          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  target_kind ENUM('recruit','study') NOT NULL,          -- recruit = 팀원 모집글, study = 그룹 스터디·멘토링
  target_id   INT UNSIGNED NOT NULL,                     -- recruits.id 또는 study_groups.id
  inviter_id  INT UNSIGNED NOT NULL,                     -- 권유한 회원 (작성자·방장)
  invitee_id  INT UNSIGNED NOT NULL,                     -- 권유받은 회원
  accepted_at DATETIME     NULL,                         -- 알림함에서 [참여하기] 를 누른 시각 (NULL = 아직)
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_invitations (target_kind, target_id, invitee_id),
  KEY ix_invitations_inviter (inviter_id, created_at),
  CONSTRAINT fk_inv_inviter FOREIGN KEY (inviter_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_inv_invitee FOREIGN KEY (invitee_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 표를 accepted_at 없이 먼저 만든 DB 라면 (이미 있으면 "Duplicate column" 오류는 무시해도 된다)
-- ALTER TABLE invitations ADD COLUMN accepted_at DATETIME NULL AFTER invitee_id;
