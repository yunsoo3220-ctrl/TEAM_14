-- 서경대학교 공모전 팀원 모집 커뮤니티 - 스키마
-- MySQL 8.x / utf8mb4
--
-- 핵심 규칙: 관리자가 공모전 게시물을 올릴 때 대상 학과를 체크하고(post_departments),
--            체크된 학과에 소속된 사용자만 해당 게시물에 댓글을 쓸 수 있다.
--
-- 읽는 법:
--   INT UNSIGNED       0 이상 정수 (id 는 음수가 없으므로 범위를 두 배로 쓴다)
--   AUTO_INCREMENT     INSERT 할 때 id 를 1, 2, 3 ... 으로 자동 부여
--   PRIMARY KEY        행을 구분하는 기본 키 (중복·NULL 불가, 자동으로 색인됨)
--   UNIQUE KEY         중복을 허용하지 않는 색인 (예: 같은 학번으로 두 번 가입 불가)
--   KEY / INDEX        검색을 빠르게 하는 색인 (WHERE, JOIN, ORDER BY 에 자주 쓰는 열)
--   FOREIGN KEY        다른 표의 행을 가리키는 열. 없는 행을 가리키지 못하게 DB 가 막는다
--     ON DELETE CASCADE    가리키던 행이 지워지면 이 행도 함께 지운다 (예: 게시물 → 그 댓글)
--     ON DELETE SET NULL   가리키던 행이 지워지면 이 열만 NULL 로 (예: 탈퇴 회원의 글은 남김)
--   ENGINE=InnoDB      트랜잭션과 외래 키를 지원하는 MySQL 기본 저장 엔진
--
-- 주의: 아래 첫 문장은 기존 DB 를 통째로 지운다. 운영 중인 DB 에는 실행하지 말 것.

DROP DATABASE IF EXISTS sku_contest;
-- utf8mb4: 한글·이모지까지 담는 UTF-8, utf8mb4_unicode_ci: 대소문자 구분 없는 유니코드 정렬 규칙
CREATE DATABASE sku_contest
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;
USE sku_contest;

-- ---------------------------------------------------------------- 대학 / 학과

-- 단과대학 (예: 공과대학). 내용은 02_seed_departments.sql 이 넣는다.
CREATE TABLE colleges (
  id    INT UNSIGNED NOT NULL AUTO_INCREMENT,
  name  VARCHAR(60)  NOT NULL,
  slug  VARCHAR(80)  NOT NULL,          -- skuniv.ac.kr 의 영문 경로
  sort  SMALLINT     NOT NULL DEFAULT 0,       -- 화면 표시 순서 (작을수록 앞)
  PRIMARY KEY (id),
  UNIQUE KEY uq_colleges_name (name)
) ENGINE=InnoDB;

-- 학과. 단과대학이 지워지면 소속 학과도 함께 지워진다 (CASCADE).
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

-- 회원 (학생과 관리자). 비밀번호는 평문 대신 솔트와 해시만 저장한다 (src/sha256.c).
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
  UNIQUE KEY uq_users_email (email),               -- UNIQUE 는 NULL 을 여러 개 허용한다 (관리자 여럿 가능)
  KEY ix_users_department (department_id),
  CONSTRAINT fk_users_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE SET NULL   -- 학과가 없어져도 회원은 남긴다
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
  PRIMARY KEY (email)                               -- 이메일당 한 행 (다시 보내면 덮어쓴다)
) ENGINE=InnoDB;

