/* 관리자 기능: 게시물 등록/삭제, 학교 공지 수집과 게시물 전환
 *
 * 이 파일이 담당하는 API (모두 관리자 전용):
 *   POST   /api/posts                        게시물 직접 등록
 *   DELETE /api/posts/{id}                   게시물 삭제
 *   PUT    /api/posts/{id}/departments       대상 학과 다시 체크 {"department_ids":[..]}
 *   GET    /api/notices?status=pending       수집한 학교 공지 목록
 *   POST   /api/notices/crawl                지금 학교 홈페이지에서 공지 수집
 *   POST   /api/notices/{id}/publish         수집한 공지를 게시물로 올리기
 *   POST   /api/notices/{id}/ignore          공지 숨기기 (게시하지 않음)
 *   GET    /api/admin/summary                관리자 화면 상단 통계
 *
 * 공지(notice)의 상태 흐름:  pending(대기) ──publish──▶ published(게시됨)
 *                                         └──ignore───▶ ignored(숨김)
 */
#include "api.h"
#include "db.h"
#include "crawler.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTS 64   /* 한 게시물에 체크할 수 있는 최대 학과 수 */

/* 체크된 학과를 post_departments 에 넣는다. 유효한 학과만 남긴다.
 * 방식: 기존 연결을 모두 지우고 새로 넣는다 (수정 = 전체 교체).
 * INSERT ... SELECT ... FROM departments WHERE id IN (...) 형태로 쓰면
 * departments 에 실제로 있는 id 만 골라 넣게 되어, 존재하지 않는 학과 id 를 보내도
 * 외래 키 오류 없이 조용히 걸러진다. INSERT IGNORE 는 중복 id 가 있어도 오류 없이 무시한다.
 * 완성되는 SQL 예:
 *   INSERT IGNORE INTO post_departments (post_id, department_id)
 *   SELECT 12, d.id FROM departments d WHERE d.id IN (3,5,7) */
static int set_post_departments(unsigned post_id, const unsigned *ids, int n)
{
    Buf sql;
    int i, ok;

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM post_departments WHERE post_id = %u", post_id);
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    if (!ok || n <= 0)   /* 지우기 실패, 또는 빈 목록(= 전체 공개)이면 여기서 끝 */
        return ok;

    buf_init(&sql);
    buf_puts(&sql, "INSERT IGNORE INTO post_departments (post_id, department_id) "
                   "SELECT ");
    db_sqlf(&sql, "%u, d.id FROM departments d WHERE d.id IN (", post_id);
    for (i = 0; i < n; i++) {
        if (i)
            buf_putc(&sql, ',');
        db_sqlf(&sql, "%u", ids[i]);   /* 숫자만 들어가므로 인젝션 위험 없음 */
    }
    buf_putc(&sql, ')');

    ok = db_exec_buf(&sql);
    buf_free(&sql);
    return ok;
}

/* 허용된 kind 만 통과시킨다. (모르는 값은 기본값 contest)
 * 돌려주는 값은 코드 안의 상수 문자열이므로 SQL 에 '%s' 로 그대로 넣어도 안전하다. */
static const char *normalize_kind(const char *kind)
{
    if (kind && strcmp(kind, "hackathon") == 0) return "hackathon";
    if (kind && strcmp(kind, "etc") == 0)       return "etc";
    return "contest";
}

/* 날짜 문자열이 비었으면 NULL 을 넣도록 "YYYY-MM-DD" 형태만 통과시킨다.
 * (자리 모양만 검사한다. 2026-13-45 같은 값은 MySQL 이 거부하거나 경고를 낸다) */
static const char *clean_date(const char *s)
{
    int i;

    if (!s || strlen(s) != 10)
        return NULL;
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {          /* 5번째, 8번째 글자는 하이픈 */
            if (s[i] != '-')
                return NULL;
        } else if (s[i] < '0' || s[i] > '9') {
            return NULL;
        }
    }
    return s;
}

/* -------------------------------------------------------- 게시물 등록/삭제 */

/* POST /api/posts
 * 본문: {title, body, host?, source_url?, kind?, deadline?, need_people?, department_ids?}
 * 게시물 INSERT 와 대상 학과 INSERT 를 트랜잭션으로 묶어, 둘 중 하나만 저장되는 일이 없게 한다. */
