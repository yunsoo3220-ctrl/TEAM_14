-- 전공책 가격 상한을 없앤다: INT UNSIGNED(최대 약 42억 원)는 큰 값을 넣으면 넘쳐서 BIGINT UNSIGNED 로 넓힌다.
-- 이미 쓰고 있던 DB 에 적용하는 변경분. 01_schema.sql 을 처음부터 돌렸다면 필요 없다.
-- 서버가 시작할 때 열이 INT 면 스스로 바꾸므로(cleanup_ensure_schema) 보통은 직접 돌리지 않아도 된다.
-- 실제 상한은 서버가 9,007,199,254,740,991원(2^53 - 1, 브라우저가 정확히 다루는 가장 큰 정수)으로 검사한다.

USE sku_contest;

ALTER TABLE books MODIFY price BIGINT UNSIGNED NOT NULL DEFAULT 0;
