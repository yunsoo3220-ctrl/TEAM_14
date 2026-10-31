/* 관리자: 회원 관리 · 알림 보내기 · 전체 게시물 (모두 관리자 전용)
 *
 *   GET    /api/admin/users?q=         가입한 모든 회원 (학번·이름·표시 이름·학과·이메일로 찾기)
 *   POST   /api/admin/users            회원 추가 (api.c - 비밀번호 해시 등 가입 도우미가 거기 있다)
 *   DELETE /api/admin/users/{id}        회원 삭제 (관리자 계정은 지울 수 없다)
 *   POST   /api/admin/users/{id}/suspend {days, reason}  회원 정지 (1~365일, 알림함으로 통보)
 *   DELETE /api/admin/users/{id}/suspend 정지 바로 풀기 (알림함으로 통보)
 *   POST   /api/admin/notify            회원 알림함으로 메시지 {user_id, title, body?, link?}
 *   GET    /api/admin/content?type=&q=  회원이 쓴 모든 글 (팀원 모집·그룹 스터디·전공책)과 주인
 *
 * 관리자에게는 실명·학번·이메일이 보인다 (회원 관리에 필요). 학생 화면에는 여전히 표시 이름·학과만 보인다.
 * 정지한 회원은 보기만 할 수 있고 쓰기 요청은 api.c 의 입구(suspended_blocked)에서 막힌다.
 * 정지 기간이 지나면 따로 풀지 않아도 시각 비교로 저절로 풀린다.
 * 회원을 지우면 그 회원의 모집글·스터디·전공책·댓글·알림 등은 외래 키(ON DELETE CASCADE)로 함께 지워지고,
 * 공모전 게시물·공모전 댓글은 작성자만 비운 채(SET NULL) 남는다.
 */
#include "api.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_LIST_MAX    500   /* 회원 목록 최대 수 */
#define CONTENT_LIST_MAX 300   /* 글 종류마다 최대 수 */

/* q 를 LIKE '%q%' 패턴으로 (% _ \ 는 글자 그대로). out 은 호출자가 buf_free. */
static void like_pattern(Buf *out, const char *q)
{
    const char *p;

    buf_init(out);
    buf_putc(out, '%');
    for (p = q; *p; p++) {
        if (*p == '%' || *p == '_' || *p == '\\')
            buf_putc(out, '\\');
        buf_putc(out, *p);
    }
    buf_putc(out, '%');
}

/* ------------------------------------------------------------ 회원 */

/* GET /api/admin/users?q=
 * 응답: {"users":[{id,student_no,name,nickname,department,email,role,created_at,
 *                  recruits,studies,books,suspended_until,suspend_reason}, ...]} (관리자 먼저, 그다음 최근 가입순)
 *   suspended_until: 지금 정지 중이면 끝나는 시각 "YYYY-MM-DD HH:MM", 아니면 null */
