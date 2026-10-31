-- 그룹 스터디 참여를 신청제로: [참여 신청] → 방장에게 알림 → 방장이 수락하면 멤버가 된다.
-- 정원이 차면 확정된 그룹(confirmed_at)이 되어 더는 신청·추가·나가기·내보내기를 할 수 없다.
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.

USE sku_contest;

-- 이미 있으면 "Duplicate column" 오류가 나는데 무시해도 된다.
ALTER TABLE study_groups ADD COLUMN confirmed_at DATETIME NULL AFTER end_date;
UPDATE study_groups g SET g.confirmed_at = NOW()
WHERE g.confirmed_at IS NULL
  AND (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id) >= g.max_members;

CREATE TABLE IF NOT EXISTS study_requests (
  group_id   INT UNSIGNED NOT NULL,
  user_id    INT UNSIGNED NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (group_id, user_id),
  KEY ix_study_requests_user (user_id),
  CONSTRAINT fk_sreq_group FOREIGN KEY (group_id) REFERENCES study_groups (id) ON DELETE CASCADE,
  CONSTRAINT fk_sreq_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;
