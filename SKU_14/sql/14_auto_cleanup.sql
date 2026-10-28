-- 지난 글 자동 정리 (src/cleanup.c) 에 필요한 변경
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 열이 없으면 스스로 추가하므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
-- 자동 정리 규칙 (서버가 1시간마다):
--   posts         마감일이 지난 공모전. 마감일을 모르는 학교 공지는 게시일로부터 60일 뒤
--   recruits      마감(closed)했거나 마감일이 지난 팀원 모집글
--   study_groups  종료일이 지난 스터디

USE sku_contest;

-- 스터디 종료일 (선택). 이미 있으면 "Duplicate column" 오류가 나는데 무시해도 된다.
ALTER TABLE study_groups ADD COLUMN end_date DATE NULL AFTER max_members;

-- 마감일을 모르는 학교 공지를 남겨 둘 날 수. 바꾸려면 숫자만 고쳐 다시 돌린다.
INSERT INTO app_config (k, v) VALUES ('cleanup.no_deadline_days', '60')
  ON DUPLICATE KEY UPDATE v = v;