static void handle_users(Request *req, Response *res)
{
    CurrentUser u;
    char q[128] = "";
    Buf sql, pat, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    if (!auth_require_admin(req, res, &u))
        return;
    req_query(req, "q", q, sizeof q);
    str_trim(q);

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT u.id, u.student_no, u.name, u.nickname, IFNULL(d.name, ''), IFNULL(u.email, ''), u.role, "
            "       DATE_FORMAT(u.created_at, '%%Y-%%m-%%d'), "
            "       (SELECT COUNT(*) FROM recruits r WHERE r.author_id = u.id), "
            "       (SELECT COUNT(*) FROM study_groups g WHERE g.owner_id = u.id), "
            "       (SELECT COUNT(*) FROM books k WHERE k.seller_id = u.id), "
            "       IF(u.suspended_until > NOW(), DATE_FORMAT(u.suspended_until, '%%Y-%%m-%%d %%H:%%i'), ''), "
            "       u.suspend_reason "
            "FROM users u LEFT JOIN departments d ON d.id = u.department_id ");
    if (q[0]) {
        like_pattern(&pat, q);
        db_sqlf(&sql, "WHERE u.student_no LIKE %Q OR u.name LIKE %Q OR u.nickname LIKE %Q "
                      "   OR d.name LIKE %Q OR u.email LIKE %Q ",
                pat.data, pat.data, pat.data, pat.data, pat.data);
        buf_free(&pat);
    }
    db_sqlf(&sql, "ORDER BY u.role = 'admin' DESC, u.created_at DESC, u.id DESC LIMIT %d", USER_LIST_MAX);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "회원 목록을 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"users\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (!first)
            buf_putc(&b, ',');
        first = 0;
        buf_putc(&b, '{');
        json_kv_int(&b, "id", atoll(row[0]));                     buf_putc(&b, ',');
        json_kv_str(&b, "student_no", row[1]);                    buf_putc(&b, ',');
        json_kv_str(&b, "name", row[2]);                          buf_putc(&b, ',');
        json_kv_str(&b, "nickname", row[3]);                      buf_putc(&b, ',');
        json_kv_str(&b, "department", row[4][0] ? row[4] : NULL); buf_putc(&b, ',');
        json_kv_str(&b, "email", row[5][0] ? row[5] : NULL);      buf_putc(&b, ',');
        json_kv_str(&b, "role", row[6]);                          buf_putc(&b, ',');
        json_kv_str(&b, "created_at", row[7]);                    buf_putc(&b, ',');
        json_kv_int(&b, "recruits", atoll(row[8]));               buf_putc(&b, ',');
        json_kv_int(&b, "studies", atoll(row[9]));                buf_putc(&b, ',');
        json_kv_int(&b, "books", atoll(row[10]));                 buf_putc(&b, ',');
        json_kv_str(&b, "suspended_until", row[11][0] ? row[11] : NULL); buf_putc(&b, ',');
        json_kv_str(&b, "suspend_reason", row[11][0] ? row[12] : NULL);
        buf_putc(&b, '}');
    }
    mysql_free_result(qr);
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* DELETE /api/admin/users/{id} - 학생 계정만. 관리자 계정(자기 자신 포함)은 지우지 않는다. */
static void handle_user_delete(Request *req, Response *res, unsigned uid)
{
    CurrentUser u;
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char role[12] = "", student_no[24] = "";
    int ok;

    if (!auth_require_admin(req, res, &u))
        return;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT role, student_no FROM users WHERE id = %u", uid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (row) {
        str_copy(role, sizeof role, row[0]);
        str_copy(student_no, sizeof student_no, row[1]);
    }
    if (qr)
        mysql_free_result(qr);
    if (!role[0]) {
        res_error(res, 404, "not_found", "회원이 없습니다.");
        return;
    }
    if (strcmp(role, "student") != 0) {
        res_error(res, 400, "is_admin", "관리자 계정은 지울 수 없습니다.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM users WHERE id = %u AND role = 'student'", uid);
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    if (!ok) {
        res_error(res, 500, "db_error", "회원을 지울 수 없습니다.");
        return;
    }
    log_info("관리자 %s 가 회원 %s 를 지웠습니다.", u.student_no, student_no);
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 정지 */

/* 학생 회원인지 확인. 학생이면 1, 아니면 응답을 채우고 0. */
static int require_student(Response *res, unsigned uid)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM users WHERE id = %u AND role = 'student'", uid);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (!n)
        res_error(res, 404, "not_found", "정지할 수 있는 학생 회원이 없습니다.");
    return n > 0;
}

/* POST /api/admin/users/{id}/suspend {days, reason}
 * 지금부터 days 일 동안 정지한다 (이미 정지 중이어도 지금부터 다시 센다). 회원 알림함으로 기간·사유를 알린다. */
static void handle_suspend(Request *req, Response *res, unsigned uid)
{
    CurrentUser u;
    Json *in;
    long days;
    char *reason = NULL;
    char until[24] = "";
    Buf sql, title, body;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int ok;

    if (!auth_require_admin(req, res, &u))
        return;
    if (!require_student(res, uid))
        return;
    in = body_object(req, res);
    if (!in)
        return;
    days = json_int(in, "days", 0);
    reason = str_dup(json_str(in, "reason", ""));
    json_free(in);
    if (!reason) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        return;
    }
    str_trim(reason);
    if (days < 1 || days > 365) {
        res_error(res, 400, "bad_days", "정지 기간은 1~365일로 정하세요.");
        goto out;
    }
    if (!reason[0] || strlen(reason) > 900) {
        res_error(res, 400, "bad_reason", "정지 사유를 1~300자로 써 주세요.");
        goto out;
    }

    buf_init(&sql);
    db_sqlf(&sql, "UPDATE users SET suspended_until = NOW() + INTERVAL %d DAY, suspend_reason = LEFT(%Q, 300) "
                  "WHERE id = %u AND role = 'student'", (int)days, reason, uid);
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    if (!ok) {
        res_error(res, 500, "db_error", "회원을 정지할 수 없습니다.");
        goto out;
    }
    buf_init(&sql);
    db_sqlf(&sql, "SELECT DATE_FORMAT(suspended_until, '%%Y-%%m-%%d %%H:%%i') FROM users WHERE id = %u", uid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (row && row[0])
        str_copy(until, sizeof until, row[0]);
    if (qr)
        mysql_free_result(qr);

    buf_init(&title);
    buf_printf(&title, "계정이 %ld일 동안 정지되었습니다", days);
    buf_init(&body);
    buf_printf(&body,
               "정지 기간: %ld일 (%s 까지)\n"
               "사유: %s\n\n"
               "정지 기간에도 공지와 글은 볼 수 있지만, 글쓰기·댓글·스터디 참여·참여 권유·판매 글 올리기는 할 수 없습니다.\n"
               "정지 기간이 지나면 자동으로 풀립니다. 문의할 내용이 있으면 관리자에게 연락해 주세요.",
               days, until, reason);
    notify_add(uid, u.id, "suspend", title.data, body.data, NULL);
    buf_free(&title);
    buf_free(&body);

    log_info("관리자 %s 가 회원 %u 를 %ld일 정지 (%s 까지)", u.student_no, uid, days, until);
    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_str(&b, "suspended_until", until);
        buf_putc(&b, '}');
        res_json_buf(res, 200, &b);
        buf_free(&b);
    }

