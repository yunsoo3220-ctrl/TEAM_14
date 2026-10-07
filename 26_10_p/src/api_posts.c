/* 공모전 게시물 조회와 댓글.
 *
 * 댓글 권한의 핵심 규칙: 관리자가 게시물에 체크한 학과(post_departments)에
 * 소속된 사용자만 댓글을 쓸 수 있다. 관리자는 항상 쓸 수 있다. */
#include "api.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMENT_MAX_LEN 1000

/* 게시물에 체크된 학과 수. 0 이면 대상 학과를 지정하지 않은 글이다. */
static int post_department_count(unsigned post_id)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM post_departments WHERE post_id = %u", post_id);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return (int)n;
}

/* 게시물에 체크된 학과에 해당 학과가 포함되는지 */
static int department_allowed(unsigned post_id, unsigned department_id)
{
    Buf sql;
    long long n;

    if (!department_id)
        return 0;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT COUNT(*) FROM post_departments "
            "WHERE post_id = %u AND department_id = %u", post_id, department_id);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

/* 댓글 가능 여부와 사유를 함께 돌려준다. reason 은 정적 문자열.
 *
 * 규칙:
 *   - 비로그인      -> 불가 (글은 볼 수 있지만 댓글은 못 쓴다)
 *   - 관리자        -> 항상 가능
 *   - 대상 학과 미지정 -> 로그인한 누구나 가능
 *   - 대상 학과 지정됨 -> 체크된 학과 소속만 가능  <- 과제의 핵심 제약 */
static int can_comment(const CurrentUser *u, unsigned post_id, const char **reason)
{
    if (!u->id) {
        *reason = "로그인 후 댓글을 쓸 수 있습니다.";
        return 0;
    }
    if (u->is_admin) {
        *reason = "관리자";
        return 1;
    }
    if (post_department_count(post_id) == 0) {
        *reason = "대상 학과가 지정되지 않아 누구나 댓글을 쓸 수 있습니다.";
        return 1;
    }
    if (department_allowed(post_id, u->department_id)) {
        *reason = "대상 학과입니다.";
        return 1;
    }
    *reason = "이 공모전의 대상 학과가 아니어서 댓글을 쓸 수 없습니다.";
    return 0;
}

/* ------------------------------------------------------------ 게시물 목록 */

static void handle_post_list(Request *req, Response *res)
{
    CurrentUser u;
    char dept[16], kind[20], only_mine[8];
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    auth_current(req, &u);

    req_query(req, "department_id", dept, sizeof dept);
    req_query(req, "kind", kind, sizeof kind);
    req_query(req, "mine", only_mine, sizeof only_mine);

    buf_init(&sql);
    buf_puts(&sql,
             "SELECT p.id, p.kind, p.title, IFNULL(p.host, ''), "
             "       IFNULL(DATE_FORMAT(p.deadline, '%Y-%m-%d'), ''), p.need_people, "
             "       p.view_count, DATE_FORMAT(p.created_at, '%Y-%m-%d %H:%i'), "
             "       IFNULL(p.source_url, ''), "
             /* 작성자가 없는 글은 학교 공지에서 자동으로 올라온 글이다. */
             "       IFNULL(u.nickname, IF(p.source_url IS NULL, '(탈퇴)', '학교 공지')), "
             "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id), "
             "       (SELECT GROUP_CONCAT(d.name ORDER BY d.sort SEPARATOR ', ') "
             "          FROM post_departments pd JOIN departments d ON d.id = pd.department_id "
             "         WHERE pd.post_id = p.id) "
             "FROM posts p LEFT JOIN users u ON u.id = p.author_id "
             "WHERE 1 = 1");

    /* 특정 학과 대상 게시물만 */
    if (dept[0] && strtoul(dept, NULL, 10) > 0)
        db_sqlf(&sql, " AND EXISTS (SELECT 1 FROM post_departments pd "
                      "WHERE pd.post_id = p.id AND pd.department_id = %u)",
                (unsigned)strtoul(dept, NULL, 10));

    /* 내 학과가 댓글을 쓸 수 있는 게시물만 */
    if (only_mine[0] == '1' && u.department_id)
        db_sqlf(&sql, " AND EXISTS (SELECT 1 FROM post_departments pd "
                      "WHERE pd.post_id = p.id AND pd.department_id = %u)",
                u.department_id);

    if (strcmp(kind, "contest") == 0 || strcmp(kind, "hackathon") == 0 ||
        strcmp(kind, "etc") == 0)
        db_sqlf(&sql, " AND p.kind = %Q", kind);

    buf_puts(&sql, " ORDER BY p.created_at DESC, p.id DESC LIMIT 200");

    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "게시물을 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"posts\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        unsigned pid = (unsigned)strtoul(row[0], NULL, 10);
        const char *why;

        if (!first)
            buf_putc(&b, ',');
        first = 0;

        buf_putc(&b, '{');
        json_kv_int(&b, "id", pid);                                   buf_putc(&b, ',');
        json_kv_str(&b, "kind", row[1]);                              buf_putc(&b, ',');
        json_kv_str(&b, "title", row[2]);                             buf_putc(&b, ',');
        json_kv_str(&b, "host", row[3][0] ? row[3] : NULL);           buf_putc(&b, ',');
        json_kv_str(&b, "deadline", row[4][0] ? row[4] : NULL);       buf_putc(&b, ',');
        json_kv_int(&b, "need_people", atoll(row[5]));                buf_putc(&b, ',');
        json_kv_int(&b, "view_count", atoll(row[6]));                 buf_putc(&b, ',');
        json_kv_str(&b, "created_at", row[7]);                        buf_putc(&b, ',');
        json_kv_str(&b, "source_url", row[8][0] ? row[8] : NULL);     buf_putc(&b, ',');
        json_kv_str(&b, "author_nickname", row[9]);                   buf_putc(&b, ',');
        json_kv_int(&b, "comment_count", atoll(row[10]));             buf_putc(&b, ',');
        json_kv_str(&b, "departments", row[11] ? row[11] : "");       buf_putc(&b, ',');
        /* 목록에서도 내가 댓글을 쓸 수 있는 글인지 바로 보여준다. */
        json_kv_bool(&b, "can_comment", can_comment(&u, pid, &why));
        buf_putc(&b, '}');
    }
    buf_puts(&b, "]}");

    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 게시물 상세 */

