-- 그룹 스터디 대상 학과 (메뉴 정리: 스터디 화면의 "내 학과 대상" 탭)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 열이 없으면 스스로 추가하므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   department_id  스터디를 만들 때 고른 대상 학과 (NULL = 학과 무관, 학과가 지워지면 NULL)
-- 이미 있던 스터디는 방장의 학과로 채운다 (스터디 만들기 화면의 기본값과 같다).

USE sku_contest;

-- 이미 있으면 "Duplicate column" 오류가 나는데 무시해도 된다.
ALTER TABLE study_groups
  ADD COLUMN department_id INT UNSIGNED NULL AFTER max_members,
  ADD KEY ix_study_groups_department (department_id),
  ADD CONSTRAINT fk_sg_department FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE SET NULL;

UPDATE study_groups g JOIN users u ON u.id = g.owner_id
SET g.department_id = u.department_id
WHERE g.department_id IS NULL;