out:
    free(reason);
}

/* DELETE /api/admin/users/{id}/suspend - 정지를 바로 푼다 */
static void handle_unsuspend(Request *req, Response *res, unsigned uid)
{
    CurrentUser u;
    Buf sql;
    long long active;

    if (!auth_require_admin(req, res, &u))
        return;
    if (!require_student(res, uid))
        return;
    buf_init(&sql);
    db_sqlf(&sql, "SELECT suspended_until > NOW() FROM users WHERE id = %u", uid);
    active = db_scalar(sql.data, 0);
    buf_reset(&sql);
    db_sqlf(&sql, "UPDATE users SET suspended_until = NULL, suspend_reason = '' WHERE id = %u", uid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "정지를 풀 수 없습니다.");
        return;
    }
    buf_free(&sql);
    if (active)   /* 정지 중이었을 때만 알린다 */
        notify_add(uid, u.id, "unsuspend", "계정 정지가 해제되었습니다",
                   "관리자가 정지를 풀었습니다. 지금부터 다시 글쓰기·댓글·참여를 할 수 있습니다.", NULL);
    log_info("관리자 %s 가 회원 %u 의 정지를 풀었습니다.", u.student_no, uid);
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 알림 보내기 */

/* POST /api/admin/notify {user_id, title, body?, link?}
 * 받는 회원의 알림함에 kind = "admin" 알림을 넣는다. link 는 "#/..." 화면 주소만 받는다. */
static void handle_notify(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    unsigned uid;
    char *title = NULL, *body = NULL;
    const char *link;
    Buf sql;
    int exists;

    if (!auth_require_admin(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;
    uid = (unsigned)json_int(in, "user_id", 0);
    title = str_dup(json_str(in, "title", ""));
    body = str_dup(json_str(in, "body", ""));
    link = json_str(in, "link", "");
    if (!title || !body) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        goto out;
    }
    str_trim(title);
    str_trim(body);
    if (!title[0] || strlen(title) > 300) {
        res_error(res, 400, "bad_title", "제목을 1~100자로 써 주세요.");
        goto out;
    }
    if (strlen(body) > 3000) {
        res_error(res, 400, "bad_body", "내용은 1000자까지 쓸 수 있습니다.");
        goto out;
    }
    if (link[0] && (strncmp(link, "#/", 2) != 0 || strlen(link) > 190)) {
        res_error(res, 400, "bad_link", "링크는 사이트 안 화면 주소(#/...)만 넣을 수 있습니다.");
        goto out;
    }

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM users WHERE id = %u", uid);
    exists = uid && db_scalar(sql.data, 0) > 0;
    buf_free(&sql);
    if (!exists) {
        res_error(res, 404, "not_found", "회원이 없습니다.");
        goto out;
    }
    if (uid == u.id) {
        res_error(res, 400, "self", "자기 자신에게는 보낼 수 없습니다.");
        goto out;
    }

    if (!notify_add(uid, u.id, "admin", title, body, link[0] ? link : NULL)) {
        res_error(res, 500, "db_error", "알림을 보낼 수 없습니다.");
        goto out;
    }
    log_info("관리자 알림: %s -> 회원 %u '%s'", u.student_no, uid, title);
    res_json(res, 201, "{\"ok\":true}");

out:
    free(title);
    free(body);
    json_free(in);
}

/* ------------------------------------------------------------ 전체 게시물 */

/* 글 한 종류를 읽어 b 에 이어 쓴다. sql 결과 열: type, id, title, owner_id, 주인 표시 이름, 주인 학번,
 * created_at, 상태, 댓글·멤버 수. type 은 recruit / study / book. */
static void append_content(Buf *b, Buf *sql, int *first)
{
    MYSQL_RES *qr = db_query_buf(sql);
    MYSQL_ROW row;

    if (!qr)
        return;
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (!*first)
            buf_putc(b, ',');
        *first = 0;
        buf_putc(b, '{');
        json_kv_str(b, "type", row[0]);                buf_putc(b, ',');
        json_kv_int(b, "id", atoll(row[1]));           buf_putc(b, ',');
        json_kv_str(b, "title", row[2]);               buf_putc(b, ',');
        json_kv_int(b, "owner_id", atoll(row[3]));     buf_putc(b, ',');
        json_kv_str(b, "owner_nickname", row[4]);      buf_putc(b, ',');
        json_kv_str(b, "owner_student_no", row[5]);    buf_putc(b, ',');
        json_kv_str(b, "created_at", row[6]);          buf_putc(b, ',');
        json_kv_str(b, "status", row[7]);              buf_putc(b, ',');
        json_kv_int(b, "count", atoll(row[8]));
        buf_putc(b, '}');
    }
    mysql_free_result(qr);
}

