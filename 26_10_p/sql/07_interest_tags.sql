-- 가입할 때 고르는 관심 키워드. 다시 돌려도 된다 (이름이 같으면 순서만 고친다).
USE sku_contest;

-- (분류, 이름, 표시 순서). 분류마다 sort 를 1~, 31~, 41~ 로 띄워 두어 나중에 끼워 넣기 쉽게 했다.
-- 이름이 유일 키라 같은 이름이 있으면 INSERT 대신 분류·순서만 갱신한다 (id 가 유지되어 회원 선택이 보존됨).
INSERT INTO interest_tags (category, name, sort) VALUES
  -- 관심 분야
  ('field', '영상',          1), ('field', '디자인',        2), ('field', '개발',          3),
  ('field', 'AI',            4), ('field', '데이터',        5), ('field', '기획',          6),
  ('field', '마케팅',        7), ('field', '창업',          8), ('field', '글쓰기',        9),
  ('field', '사진',         10), ('field', '음악',         11), ('field', '공연',         12),
  ('field', '사회문제',     13), ('field', '환경',         14), ('field', '교육',         15),
  ('field', '금융',         16), ('field', '공공정책',     17), ('field', '도시건축',     18),
  ('field', '뷰티',         19), ('field', '스포츠',       20), ('field', '게임',         21),
  ('field', '과학',         22), ('field', '글로벌',       23), ('field', '만화',         24),
  -- 맡고 싶은 역할
  ('role',  '팀장',         31), ('role',  '기획자',       32), ('role',  '디자이너',     33),
  ('role',  '개발자',       34), ('role',  '영상편집',     35), ('role',  '발표',         36),
  ('role',  '자료조사',     37), ('role',  '작가',         38),
  -- 협업 성향
  ('style', '꼼꼼함',       41), ('style', '아이디어형',   42), ('style', '실행력',       43),
  ('style', '소통중시',     44), ('style', '마감준수',     45), ('style', '수상목표',     46),
  ('style', '경험쌓기',     47), ('style', '온라인협업',   48), ('style', '대면모임',     49)
ON DUPLICATE KEY UPDATE category = VALUES(category), sort = VALUES(sort);