static void write_post_departments(Buf *b, unsigned post_id)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT d.id, d.name, c.name FROM post_departments pd "
            "JOIN departments d ON d.id = pd.department_id "
            "JOIN colleges c ON c.id = d.college_id "
            "WHERE pd.post_id = %u ORDER BY c.sort, d.sort", post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    buf_putc(b, '[');
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(b, ',');
            first = 0;
            buf_putc(b, '{');
            json_kv_int(b, "id", atoll(row[0]));  buf_putc(b, ',');
            json_kv_str(b, "name", row[1]);       buf_putc(b, ',');
            json_kv_str(b, "college_name", row[2]);
            buf_putc(b, '}');
        }
        mysql_free_result(qr);
    }
    buf_putc(b, ']');
}

static void write_comments(Buf *b, unsigned post_id)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT c.id, c.body, DATE_FORMAT(c.created_at, '%%Y-%%m-%%d %%H:%%i'), "
            "       IFNULL(u.nickname, '(탈퇴)'), IFNULL(d.name, ''), IFNULL(c.author_id, 0) "
            "FROM comments c "
            "LEFT JOIN users u ON u.id = c.author_id "
            "LEFT JOIN departments d ON d.id = u.department_id "
            "WHERE c.post_id = %u ORDER BY c.created_at, c.id", post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    buf_putc(b, '[');
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(b, ',');
            first = 0;
            buf_putc(b, '{');
            json_kv_int(b, "id", atoll(row[0]));                       buf_putc(b, ',');
            json_kv_str(b, "body", row[1]);                            buf_putc(b, ',');
            json_kv_str(b, "created_at", row[2]);                      buf_putc(b, ',');
            json_kv_str(b, "author_nickname", row[3]);                 buf_putc(b, ',');
            json_kv_str(b, "author_department", row[4][0] ? row[4] : NULL); buf_putc(b, ',');
            json_kv_int(b, "author_id", atoll(row[5]));
            buf_putc(b, '}');
        }
        mysql_free_result(qr);
    }
    buf_putc(b, ']');
}

static void handle_post_detail(Request *req, Response *res, unsigned post_id)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    const char *reason;
    int allowed;

    auth_current(req, &u);

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT p.id, p.kind, p.title, p.body, IFNULL(p.host, ''), "
            "       IFNULL(DATE_FORMAT(p.deadline, '%%Y-%%m-%%d'), ''), p.need_people, "
            "       p.view_count, DATE_FORMAT(p.created_at, '%%Y-%%m-%%d %%H:%%i'), "
            "       IFNULL(p.source_url, ''), "
            "       IFNULL(u.nickname, IF(p.source_url IS NULL, '(탈퇴)', '학교 공지')) "
            "FROM posts p LEFT JOIN users u ON u.id = p.author_id "
            "WHERE p.id = %u", post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    if (!qr) {
        res_error(res, 500, "db_error", "게시물을 읽을 수 없습니다.");
        return;
    }
    row = mysql_fetch_row(qr);
    if (!row) {
        mysql_free_result(qr);
        res_error(res, 404, "not_found", "게시물이 없습니다.");
        return;
    }

    /* 조회수 증가 (실패해도 조회 자체는 계속한다) */
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE posts SET view_count = view_count + 1 WHERE id = %u", post_id);
    db_exec_buf(&sql);
    buf_free(&sql);

    allowed = can_comment(&u, post_id, &reason);

    buf_init(&b);
    buf_puts(&b, "{\"post\":{");
    json_kv_int(&b, "id", atoll(row[0]));                            buf_putc(&b, ',');
    json_kv_str(&b, "kind", row[1]);                                 buf_putc(&b, ',');
    json_kv_str(&b, "title", row[2]);                                buf_putc(&b, ',');
    json_kv_str(&b, "body", row[3]);                                 buf_putc(&b, ',');
    json_kv_str(&b, "host", row[4][0] ? row[4] : NULL);              buf_putc(&b, ',');
    json_kv_str(&b, "deadline", row[5][0] ? row[5] : NULL);          buf_putc(&b, ',');
    json_kv_int(&b, "need_people", atoll(row[6]));                   buf_putc(&b, ',');
    json_kv_int(&b, "view_count", atoll(row[7]) + 1);                buf_putc(&b, ',');
    json_kv_str(&b, "created_at", row[8]);                           buf_putc(&b, ',');
    json_kv_str(&b, "source_url", row[9][0] ? row[9] : NULL);        buf_putc(&b, ',');
    json_kv_str(&b, "author_nickname", row[10]);                     buf_putc(&b, ',');
    buf_puts(&b, "\"departments\":");
    write_post_departments(&b, post_id);
    buf_puts(&b, "},\"comments\":");
    write_comments(&b, post_id);
    buf_puts(&b, ",\"permission\":{");
    json_kv_bool(&b, "can_comment", allowed);
    buf_putc(&b, ',');
    json_kv_str(&b, "reason", reason);
    buf_puts(&b, "}}");

    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* -------------------------------------------------------------- 댓글 작성 */