-- 로그인 세션. 쿠키 sid 의 값이 token 이다. 만료된 행은 로그인할 때 청소한다 (src/api.c).
CREATE TABLE sessions (
  token      CHAR(48)     NOT NULL,                 -- 추측할 수 없는 난수 16진 48자
  user_id    INT UNSIGNED NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  expires_at DATETIME     NOT NULL,
  PRIMARY KEY (token),
  KEY ix_sessions_user (user_id),
  KEY ix_sessions_expires (expires_at),             -- 만료 세션 일괄 삭제를 빠르게
  CONSTRAINT fk_sessions_user
    FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- -------------------------------------------------------------------- 게시물

-- 공모전 게시물. 관리자가 직접 쓰거나 학교 공지에서 자동으로 올라온다 (author_id 가 NULL 이고 source_url 이 있음).
CREATE TABLE posts (
  id          INT UNSIGNED NOT NULL AUTO_INCREMENT,
  author_id   INT UNSIGNED NULL,                  -- 작성자 탈퇴 시 NULL
  kind        ENUM('contest','hackathon','etc') NOT NULL DEFAULT 'contest',
  title       VARCHAR(200) NOT NULL,
  body        TEXT         NOT NULL,              -- TEXT: 최대 64KB
  source_url  VARCHAR(500) NULL,                  -- 원본 공지 링크
  host        VARCHAR(120) NULL,                  -- 주최 기관
  deadline    DATE         NULL,                  -- 모집 마감일
  need_people SMALLINT     NOT NULL DEFAULT 0,    -- 모집 인원 (0 = 미정)
  view_count  INT UNSIGNED NOT NULL DEFAULT 0,
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_posts_created (created_at),                -- 최신순 목록 정렬용
  KEY ix_posts_author (author_id),
  CONSTRAINT fk_posts_author
    FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- 게시물이 대상으로 체크한 학과. 댓글 권한의 근거가 되는 표.
CREATE TABLE post_departments (
  post_id       INT UNSIGNED NOT NULL,
  department_id INT UNSIGNED NOT NULL,
  PRIMARY KEY (post_id, department_id),             -- 복합 기본 키: 같은 (게시물, 학과) 쌍은 한 번만
  KEY ix_post_departments_dept (department_id),     -- "이 학과 대상 게시물" 검색용 (기본 키는 post_id 가 앞이라)
  CONSTRAINT fk_pd_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE,
  CONSTRAINT fk_pd_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 공모전 게시물의 댓글. 게시물이 지워지면 함께 지워지고, 작성자가 탈퇴하면 author_id 만 NULL ("(탈퇴)" 로 표시).
CREATE TABLE comments (
  id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
  post_id    INT UNSIGNED NOT NULL,
  author_id  INT UNSIGNED NULL,
  body       VARCHAR(1000) NOT NULL,
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_comments_post (post_id, created_at),       -- "게시물의 댓글을 시간순으로" 질의에 딱 맞는 복합 색인
  CONSTRAINT fk_comments_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE,
  CONSTRAINT fk_comments_author
    FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- ------------------------------------------------- 학교 홈페이지에서 수집한 공지

-- 수집한 공지 원본 목록 (src/crawler.c). 같은 공지는 ext_id 로 한 행만 남는다.
CREATE TABLE notices (
  id           INT UNSIGNED NOT NULL AUTO_INCREMENT,
  ext_id       INT UNSIGNED NOT NULL,             -- skuniv WP 글 번호
  keyword      VARCHAR(40)  NOT NULL,             -- 수집 검색어 (공모전 / 해커톤)
  title        VARCHAR(300) NOT NULL,
  url          VARCHAR(500) NOT NULL,
  published_at DATE         NOT NULL,
  status       ENUM('pending','published','ignored') NOT NULL DEFAULT 'pending',   -- 대기 / 게시됨 / 숨김
  post_id      INT UNSIGNED NULL,                 -- 게시물로 올린 경우 연결
  fetched_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_notices_ext (ext_id),               -- ON DUPLICATE KEY UPDATE (upsert) 의 기준
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
  model       VARCHAR(60)   NOT NULL,               -- 분석에 쓴 모델 ID (나중에 모델을 바꿨을 때 구분)
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
  KEY ix_post_ai_dept (department_id, score),       -- "이 학과에 점수 높은 글" 추천 질의용
  CONSTRAINT fk_pad_ai_post
    FOREIGN KEY (post_id) REFERENCES posts (id) ON DELETE CASCADE,
  CONSTRAINT fk_pad_ai_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 자체 추천 모델(src/ml.c)의 학과 프로필. 내용은 05_dept_profiles.sql 이 넣는다.
CREATE TABLE dept_profiles (
  department_id INT UNSIGNED NOT NULL,
  keywords      TEXT         NOT NULL,              -- 학과를 설명하는 낱말들 (TF-IDF·임베딩의 출발 벡터)
  updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,   -- 행이 바뀔 때마다 자동 갱신
  PRIMARY KEY (department_id),
  CONSTRAINT fk_dept_profiles_department
    FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- ------------------------------------------------ 관심 키워드 / 팀원 모집 게시판

-- 가입할 때 고르는 정해진 키워드. 목록은 07_interest_tags.sql 이 넣는다.
CREATE TABLE interest_tags (
  id       INT UNSIGNED NOT NULL AUTO_INCREMENT,
  category ENUM('field','role','style') NOT NULL,   -- 관심 분야 / 맡고 싶은 역할 / 협업 성향
  name     VARCHAR(30)  NOT NULL,                  -- 화면 표시 이름이자 추천 모델의 토큰
  sort     SMALLINT     NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uq_interest_tags_name (name)
) ENGINE=InnoDB;

-- 회원이 고른 관심 키워드 (다대다 연결 표: 회원 N - 키워드 M)
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
  post_id     INT UNSIGNED NULL,                  -- 함께 나갈 공모전 (없으면 NULL, 공모전이 지워져도 모집글은 남음)
  title       VARCHAR(200) NOT NULL,
  body        TEXT         NOT NULL,
  need_people SMALLINT     NOT NULL DEFAULT 1,    -- 구하는 인원 (1~20, 서버에서 검사)
  deadline    DATE         NULL,
  status      ENUM('open','closed') NOT NULL DEFAULT 'open',
  created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_recruits_status (status, created_at),
  KEY ix_recruits_post (post_id),
  CONSTRAINT fk_recruits_author FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT fk_recruits_post   FOREIGN KEY (post_id)   REFERENCES posts (id) ON DELETE SET NULL
) ENGINE=InnoDB;

-- 모집글이 찾는 키워드 (다대다)
CREATE TABLE recruit_tags (
  recruit_id INT UNSIGNED NOT NULL,
  tag_id     INT UNSIGNED NOT NULL,
  PRIMARY KEY (recruit_id, tag_id),
  CONSTRAINT fk_rt_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rt_tag     FOREIGN KEY (tag_id)     REFERENCES interest_tags (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 모집글 댓글 (공모전 댓글과 달리 학과 제한 없음)
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
  PRIMARY KEY (recruit_id, user_id),                -- 한 사람이 같은 글에 한 번만
  KEY ix_recruit_likes_user (user_id),
  CONSTRAINT fk_rl_recruit FOREIGN KEY (recruit_id) REFERENCES recruits (id) ON DELETE CASCADE,
  CONSTRAINT fk_rl_user    FOREIGN KEY (user_id)    REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 마지막 수집 시각 등 서버 설정값
-- 키-값(k-v) 형태라 설정이 늘어도 표 구조를 바꿀 필요가 없다.
--   crawl.window_days : 공지 수집 기간(일), crawl.fetched_at : 마지막 수집 시각
CREATE TABLE app_config (
  k VARCHAR(40)  NOT NULL,
  v VARCHAR(200) NOT NULL,
  PRIMARY KEY (k)
) ENGINE=InnoDB;

-- 게시물 목록에서 자주 쓰는 집계를 미리 묶어둔 뷰
-- 뷰(VIEW)는 저장된 SELECT 문이다. 데이터를 따로 저장하지 않고, 조회할 때마다 아래 질의가 실행된다.
-- (현재 C 서버는 이 뷰 대신 직접 질의를 쓰며, Workbench 에서 확인할 때 편리하다)
CREATE OR REPLACE VIEW post_list AS
SELECT p.id, p.kind, p.title, p.host, p.deadline, p.need_people,
       p.view_count, p.created_at, p.source_url,
       u.nickname AS author_nickname,
       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id)         AS comment_count,
       (SELECT COUNT(*) FROM post_departments d WHERE d.post_id = p.id) AS department_count
FROM posts p
LEFT JOIN users u ON u.id = p.author_id;
