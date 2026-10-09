-- 공지 수집 범위를 2년으로 늘린다 (추천 모델 학습 데이터를 넉넉히).
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
USE sku_contest;

INSERT INTO app_config (k, v) VALUES ('crawl.window_days', '730')
ON DUPLICATE KEY UPDATE v = VALUES(v);

-- 예전에 수집한 게시물의 등록 시각을 학교 공지 게시일로 맞춘다 (새 크롤러와 같은 기준).
UPDATE posts p JOIN notices n ON n.post_id = p.id
SET p.created_at = n.published_at
WHERE p.author_id IS NULL;