/* GET /api/admin/content?type=all|recruit|study|book&q=제목
 * 응답: {"items":[{type,id,title,owner_id,owner_nickname,owner_student_no,created_at,status,count}, ...]}
 *   status: 모집글 open/closed, 스터디 "멤버수/정원", 전공책 selling/reserved/sold
 *   count : 모집글·전공책은 댓글 수, 스터디는 멤버 수
 * 종류마다 따로 읽어 이어 붙이고, 최신순 정렬은 화면이 한다. */
static void handle_content(Request *req, Response *res)
{
    CurrentUser u;
    char type[16] = "", q[128] = "";
    Buf sql, pat, b;
    int first = 1, all;

    if (!auth_require_admin(req, res, &u))
        return;
    req_query(req, "type", type, sizeof type);
    req_query(req, "q", q, sizeof q);
    str_trim(q);
    all = !type[0] || !strcmp(type, "all");
    like_pattern(&pat, q);   /* q 가 비면 '%%' = 전부 */

    buf_init(&b);
    buf_puts(&b, "{\"items\":[");

    if (all || !strcmp(type, "recruit")) {
        buf_init(&sql);
        db_sqlf(&sql,
                "SELECT 'recruit', r.id, r.title, r.author_id, u.nickname, u.student_no, "
                "       DATE_FORMAT(r.created_at, '%%Y-%%m-%%d %%H:%%i'), r.status, "
                "       (SELECT COUNT(*) FROM recruit_comments c WHERE c.recruit_id = r.id) "
                "FROM recruits r JOIN users u ON u.id = r.author_id "
                "WHERE r.title LIKE %Q ORDER BY r.created_at DESC LIMIT %d", pat.data, CONTENT_LIST_MAX);
        append_content(&b, &sql, &first);
        buf_free(&sql);
    }
    if (all || !strcmp(type, "study")) {
        buf_init(&sql);
        db_sqlf(&sql,
                "SELECT 'study', g.id, g.name, g.owner_id, u.nickname, u.student_no, "
                "       DATE_FORMAT(g.created_at, '%%Y-%%m-%%d %%H:%%i'), "
                "       CONCAT((SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id), '/', g.max_members), "
                "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id) "
                "FROM study_groups g JOIN users u ON u.id = g.owner_id "
                "WHERE g.name LIKE %Q ", pat.data);
        db_sqlf(&sql, "ORDER BY g.created_at DESC LIMIT %d", CONTENT_LIST_MAX);
        append_content(&b, &sql, &first);
        buf_free(&sql);
    }
    if (all || !strcmp(type, "book")) {
        buf_init(&sql);
        db_sqlf(&sql,
                "SELECT 'book', k.id, k.title, k.seller_id, u.nickname, u.student_no, "
                "       DATE_FORMAT(k.created_at, '%%Y-%%m-%%d %%H:%%i'), k.status, "
                "       (SELECT COUNT(*) FROM book_comments c WHERE c.book_id = k.id) "
                "FROM books k JOIN users u ON u.id = k.seller_id "
                "WHERE k.title LIKE %Q ORDER BY k.created_at DESC LIMIT %d", pat.data, CONTENT_LIST_MAX);
        append_content(&b, &sql, &first);
        buf_free(&sql);
    }
    buf_free(&pat);
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 라우터 */

int route_manage(Request *req, Response *res)
{
    unsigned id;

    if (req_is(req, "GET", "/api/admin/users"))    { handle_users(req, res);   return 1; }
    if (req_is(req, "POST", "/api/admin/notify"))  { handle_notify(req, res);  return 1; }
    if (req_is(req, "GET", "/api/admin/content"))  { handle_content(req, res); return 1; }
    if (!strcmp(req->method, "POST") && path_id(req->path, "/api/admin/users/", "/suspend", &id)) {
        handle_suspend(req, res, id);
        return 1;
    }
    if (!strcmp(req->method, "DELETE") && path_id(req->path, "/api/admin/users/", "/suspend", &id)) {
        handle_unsuspend(req, res, id);
        return 1;
    }
    if (!strcmp(req->method, "DELETE") && path_id(req->path, "/api/admin/users/", NULL, &id)) {
        handle_user_delete(req, res, id);
        return 1;
    }
    return 0;
}
