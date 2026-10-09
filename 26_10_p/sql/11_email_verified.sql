-- 인증 코드를 [확인] 버튼으로 먼저 맞춰 두는 단계.
-- 10_email_verify.sql 을 이미 적용한 DB 에 한 번만 적용한다 (01_schema.sql 로 새로 만들었다면 필요 없다).
USE sku_contest;

-- 코드가 맞은 시각. 이 뒤로 30분 안에 가입을 마치면 된다. 코드를 다시 받으면 비운다.
ALTER TABLE email_verifications ADD COLUMN verified_at DATETIME NULL AFTER attempts;
