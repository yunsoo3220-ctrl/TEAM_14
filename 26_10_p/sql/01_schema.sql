-- 서경대학교 공모전 팀원 모집 커뮤니티 - 스키마
-- MySQL 8.x / utf8mb4
--
-- 핵심 규칙: 관리자가 공모전 게시물을 올릴 때 대상 학과를 체크하고(post_departments),
--            체크된 학과에 소속된 사용자만 해당 게시물에 댓글을 쓸 수 있다.

DROP DATABASE IF EXISTS sku_contest;
CREATE DATABASE sku_contest
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;
USE sku_contest;

-- ---------------------------------------------------------------- 대학 / 학과

CREATE TABLE colleges (
  id    INT UNSIGNED NOT NULL AUTO_INCREMENT,
  name  VARCHAR(60)  NOT NULL,
  slug  VARCHAR(80)  NOT NULL,          -- skuniv.ac.kr 의 영문 경로
  sort  SMALLINT     NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uq_colleges_name (name)
) ENGINE=InnoDB;

CREATE TABLE departments (
  id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
  college_id INT UNSIGNED NOT NULL,
  name       VARCHAR(60)  NOT NULL,
  sort       SMALLINT     NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uq_departments_name (name),
  KEY ix_departments_college (college_id),
  CONSTRAINT fk_departments_college
    FOREIGN KEY (college_id) REFERENCES colleges (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- -------------------------------------------------------------------- 사용자

CREATE TABLE users (
  id            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  student_no    VARCHAR(20)  NOT NULL,            -- 학번 (로그인 ID)
  name          VARCHAR(40)  NOT NULL,            -- 실명. 화면에 그대로 내보내지 않는다.
  phone         VARCHAR(20)  NOT NULL DEFAULT '', -- 전화번호. 화면에 그대로 내보내지 않는다.
  email         VARCHAR(120) NULL,                -- 인증한 학교 이메일 (@skuniv.ac.kr). 관리자는 NULL
  nickname      VARCHAR(40)  NOT NULL,            -- 게시판 표시 이름. 가입할 때 서버가
                                                  -- 이름과 전화번호로 만든다 (손*권_9948).
                                                  -- 사용자가 직접 정하지 않는다.
  department_id INT UNSIGNED NULL,                -- 관리자는 NULL 허용
  bio           VARCHAR(500) NOT NULL DEFAULT '', -- 자유롭게 적는 자기소개 (다른 회원에게 보인다)
  role         ENUM('student','admin') NOT NULL DEFAULT 'student',
  pw_salt       CHAR(32)     NOT NULL,            -- hex 16바이트
  pw_hash       CHAR(64)     NOT NULL,            -- hex SHA-256 (반복 해싱)
  created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_users_student_no (student_no),
  UNIQUE KEY uq_users_email (email),
  KEY ix_users_department (department_id),
  CONSTRAINT fk_users_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- 가입 때 보낸 학교 이메일 인증 코드. 코드는 해시로만 남기고, 가입에 성공하면 지운다.
CREATE TABLE email_verifications (
  email      VARCHAR(120) NOT NULL,
  code_salt  CHAR(32)     NOT NULL,
  code_hash  CHAR(64)     NOT NULL,
  attempts   TINYINT UNSIGNED NOT NULL DEFAULT 0,   -- 틀린 횟수 (5번이면 코드 무효)
  verified_at DATETIME    NULL,                     -- [확인]으로 코드가 맞은 시각 (뒤로 30분 안에 가입)
  sent_at    DATETIME     NOT NULL,
  expires_at DATETIME     NOT NULL,                 -- 보낸 뒤 10분, 확인하면 그때부터 30분
  PRIMARY KEY (email)
) ENGINE=InnoDB;

CREATE TABLE sessions (
  token      CHAR(48)     NOT NULL,
  user_id    INT UNSIGNED NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  expires_at DATETIME     NOT NULL,
  PRIMARY KEY (token),
  KEY ix_sessions_user (user_id),
  KEY ix_sessions_expires (expires_at),
  CONSTRAINT fk_sessions_user
    FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- -------------------------------------------------------------------- 게시물

CREATE TABLE posts (
  id          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  author_id   INT UNSIGNED NULL,                  -- 작성자 탈퇴 시 NULL
  kind        ENUM('contest','hackathon','etc') NOT NULL DEFAULT 'contest',
  title       VARCHAR(200) NOT NULL,
  body        TEXT         NOT NULL,
  source_url  VARCHAR(500) NULL,                  -- 원본 공지 링크
  host        VARCHAR(120) NULL,                  -- 주최 기관
  deadline    DATE         NULL,                  -- 모집 마감일
  need_people SMALLINT     NOT NULL DEFAULT 0,    -- 모집 인원 (0 = 미정)
  view_count  INT UNSIGNED NOT NULL DEFAULT 0,
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_posts_created (created_at),
  KEY ix_posts_author (author_id),
  CONSTRAINT fk_posts_author
    FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- 게시물이 대상으로 체크한 학과. 댓글 권한의 근거가 되는 표.
CREATE TABLE post_departments (
  post_id       INT UNSIGNED NOT NULL,
  department_id INT UNSIGNED NOT NULL,
  PRIMARY KEY (post_id, department_id),
  KEY ix_post_departments_dept (department_id),
  CONSTRAINT fk_pd_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE,
  CONSTRAINT fk_pd_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE comments (
  id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
  post_id    INT UNSIGNED NOT NULL,
  author_id  INT UNSIGNED NULL,
  body       VARCHAR(1000) NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_comments_post (post_id, created_at),
  CONSTRAINT fk_comments_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE,
  CONSTRAINT fk_comments_author
    FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- ------------------------------------------------- 학교 홈페이지에서 수집한 공지

CREATE TABLE notices (
  id           INT UNSIGNED NOT NULL AUTO_INCREMENT,
  ext_id       INT UNSIGNED NOT NULL,             -- skuniv WP 글 번호
  keyword      VARCHAR(40)  NOT NULL,             -- 수집 검색어 (공모전 / 해커톤)
  title        VARCHAR(300) NOT NULL,
  url          VARCHAR(500) NOT NULL,
  published_at DATE         NOT NULL,
  status       ENUM('pending','published','ignored') NOT NULL DEFAULT 'pending',
  post_id      INT UNSIGNED NULL,                 -- 게시물로 올린 경우 연결
  fetched_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_notices_ext (ext_id),
  KEY ix_notices_status (status, published_at),
  CONSTRAINT fk_notices_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- ------------------------------------------------------ AI 게시물 분석 (src/ai.c)

-- 게시물마다 한 번 Claude 로 분석한 요약·태그·주최·마감일
CREATE TABLE post_ai (
  post_id     INT UNSIGNED  NOT NULL,
  summary     VARCHAR(1000) NOT NULL,
  tags        VARCHAR(1000) NOT NULL DEFAULT '[]',  -- JSON 문자열 배열 (서버가 만든 값)
  host        VARCHAR(120)  NULL,
  deadline    DATE          NULL,
  model       VARCHAR(60)   NOT NULL,
  analyzed_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (post_id),
  CONSTRAINT fk_post_ai_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 학과별 관련도. 추천의 근거. 댓글 권한(post_departments)과는 별개다.
CREATE TABLE post_ai_departments (
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

-- 자체 추천 모델(src/ml.c)의 학과 프로필. 내용은 05_dept_profiles.sql 이 넣는다.
CREATE TABLE dept_profiles (
  department_id INT UNSIGNED NOT NULL,
  keywords      TEXT         NOT NULL,
  updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (department_id),
  CONSTRAINT fk_dept_profiles_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ------------------------------------------------ 관심 키워드 / 팀원 모집 게시판

-- 가입할 때 고르는 정해진 키워드. 목록은 07_interest_tags.sql 이 넣는다.
CREATE TABLE interest_tags (
  id       INT UNSIGNED NOT NULL AUTO_INCREMENT,
  category ENUM('field','role','style') NOT NULL,   -- 관심 분야 / 맡고 싶은 역할 / 협업 성향
  name     VARCHAR(30)  NOT NULL,
  sort     SMALLINT     NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uq_interest_tags_name (name)
) ENGINE=InnoDB;

CREATE TABLE user_interests (
  user_id INT UNSIGNED NOT NULL,
  tag_id  INT UNSIGNED NOT NULL,
  PRIMARY KEY (user_id, tag_id),
  KEY ix_user_interests_tag (tag_id),
  CONSTRAINT fk_ui_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_ui_tag  FOREIGN KEY (tag_id)  REFERENCES interest_tags (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 학생이 직접 쓰는 팀원 모집 글. 공모전 게시물(posts)과 이어 둘 수 있다.
CREATE TABLE recruits (
  id          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  author_id   INT UNSIGNED NOT NULL,
  post_id     INT UNSIGNED NULL,
  title       VARCHAR(200) NOT NULL,
  body        TEXT         NOT NULL,
  need_people SMALLINT     NOT NULL DEFAULT 1,
  deadline    DATE         NULL,
  status      ENUM('open','closed') NOT NULL DEFAULT 'open',
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_recruits_status (status, created_at),
  KEY ix_recruits_post (post_id),
  CONSTRAINT fk_recruits_author FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_recruits_post   FOREIGN KEY (post_id)   REFERENCES posts (id) ON DELETE SET NULL
) ENGINE=InnoDB;

CREATE TABLE recruit_tags (
  recruit_id INT UNSIGNED NOT NULL,
  tag_id     INT UNSIGNED NOT NULL,
  PRIMARY KEY (recruit_id, tag_id),
  CONSTRAINT fk_rt_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rt_tag     FOREIGN KEY (tag_id)     REFERENCES interest_tags (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE recruit_comments (
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

-- 모집글 "마음에 들어요". 추천 모델(src/ml.c)이 관련 공모전·팀원을 찾는 근거로도 쓴다.
CREATE TABLE recruit_likes (
  recruit_id INT UNSIGNED NOT NULL,
  user_id    INT UNSIGNED NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (recruit_id, user_id),
  KEY ix_recruit_likes_user (user_id),
  CONSTRAINT fk_rl_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rl_user    FOREIGN KEY (user_id)    REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 마지막 수집 시각 등 서버 설정값
CREATE TABLE app_config (
  k VARCHAR(40)  NOT NULL,
  v VARCHAR(200) NOT NULL,
  PRIMARY KEY (k)
) ENGINE=InnoDB;

-- 게시물 목록에서 자주 쓰는 집계를 미리 묶어둔 뷰
CREATE OR REPLACE VIEW post_list AS
SELECT p.id, p.kind, p.title, p.host, p.deadline, p.need_people,
       p.view_count, p.created_at, p.source_url,
       u.nickname AS author_nickname,
       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id)         AS comment_count,
       (SELECT COUNT(*) FROM post_departments d WHERE d.post_id = p.id) AS department_count
FROM posts p
LEFT JOIN users u ON u.id = p.author_id;
