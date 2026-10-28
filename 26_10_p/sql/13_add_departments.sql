-- 이공대학에 컴퓨터공학과 · 전자공학과 추가
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 02_seed_departments.sql 을 처음부터 돌렸다면 필요 없다.
-- (02 는 학과 표를 비우고 다시 넣으므로 쓰던 DB 에 돌리면 게시물의 대상 학과 체크가 사라진다)
--
-- 적용한 뒤 05_dept_profiles.sql 을 다시 돌려 두 학과의 추천 프로필을 넣는다.

USE sku_contest;

-- 학과 이름에 UNIQUE 키가 있으므로, 이미 있으면 IGNORE 로 건너뛴다
INSERT IGNORE INTO departments (college_id, name, sort) VALUES
  (2, '컴퓨터공학과', 80),
  (2, '전자공학과',   90);
