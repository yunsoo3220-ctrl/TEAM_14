-- 전공책 장터 (중고 전공책 사고팔기 · 책 사진 · 문의 댓글)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 표가 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   books          판매 글 (seller_id = 판매자, status = 판매 중 / 예약 중 / 판매 완료)
--   book_photos    책 사진 (이미지 파일 그대로 MEDIUMBLOB 에, 한 권에 최대 5장)
--   book_comments  문의 댓글

USE sku_contest;

CREATE TABLE IF NOT EXISTS books (
  id             INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  seller_id      INT UNSIGNED  NOT NULL,                -- 판매자
  title          VARCHAR(200)  NOT NULL,                -- 책 제목
  author         VARCHAR(100)  NOT NULL DEFAULT '',     -- 저자·출판사
  course         VARCHAR(100)  NOT NULL DEFAULT '',     -- 쓰는 과목 이름
  department_id  INT UNSIGNED  NULL,                    -- 관련 학과 (없으면 NULL)
  price          INT UNSIGNED  NOT NULL DEFAULT 0,      -- 가격(원), 0 = 나눔
  book_condition ENUM('new','good','fair','poor') NOT NULL DEFAULT 'good',  -- 새 책 / 깨끗함 / 필기 조금 / 필기 많음
  description    VARCHAR(2000) NOT NULL DEFAULT '',
  status         ENUM('selling','reserved','sold') NOT NULL DEFAULT 'selling',
  sold_at        DATETIME      NULL,                    -- 판매 완료한 시각 (30일 뒤 자동 삭제)
  created_at     DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_books_status (status, created_at),
  KEY ix_books_seller (seller_id),
  KEY ix_books_department (department_id),
  CONSTRAINT fk_books_seller     FOREIGN KEY (seller_id)     REFERENCES users (id)       ON DELETE CASCADE,
  CONSTRAINT fk_books_department FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE SET NULL
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS book_photos (
  id         INT UNSIGNED     NOT NULL AUTO_INCREMENT,
  book_id    INT UNSIGNED     NOT NULL,
  sort_order TINYINT UNSIGNED NOT NULL DEFAULT 0,       -- 0 이 대표 사진
  mime       VARCHAR(20)      NOT NULL,                 -- image/jpeg, image/png, image/webp
  data       MEDIUMBLOB       NOT NULL,                 -- 이미지 파일 내용 (최대 16MB, 서버는 1MB 까지만 받는다)
  PRIMARY KEY (id),
  KEY ix_book_photos_book (book_id, sort_order),
  CONSTRAINT fk_bp_book FOREIGN KEY (book_id) REFERENCES books (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS book_comments (
  id         INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  book_id    INT UNSIGNED  NOT NULL,
  author_id  INT UNSIGNED  NOT NULL,
  body       VARCHAR(1000) NOT NULL,
  created_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_book_comments (book_id, created_at),
  CONSTRAINT fk_bc_book   FOREIGN KEY (book_id)   REFERENCES books (id) ON DELETE CASCADE,
  CONSTRAINT fk_bc_author FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;