static void handle_post_create(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    const char *title, *body, *host, *source_url, *kind;
    const char *deadline;
    unsigned depts[MAX_DEPTS];
    int ndepts;
    int need_people;            /* 모집 인원 (0 = 정하지 않음) */
    Buf sql;
    unsigned long long post_id;

    if (!auth_require_admin(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    /* 선택 항목은 기본값을 NULL 로 받아 DB 에도 NULL 로 저장되게 한다 (%Q 가 NULL 을 처리) */
    title       = json_str(in, "title", "");
    body        = json_str(in, "body", "");
    host        = json_str(in, "host", NULL);
    source_url  = json_str(in, "source_url", NULL);
    kind        = normalize_kind(json_str(in, "kind", "contest"));
    deadline    = clean_date(json_str(in, "deadline", NULL));
    need_people = (int)json_int(in, "need_people", 0);
    ndepts      = json_uint_array(json_get(in, "department_ids"), depts, MAX_DEPTS);

    if (strlen(title) < 2) {
        res_error(res, 400, "bad_title", "제목을 입력하세요.");
        goto out;
    }
    if (strlen(body) < 2) {
        res_error(res, 400, "bad_body", "내용을 입력하세요.");
        goto out;
    }
    /* 대상 학과를 비워 두면 전체 공개 글이 되어 로그인한 누구나 댓글을 쓸 수 있다.
     * 특정 학과로 좁히려면 체크해서 보내면 된다. */
    if (need_people < 0 || need_people > 1000)
        need_people = 0;

    if (!db_begin()) {          /* 트랜잭션 시작 (연결 잠금도 함께) */
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }

    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO posts (author_id, kind, title, body, source_url, host, "
            "                   deadline, need_people) "
            "VALUES (%u, '%s', %Q, %Q, %Q, %Q, %Q, %d)",
            u.id, kind, title, body, source_url, host, deadline, need_people);

    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();          /* 되돌리고 잠금 해제 */
        res_error(res, 500, "db_error", "게시물을 저장할 수 없습니다.");
        goto out;
    }
    buf_free(&sql);

    /* 트랜잭션이 연결을 잠그고 있으므로 다른 스레드의 INSERT 가 끼어들 수 없어
     * db_last_id 가 정확히 방금 만든 게시물 id 를 돌려준다. */
    post_id = db_last_id();
    if (!set_post_departments((unsigned)post_id, depts, ndepts)) {
        db_rollback();
        res_error(res, 500, "db_error", "대상 학과를 저장할 수 없습니다.");
        goto out;
    }

    if (!db_commit()) {
        res_error(res, 500, "db_error", "저장을 확정할 수 없습니다.");
        goto out;
    }

    ai_start_background(0);         /* 새 글을 AI 로 분석 (키가 없으면 아무 일도 안 한다) */

    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "id", (long long)post_id);
        buf_putc(&b, '}');
        res_json_buf(res, 201, &b);
        buf_free(&b);
    }

out:
    json_free(in);
}

/* DELETE /api/posts/{id}
 * 댓글·대상 학과·AI 분석 등 딸린 행은 스키마의 ON DELETE CASCADE 로 함께 지워진다. */
