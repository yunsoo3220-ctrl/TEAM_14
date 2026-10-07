-- 서경대학교 대학/학과 기초 데이터
-- 출처: 서경대학교 > 입학·교육 > 대학 (https://www.skuniv.ac.kr/undergraduate)
--       각 대학 페이지의 학과 목록은 스크립트로 렌더링되어 정적 수집이 불가해,
--       이공대학은 과제 명세의 목록을, 나머지는 교육과정편성표/학부 안내를 대조해 입력했다.
-- 학과 개편이 있으면 이 파일만 고쳐 다시 적용하면 된다.

USE sku_contest;

SET FOREIGN_KEY_CHECKS = 0;
TRUNCATE TABLE post_departments;
TRUNCATE TABLE departments;
TRUNCATE TABLE colleges;
SET FOREIGN_KEY_CHECKS = 1;

INSERT INTO colleges (id, name, slug, sort) VALUES
  (1, '인문사회과학대학', 'college-of-humanities-and-social-science',  10),
  (2, '이공대학',         'college-of-natural-science-and-engineering', 20),
  (3, '공연예술대학',     'college-of-arts',                            30),
  (4, '디자인&영상대학',  'college-of-design-and-digital-film',          40),
  (5, '미용예술대학',     'college-of-beauty-art',                      50),
  (6, '미래융합대학',     'college-of-interdisciplinary-studies',        60),
  (7, '융합대학',         'college-of-multidisciplinary-studies',        70),
  (8, '창의인재대학',     'college-of-creative-talents',                80);

INSERT INTO departments (college_id, name, sort) VALUES
  -- 인문사회과학대학
  (1, '글로벌비즈니스어학부', 10),
  (1, '경영학부',             20),
  (1, '공공인재학부',         30),
  (1, '아동청소년학과',       40),
  (1, '군사학과',             50),
  -- 이공대학 (과제 명세에 제시된 목록)
  (2, '소프트웨어학과',       10),
  (2, '금융정보공학과',       20),
  (2, '전자컴퓨터공학과',     30),
  (2, '나노화학생명공학과',   40),
  (2, '물류시스템공학과',     50),
  (2, '도시공학과',           60),
  (2, '토목건축공학과',       70),
  -- 공연예술대학
  (3, '음악학부',             10),
  (3, '실용음악학부',         20),
  (3, '무용예술학부',         30),
  (3, '공연예술학부',         40),
  -- 디자인&영상대학
  (4, '디자인학부',           10),
  (4, '영화영상학과',         20),
  (4, '광고홍보영상학과',     30),
  -- 미용예술대학
  (5, '헤어디자인학과',           10),
  (5, '메이크업디자인학과',       20),
  (5, '코스메틱&뷰티테라피학과',  30),
  -- 미래융합대학
  (6, '미래융합학부1',        10),
  (6, '미래융합학부2',        20),
  -- 융합대학
  (7, '아트앤테크놀로지학과',     10),
  (7, '스포츠앤테크놀로지학과',   20),
  -- 창의인재대학
  (8, '자유전공학부',         10);

INSERT INTO app_config (k, v) VALUES
  ('crawl.window_days', '92'),          -- 공지 수집 범위: 약 3개월
  ('crawl.fetched_at',  '')
ON DUPLICATE KEY UPDATE v = VALUES(v);
