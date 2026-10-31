-- 전공책 장터 거래 정보 (원하는 학교 장소 · 거래 가능한 시간 · 희망 거래 시간)
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 열이 없으면 스스로 추가하므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
--
--   trade_places  원하는 학교 장소 (화면의 장소 버튼으로 고른 것, 예: "중앙도서관 · 정문")
--   trade_times   거래 가능한 시간 (요일·시간대 버튼, 예: "월·수·금 / 점심·오후")
--   trade_wish    희망 거래 시간 (예: "수요일 12:30")

USE sku_contest;

-- 이미 있으면 "Duplicate column" 오류가 나는데 무시해도 된다.
ALTER TABLE books
  ADD COLUMN trade_places VARCHAR(200) NOT NULL DEFAULT '' AFTER description,
  ADD COLUMN trade_times  VARCHAR(200) NOT NULL DEFAULT '' AFTER trade_places,
  ADD COLUMN trade_wish   VARCHAR(100) NOT NULL DEFAULT '' AFTER trade_times;