static void handle_post_delete(Request *req, Response *res, unsigned post_id)
{
    CurrentUser u;
    Buf sql;

    if (!auth_require_admin(req, res, &u))
        return;

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM posts WHERE id = %u", post_id);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "게시물을 삭제할 수 없습니다.");
        return;
    }
    buf_free(&sql);

    if (db_affected() == 0) {   /* 지운 행이 없으면 원래 없던 게시물 */
        res_error(res, 404, "not_found", "게시물이 없습니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

/* 이미 등록한 게시물의 대상 학과만 다시 체크한다.
 * PUT /api/posts/{id}/departments {"department_ids":[3,5]} */
static void handle_post_departments_update(Request *req, Response *res, unsigned post_id)
{
    CurrentUser u;
    Json *in;
    unsigned depts[MAX_DEPTS];
    int ndepts;
    Buf sql;
    long long exists;

    if (!auth_require_admin(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM posts WHERE id = %u", post_id);
    exists = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (!exists) {
        res_error(res, 404, "not_found", "게시물이 없습니다.");
        goto out;
    }

    /* 빈 배열을 보내면 제한이 풀려 전체 공개 글이 된다. */
    ndepts = json_uint_array(json_get(in, "department_ids"), depts, MAX_DEPTS);
    if (!set_post_departments(post_id, depts, ndepts)) {
        res_error(res, 500, "db_error", "대상 학과를 저장할 수 없습니다.");
        goto out;
    }
    res_json(res, 200, "{\"ok\":true}");

out:
    json_free(in);
}

/* ------------------------------------------------------------ 공지 수집 */

/* GET /api/notices?status=pending|published|ignored (없으면 전체) */
static void handle_notice_list(Request *req, Response *res)
{
    CurrentUser u;
    char status[20];
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    if (!auth_require_admin(req, res, &u))
        return;

    req_query(req, "status", status, sizeof status);

    /* buf_puts 로 넣으므로 날짜 형식의 % 를 그대로 쓴다 (db_sqlf 였다면 %% 로 써야 함) */
    buf_init(&sql);
    buf_puts(&sql,
             "SELECT n.id, n.ext_id, n.keyword, n.title, n.url, "
             "       DATE_FORMAT(n.published_at, '%Y-%m-%d'), n.status, "
             "       IFNULL(n.post_id, 0), "
             "       DATE_FORMAT(n.fetched_at, '%Y-%m-%d %H:%i') "
             "FROM notices n WHERE 1 = 1");

    /* 상태 필터는 허용된 세 값만 받는다 */
    if (strcmp(status, "pending") == 0 || strcmp(status, "published") == 0 ||
        strcmp(status, "ignored") == 0)
        db_sqlf(&sql, " AND n.status = %Q", status);

    buf_puts(&sql, " ORDER BY n.published_at DESC, n.id DESC LIMIT 300");

    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "공지 목록을 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"notices\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (!first)
            buf_putc(&b, ',');
        first = 0;
        buf_putc(&b, '{');
        json_kv_int(&b, "id", atoll(row[0]));           buf_putc(&b, ',');
        json_kv_int(&b, "ext_id", atoll(row[1]));       buf_putc(&b, ',');   /* 학교 홈페이지의 공지 번호 */
        json_kv_str(&b, "keyword", row[2]);             buf_putc(&b, ',');   /* 어떤 검색어로 찾았는지 */
        json_kv_str(&b, "title", row[3]);               buf_putc(&b, ',');
        json_kv_str(&b, "url", row[4]);                 buf_putc(&b, ',');
        json_kv_str(&b, "published_at", row[5]);        buf_putc(&b, ',');
        json_kv_str(&b, "status", row[6]);              buf_putc(&b, ',');
        json_kv_int(&b, "post_id", atoll(row[7]));      buf_putc(&b, ',');   /* 게시된 경우 연결된 게시물 */
        json_kv_str(&b, "fetched_at", row[8]);
        buf_putc(&b, '}');
    }
    buf_puts(&b, "]}");

    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* POST /api/notices/crawl - 요청 스레드에서 바로 수집한다(수 초~수십 초 걸릴 수 있음). */
static void handle_notice_crawl(Request *req, Response *res)
{
    CurrentUser u;
    CrawlResult cr;
    int window;
    Buf b;

    if (!auth_require_admin(req, res, &u))
        return;

    /* 수집 기간(일). 설정이 없으면 730일 */
    window = (int)db_scalar(
        "SELECT CAST(v AS SIGNED) FROM app_config WHERE k = 'crawl.window_days'", 730);

    if (!crawl_notices(window, &cr)) {
        /* 502 Bad Gateway: 우리 서버가 아니라 바깥 서버(학교 홈페이지) 쪽 문제라는 뜻 */
        res_error(res, 502, "crawl_failed",
                  cr.error[0] ? cr.error : "학교 홈페이지에서 공지를 가져오지 못했습니다.");
        return;
    }

    if (cr.published > 0)
        ai_start_background(0);     /* 새로 게시된 공지를 AI 로 분석 */

    buf_init(&b);
    buf_puts(&b, "{\"ok\":true,");
    json_kv_int(&b, "fetched", cr.fetched);     buf_putc(&b, ',');
    json_kv_int(&b, "inserted", cr.inserted);   buf_putc(&b, ',');
    json_kv_int(&b, "skipped", cr.skipped);     buf_putc(&b, ',');
    json_kv_int(&b, "published", cr.published); buf_putc(&b, ',');
    json_kv_int(&b, "window_days", window);
    buf_putc(&b, '}');
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* 수집한 공지를 공모전 게시물로 올린다. 대상 학과 체크가 필수다.
 * (참고: 현재 코드는 department_ids 가 비어 있어도 거절하지 않고 전체 공개 글로 저장한다.)
 * POST /api/notices/{id}/publish {department_ids?, kind?, deadline?, need_people?, body?, host?}
 * 처리: 공지 읽기 → 본문 만들기(관리자 글 + 원문 텍스트) → 트랜잭션으로
 *       [게시물 INSERT, 대상 학과 INSERT, 공지 상태 UPDATE] → AI 분석 시작 */
static void handle_notice_publish(Request *req, Response *res, unsigned notice_id)
{
    CurrentUser u;
    Json *in;
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char title[400] = "", url[600] = "", published[16] = "", keyword[48] = "";
    char status[20] = "";
    long ext_id = 0;            /* 학교 홈페이지 쪽 공지 번호 (원문을 받아올 때 사용) */
    unsigned depts[MAX_DEPTS];
    int ndepts, need_people;
    const char *kind, *deadline, *extra_body, *host;
    Buf body;
    unsigned long long post_id;

    if (!auth_require_admin(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    /* 1) 공지 정보 읽기 */
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT title, url, DATE_FORMAT(published_at, '%%Y-%%m-%%d'), keyword, status, ext_id "
            "FROM notices WHERE id = %u", notice_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    if (qr) {
        row = mysql_fetch_row(qr);
        if (row) {
            str_copy(title, sizeof title, row[0]);
            str_copy(url, sizeof url, row[1]);
            str_copy(published, sizeof published, row[2]);
            str_copy(keyword, sizeof keyword, row[3]);
            str_copy(status, sizeof status, row[4]);
            ext_id = row[5] ? strtol(row[5], NULL, 10) : 0;
        }
        mysql_free_result(qr);
    }
    if (!title[0]) {
        res_error(res, 404, "not_found", "공지를 찾을 수 없습니다.");
        goto out;
    }
    if (strcmp(status, "published") == 0) {   /* 같은 공지를 두 번 게시하지 않는다 */
        res_error(res, 409, "already_published", "이미 게시물로 등록한 공지입니다.");
        goto out;
    }

    /* 2) 관리자가 보낸 옵션 읽기 */
    ndepts = json_uint_array(json_get(in, "department_ids"), depts, MAX_DEPTS);

    /* kind 를 안 보냈으면 검색어가 "해커톤" 이었던 공지는 hackathon, 나머지는 contest */
    kind        = normalize_kind(json_str(in, "kind",
                      strcmp(keyword, "해커톤") == 0 ? "hackathon" : "contest"));
    deadline    = clean_date(json_str(in, "deadline", NULL));
    need_people = (int)json_int(in, "need_people", 0);
    extra_body  = json_str(in, "body", NULL);
    host        = json_str(in, "host", NULL);
    if (need_people < 0 || need_people > 1000)
        need_people = 0;

    /* 3) 본문은 관리자가 적어 보낸 글 뒤에 공지 원문을 텍스트로 붙인다.
     * 원문 링크는 source_url 에 따로 두고, 원문을 못 받으면 출처만 적는다.
     * (원문은 학교 홈페이지에 다시 요청해 받으므로 트랜잭션 시작 전에 미리 해 둔다.
     *  네트워크를 기다리는 동안 DB 연결을 잠가 두면 다른 요청이 모두 멈추기 때문이다.) */
    buf_init(&body);
    if (extra_body && extra_body[0]) {
        buf_puts(&body, extra_body);
        buf_puts(&body, "\n\n");
    }
    if (!notice_body_text(ext_id, &body))
        buf_printf(&body, "학교 공지 (%s, %s 게시)", keyword, published);

    /* 4) 트랜잭션 */
    if (!db_begin()) {
        buf_free(&body);
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }

    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO posts (author_id, kind, title, body, source_url, host, "
            "                   deadline, need_people) "
            "VALUES (%u, '%s', %Q, %Q, %Q, %Q, %Q, %d)",
            u.id, kind, title, body.data, url, host, deadline, need_people);
    buf_free(&body);   /* SQL 에 이스케이프되어 복사됐으므로 원본은 더 필요 없다 */

    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "게시물을 저장할 수 없습니다.");
        goto out;
    }
    buf_free(&sql);

    post_id = db_last_id();
    if (!set_post_departments((unsigned)post_id, depts, ndepts)) {
        db_rollback();
        res_error(res, 500, "db_error", "대상 학과를 저장할 수 없습니다.");
        goto out;
    }

    /* 공지와 게시물을 연결하고 상태를 published 로 */
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE notices SET status = 'published', post_id = %L WHERE id = %u",
            (long long)post_id, notice_id);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "공지 상태를 갱신할 수 없습니다.");
        goto out;
    }
    buf_free(&sql);

    if (!db_commit()) {
        res_error(res, 500, "db_error", "저장을 확정할 수 없습니다.");
        goto out;
    }

    ai_start_background(0);   /* 새 게시물 AI 분석 */

    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "post_id", (long long)post_id);
        buf_putc(&b, '}');
        res_json_buf(res, 201, &b);
        buf_free(&b);
    }

out:
    json_free(in);
}

