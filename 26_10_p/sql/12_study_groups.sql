-- 그룹 스터디 (스터디 이름 · 정원 · 멤버 · 멘토/멘티)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
--
--   study_groups   스터디 하나 (만든 사람 = 방장, 멤버 추가·역할 지정·삭제 권한)
--   study_members  (스터디, 회원, 역할) - 역할은 member(일반) / mentor(멘토) / mentee(멘티)

USE sku_contest;

CREATE TABLE IF NOT EXISTS study_groups (
  id          INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  owner_id    INT UNSIGNED  NOT NULL,                 -- 방장
  name        VARCHAR(100)  NOT NULL,                 -- 스터디 이름
  description VARCHAR(1000) NOT NULL DEFAULT '',
  max_members SMALLINT      NOT NULL DEFAULT 10,      -- 정원 (2~50, 서버에서 검사)
  created_at  DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_study_groups_owner (owner_id),
  CONSTRAINT fk_sg_owner FOREIGN KEY (owner_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS study_members (
  group_id  INT UNSIGNED NOT NULL,
  user_id   INT UNSIGNED NOT NULL,
  role      ENUM('member','mentor','mentee') NOT NULL DEFAULT 'member',
  joined_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (group_id, user_id),                    -- 한 스터디에 한 번만
  KEY ix_study_members_user (user_id),
  CONSTRAINT fk_sm_group FOREIGN KEY (group_id) REFERENCES study_groups (id) ON DELETE CASCADE,
  CONSTRAINT fk_sm_user  FOREIGN KEY (user_id)  REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;
