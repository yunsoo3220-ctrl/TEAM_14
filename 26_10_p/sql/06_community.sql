-- 관심 키워드 · 자기소개 · 팀원 모집 게시판
-- 이미 쓰고 있던 DB 에 한 번만 적용하는 변경분 (ALTER 가 있어 두 번 돌리면 실패한다).
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다. 키워드 목록은 07_interest_tags.sql.

USE sku_contest;

-- 자유롭게 적는 자기소개 (관심 분야, 해 본 활동, 찾는 팀원 등). 다른 회원에게 보인다.
ALTER TABLE users ADD COLUMN bio VARCHAR(500) NOT NULL DEFAULT '' AFTER department_id;

-- 가입할 때 고르는 정해진 키워드
CREATE TABLE IF NOT EXISTS interest_tags (
  id       INT UNSIGNED NOT NULL AUTO_INCREMENT,
  category ENUM('field','role','style') NOT NULL,   -- 관심 분야 / 맡고 싶은 역할 / 협업 성향
  name     VARCHAR(30)  NOT NULL,
  sort     SMALLINT     NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uq_interest_tags_name (name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS user_interests (
  user_id INT UNSIGNED NOT NULL,
  tag_id  INT UNSIGNED NOT NULL,
  PRIMARY KEY (user_id, tag_id),
  KEY ix_user_interests_tag (tag_id),
  CONSTRAINT fk_ui_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_ui_tag  FOREIGN KEY (tag_id)  REFERENCES interest_tags (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 학생이 직접 쓰는 팀원 모집 글. 공모전 게시물(posts)과 이어 둘 수 있다.
CREATE TABLE IF NOT EXISTS recruits (
  id          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  author_id   INT UNSIGNED NOT NULL,
  post_id     INT UNSIGNED NULL,                    -- 함께 나갈 공모전
  title       VARCHAR(200) NOT NULL,
  body        TEXT         NOT NULL,
  need_people SMALLINT     NOT NULL DEFAULT 1,
  deadline    DATE         NULL,                    -- 모집 마감일
  status      ENUM('open','closed') NOT NULL DEFAULT 'open',
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_recruits_status (status, created_at),
  KEY ix_recruits_post (post_id),
  CONSTRAINT fk_recruits_author FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_recruits_post   FOREIGN KEY (post_id)   REFERENCES posts (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- 모집 글이 찾는 분야·역할
CREATE TABLE IF NOT EXISTS recruit_tags (
  recruit_id INT UNSIGNED NOT NULL,
  tag_id     INT UNSIGNED NOT NULL,
  PRIMARY KEY (recruit_id, tag_id),
  CONSTRAINT fk_rt_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rt_tag     FOREIGN KEY (tag_id)     REFERENCES interest_tags (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS recruit_comments (
  id         INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  recruit_id INT UNSIGNED  NOT NULL,
  author_id  INT UNSIGNED  NOT NULL,
  body       VARCHAR(1000) NOT NULL,
  created_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_recruit_comments (recruit_id, created_at),
  CONSTRAINT fk_rc_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rc_author  FOREIGN KEY (author_id)  REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;
