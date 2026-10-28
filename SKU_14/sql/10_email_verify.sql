-- 학교 이메일(@skuniv.ac.kr) 인증 가입
-- 이미 쓰고 있던 DB 에 한 번만 적용하는 변경분 (ALTER 가 있어 두 번 돌리면 실패한다).
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
USE sku_contest;

-- 학생은 가입할 때 학교 이메일을 인증한다. 관리자·예전 회원은 비어 있을 수 있다.
-- 열 추가와 유일 키 추가를 한 문장으로. 기존 회원은 NULL 이 되며, UNIQUE 는 NULL 끼리는 중복으로 보지 않는다.
ALTER TABLE users ADD COLUMN email VARCHAR(120) NULL AFTER phone,
                  ADD UNIQUE KEY uq_users_email (email);

-- 보낸 인증 코드. 코드는 해시로만 남기고, 가입에 성공하면 지운다.
CREATE TABLE IF NOT EXISTS email_verifications (
  email      VARCHAR(120) NOT NULL,
  code_salt  CHAR(32)     NOT NULL,
  code_hash  CHAR(64)     NOT NULL,
  attempts   TINYINT UNSIGNED NOT NULL DEFAULT 0,   -- 틀린 횟수 (5번이면 코드 무효)
  sent_at    DATETIME     NOT NULL,
  expires_at DATETIME     NOT NULL,                 -- 보낸 뒤 10분
  PRIMARY KEY (email)
) ENGINE=InnoDB;
