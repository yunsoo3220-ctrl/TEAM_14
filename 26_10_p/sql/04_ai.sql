-- AI 게시물 분석 결과 (src/ai.c)
-- 이미 쓰고 있던 DB 에 추가하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
--
-- 게시물마다 한 번 Claude 로 분석해 요약·태그·주최·마감일과 학과별 관련도를 저장한다.
-- 추천은 저장된 점수로만 계산하므로 화면을 볼 때마다 API 를 부르지 않는다.

USE sku_contest;

CREATE TABLE IF NOT EXISTS post_ai (
  post_id     INT UNSIGNED  NOT NULL,
  summary     VARCHAR(1000) NOT NULL,
  tags        VARCHAR(1000) NOT NULL DEFAULT '[]',  -- JSON 문자열 배열 (서버가 만든 값)
  host        VARCHAR(120)  NULL,                   -- 본문에서 찾은 주최 기관
  deadline    DATE          NULL,                   -- 본문에서 찾은 접수 마감일
  model       VARCHAR(60)   NOT NULL,
  analyzed_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (post_id),
  CONSTRAINT fk_post_ai_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 학과별 관련도. 추천의 근거가 되는 표. 댓글 권한(post_departments)과는 별개다.
CREATE TABLE IF NOT EXISTS post_ai_departments (
  post_id       INT UNSIGNED     NOT NULL,
  department_id INT UNSIGNED     NOT NULL,
  score         TINYINT UNSIGNED NOT NULL,          -- 0 ~ 100
  reason        VARCHAR(400)     NOT NULL,
  PRIMARY KEY (post_id, department_id),
  KEY ix_post_ai_dept (department_id, score),
  CONSTRAINT fk_pad_ai_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE,
  CONSTRAINT fk_pad_ai_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;