/* POST /api/notices/{id}/ignore - 대기(pending) 중인 공지만 숨긴다.
 * 이미 게시된 공지는 WHERE 조건에서 걸러져 바뀌지 않는다. */
static void handle_notice_ignore(Request *req, Response *res, unsigned notice_id)
{
    CurrentUser u;
    Buf sql;

    if (!auth_require_admin(req, res, &u))
        return;

    buf_init(&sql);
    db_sqlf(&sql, "UPDATE notices SET status = 'ignored' "
                  "WHERE id = %u AND status = 'pending'", notice_id);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "공지를 숨길 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 200, "{\"ok\":true}");
}

/* 관리자 화면 상단에 보여줄 요약
 * GET /api/admin/summary → {post_count, comment_count, user_count, notice_pending, crawl_fetched_at} */
static void handle_summary(Request *req, Response *res)
{
    CurrentUser u;
    Buf b, sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char fetched_at[40] = "";   /* 마지막 수집 시각 (app_config 에 문자열로 저장) */

    if (!auth_require_admin(req, res, &u))
        return;

    buf_init(&sql);
    buf_puts(&sql, "SELECT v FROM app_config WHERE k = 'crawl.fetched_at'");
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        row = mysql_fetch_row(qr);
        if (row && row[0])
            str_copy(fetched_at, sizeof fetched_at, row[0]);
        mysql_free_result(qr);
    }

    buf_init(&b);
    buf_putc(&b, '{');
    json_kv_int(&b, "post_count", db_scalar("SELECT COUNT(*) FROM posts", 0));
    buf_putc(&b, ',');
    json_kv_int(&b, "comment_count", db_scalar("SELECT COUNT(*) FROM comments", 0));
    buf_putc(&b, ',');
    json_kv_int(&b, "user_count",
                db_scalar("SELECT COUNT(*) FROM users WHERE role = 'student'", 0));   /* 관리자 제외 */
    buf_putc(&b, ',');
    json_kv_int(&b, "notice_pending",
                db_scalar("SELECT COUNT(*) FROM notices WHERE status = 'pending'", 0));
    buf_putc(&b, ',');
    json_kv_str(&b, "crawl_fetched_at", fetched_at[0] ? fetched_at : NULL);
    buf_putc(&b, '}');

    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* 관리자 경로 라우터.
 * 주의: GET /api/posts/{id} 는 api_posts.c 가 먼저 처리하므로, 여기서는 같은 경로라도
 * POST/DELETE/PUT 메서드만 맡는다. (디스패처가 route_posts 를 route_admin 보다 먼저 부름) */
