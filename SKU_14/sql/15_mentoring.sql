-- 멘토링 (그룹 스터디와 같은 구조, study_groups.kind 로 구분)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 열이 없으면 스스로 추가하므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.

USE sku_contest;

-- 이미 있으면 "Duplicate column" 오류가 나는데 무시해도 된다.
ALTER TABLE study_groups ADD COLUMN kind ENUM('study','mentoring') NOT NULL DEFAULT 'study' AFTER end_date;
