-- 전공책 장터 1:1 채팅 (사고 싶은 사람 ↔ 판매자)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 다시 돌려도 된다.
-- 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 표·값이 없으면 스스로 만들므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   book_chats   (판매 글, 사는 사람) 하나가 채팅방 하나. 판매자는 books.seller_id
--   chat_messages · chat_reads 의 room_kind 에 'book' 을 더한다 (room_id = book_chats.id)
-- 판매 글이 지워지면 book_chats 는 외래 키로 함께 지워지고, 메시지는 자동 정리(cleanup.c)가 지운다.

USE sku_contest;

CREATE TABLE IF NOT EXISTS book_chats (
  id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
  book_id    INT UNSIGNED NOT NULL,
  buyer_id   INT UNSIGNED NOT NULL,                   -- 채팅을 건 사람 (판매자 자신은 안 됨)
  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_book_chats (book_id, buyer_id),       -- 한 책에 한 사람당 방 하나
  KEY ix_book_chats_buyer (buyer_id),
  CONSTRAINT fk_bch_book  FOREIGN KEY (book_id)  REFERENCES books (id) ON DELETE CASCADE,
  CONSTRAINT fk_bch_buyer FOREIGN KEY (buyer_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

ALTER TABLE chat_messages MODIFY room_kind ENUM('recruit','study','book') NOT NULL;
ALTER TABLE chat_reads    MODIFY room_kind ENUM('recruit','study','book') NOT NULL;
