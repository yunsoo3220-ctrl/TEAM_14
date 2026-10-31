-- 그룹 스터디 대상 학과를 여러 개로 (스터디를 만들 때 대상 학과를 여러 개 고른다)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 표가 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   study_departments  (스터디, 대상 학과). 행이 없으면 학과 무관 (누구나 참여)
--   대상 학과가 있으면 그 학과 학생만 참여할 수 있다 (참여하기·방장의 멤버 추가·참여 권유 모두)
-- 예전의 study_groups.department_id(학과 하나)는 옮겨 담고 더는 쓰지 않는다.
-- 정원 상한도 8명으로 줄었다 (이미 8명보다 많이 모인 스터디는 지금 인원까지만 줄인다).

USE sku_contest;

CREATE TABLE IF NOT EXISTS study_departments (
  group_id      INT UNSIGNED NOT NULL,
  department_id INT UNSIGNED NOT NULL,
  PRIMARY KEY (group_id, department_id),
  KEY ix_study_departments_department (department_id),
  CONSTRAINT fk_sdep_group FOREIGN KEY (group_id) REFERENCES study_groups (id) ON DELETE CASCADE,
  CONSTRAINT fk_sdep_department FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;

INSERT IGNORE INTO study_departments (group_id, department_id)
SELECT id, department_id FROM study_groups WHERE department_id IS NOT NULL;

UPDATE study_groups g
SET g.max_members = GREATEST(8, (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id))
WHERE g.max_members > 8;
