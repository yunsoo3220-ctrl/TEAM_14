-- 모집글 "마음에 들어요"
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
--
-- 추천 모델(src/ml.c)이 이 표를 이렇게 쓴다.
--   관련 공모전: 학생이 공모전에 연결된 모집글을 좋아하면 (그 공모전, 학생 학과) 를 학습 데이터로 (가중 0.5)
--   개인 맞춤:   좋아한 모집글의 제목·키워드를 내 관심사에 더한다
--   팀원 찾기:   좋아한 모집글 내용을 회원 문서에 넣고, 같은 글을 좋아한 회원 · 내 글을 좋아한 회원에게 가산점

USE sku_contest;

CREATE TABLE IF NOT EXISTS recruit_likes (
  recruit_id INT UNSIGNED NOT NULL,
  user_id    INT UNSIGNED NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (recruit_id, user_id),
  KEY ix_recruit_likes_user (user_id),
  CONSTRAINT fk_rl_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rl_user    FOREIGN KEY (user_id)    REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;