int route_admin(Request *req, Response *res)
{
    unsigned id;

    if (req_is(req, "POST", "/api/posts")) {
        handle_post_create(req, res);
        return 1;
    }
    if (strcmp(req->method, "DELETE") == 0 &&
        path_id(req->path, "/api/posts/", NULL, &id)) {
        handle_post_delete(req, res, id);
        return 1;
    }
    if (strcmp(req->method, "PUT") == 0 &&
        path_id(req->path, "/api/posts/", "/departments", &id)) {
        handle_post_departments_update(req, res, id);
        return 1;
    }
    if (req_is(req, "GET", "/api/notices")) {
        handle_notice_list(req, res);
        return 1;
    }
    if (req_is(req, "POST", "/api/notices/crawl")) {
        handle_notice_crawl(req, res);
        return 1;
    }
    if (strcmp(req->method, "POST") == 0 &&
        path_id(req->path, "/api/notices/", "/publish", &id)) {
        handle_notice_publish(req, res, id);
        return 1;
    }
    if (strcmp(req->method, "POST") == 0 &&
        path_id(req->path, "/api/notices/", "/ignore", &id)) {
        handle_notice_ignore(req, res, id);
        return 1;
    }
    if (req_is(req, "GET", "/api/admin/summary")) {
        handle_summary(req, res);
        return 1;
    }
    return 0;
}
