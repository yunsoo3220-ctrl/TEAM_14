-- scripts/seed_demo.py 가 만든 가상 회원을 지운다.
-- 그들의 세션 · 관심 키워드 · 모집글 · 모집글 댓글은 외래 키로 함께 지워진다.
-- 공모전 게시물에 단 댓글은 작성자만 비워진다 (가상 회원은 그런 댓글을 쓰지 않는다).
USE sku_contest;
DELETE FROM users WHERE student_no LIKE '2099%' AND role = 'student';