static void handle_comment_create(Request *req, Response *res, unsigned post_id)
{
    CurrentUser u;
    Json *in;
    const char *body;
    const char *reason;
    Buf sql;
    long long exists;

    if (!auth_require(req, res, &u))
        return;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM posts WHERE id = %u", post_id);
    exists = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (!exists) {
        res_error(res, 404, "not_found", "게시물이 없습니다.");
        return;
    }

    /* 여기가 과제의 핵심 제약이다: 체크된 학과가 아니면 거부한다. */
    if (!can_comment(&u, post_id, &reason)) {
        res_error(res, 403, "department_not_allowed", reason);
        return;
    }

    in = body_object(req, res);
    if (!in)
        return;

    body = json_str(in, "body", "");
    {
        char *trimmed = str_dup(body);
        if (!trimmed) {
            json_free(in);
            res_error(res, 500, "oom", "메모리가 부족합니다.");
            return;
        }
        str_trim(trimmed);

        if (strlen(trimmed) == 0) {
            free(trimmed);
            json_free(in);
            res_error(res, 400, "empty_comment", "댓글 내용을 입력하세요.");
            return;
        }
        if (strlen(trimmed) > COMMENT_MAX_LEN) {
            free(trimmed);
            json_free(in);
            res_error(res, 400, "too_long", "댓글은 1000자까지 쓸 수 있습니다.");
            return;
        }

        buf_init(&sql);
        db_sqlf(&sql,
                "INSERT INTO comments (post_id, author_id, body) VALUES (%u, %u, %Q)",
                post_id, u.id, trimmed);
        if (!db_exec_buf(&sql)) {
            buf_free(&sql);
            free(trimmed);
            json_free(in);
            res_error(res, 500, "db_error", "댓글을 저장할 수 없습니다.");
            return;
        }
        buf_free(&sql);
        free(trimmed);
    }

    json_free(in);
    res_json(res, 201, "{\"ok\":true}");
}

/* 작성자 본인 또는 관리자만 삭제할 수 있다. */
static void handle_comment_delete(Request *req, Response *res, unsigned comment_id)
{
    CurrentUser u;
    Buf sql;

    if (!auth_require(req, res, &u))
        return;

    buf_init(&sql);
    if (u.is_admin)
        db_sqlf(&sql, "DELETE FROM comments WHERE id = %u", comment_id);
    else
        db_sqlf(&sql, "DELETE FROM comments WHERE id = %u AND author_id = %u",
                comment_id, u.id);

    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "댓글을 삭제할 수 없습니다.");
        return;
    }
    buf_free(&sql);

    if (db_affected() == 0) {
        res_error(res, 403, "not_allowed", "본인 댓글만 삭제할 수 있습니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

int route_posts(Request *req, Response *res)
{
    unsigned id;

    if (req_is(req, "GET", "/api/posts")) {
        handle_post_list(req, res);
        return 1;
    }
    if (strcmp(req->method, "GET") == 0 && path_id(req->path, "/api/posts/", NULL, &id)) {
        handle_post_detail(req, res, id);
        return 1;
    }
    if (strcmp(req->method, "POST") == 0 &&
        path_id(req->path, "/api/posts/", "/comments", &id)) {
        handle_comment_create(req, res, id);
        return 1;
    }
    if (strcmp(req->method, "DELETE") == 0 &&
        path_id(req->path, "/api/comments/", NULL, &id)) {
        handle_comment_delete(req, res, id);
        return 1;
    }
    return 0;
}
