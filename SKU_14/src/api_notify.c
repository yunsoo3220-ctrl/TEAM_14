/* 알림함 (로그인한 회원에게 온 알림)
 *
 *   GET    /api/notifications                  내 알림 최근 NOTIFY_LIST_MAX 개 + 안 읽은 수 (로그인)
 *   GET    /api/notifications/unread-count     안 읽은 수만 {unread} (사이드바 배지용, 로그인)
 *   GET    /api/notifications/{id}             알림 하나 자세히 (열면 읽음으로 표시)
 *   POST   /api/notifications/{id}/read        읽음으로 표시
 *   POST   /api/notifications/read-all         모두 읽음
 *   POST   /api/notifications/{id}/accept      참여 권유 알림에서 [참여하기] (api_invite.c 가 처리)
 *   DELETE /api/notifications/{id}             알림 지우기
 *
 * 알림을 만드는 쪽은 notify_add (api.h) 를 부른다. 지금은 참여 권유(api_invite.c)가 쓴다.
 * link 는 화면 주소(#/recruits/3 처럼 # 으로 시작)만 넣는다. 다른 사이트로 가는 링크는 만들지 않는다.
 * 오래된 알림은 자동 정리(cleanup.c)가 NOTIFY_KEEP_DAYS 일 뒤 지운다.
 *
 * 관련 테이블:
 *   notifications  (받는 회원, 보낸 회원, 종류, 제목, 내용, 링크, 읽은 시각)
 */
#include "api.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTIFY_LIST_MAX 50   /* 알림함에 보여 줄 최대 개수 */

/* ------------------------------------------------------------ 만들기 (api.h 참고) */

int notify_add(unsigned user_id, unsigned actor_id, const char *kind,
               const char *title, const char *body, const char *link)
{
    Buf sql;
    int ok;

    if (!user_id || !kind || !title)
        return 0;
    if (link && link[0] != '#')   /* 사이트 안 화면 주소만 */
        link = NULL;
    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO notifications (user_id, actor_id, kind, title, body, link) VALUES (%u, ",
            user_id);
    if (actor_id)
        db_sqlf(&sql, "%u, ", actor_id);
    else
        buf_puts(&sql, "NULL, ");
    db_sqlf(&sql, "%Q, LEFT(%Q, 300), LEFT(%Q, 1000), %Q)", kind, title, body ? body : "", link ? link : "");
    ok = db_exec_buf(&sql);
    if (!ok)
        log_err("알림 저장 실패 (회원 %u): %s", user_id, db_error());
    buf_free(&sql);
    return ok;
}

static long long unread_count(unsigned user_id)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM notifications WHERE user_id = %u AND read_at IS NULL", user_id);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n;
}

/* ------------------------------------------------------------ 목록 */

/* GET /api/notifications
 * 응답: {"unread":N,"notifications":[{id,kind,title,body,link,read,created_at,actor_nickname[,invite_state]}, ...]} (최신순)
 * 참여 권유(kind = invite) 알림에는 [참여하기] 버튼을 그릴 수 있게 invite_state 를 붙인다 (api.h 참고). */
static void handle_list(Request *req, Response *res)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT n.id, n.kind, n.title, n.body, n.link, n.read_at IS NOT NULL, "
            "       DATE_FORMAT(n.created_at, '%%Y-%%m-%%d %%H:%%i'), IFNULL(a.nickname, '') "
            "FROM notifications n LEFT JOIN users a ON a.id = n.actor_id "
            "WHERE n.user_id = %u ORDER BY n.created_at DESC, n.id DESC LIMIT %d",
            u.id, NOTIFY_LIST_MAX);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "알림을 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_putc(&b, '{');
    json_kv_int(&b, "unread", unread_count(u.id));
    buf_puts(&b, ",\"notifications\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (!first)
            buf_putc(&b, ',');
        first = 0;
        buf_putc(&b, '{');
        json_kv_int(&b, "id", atoll(row[0]));                         buf_putc(&b, ',');
        json_kv_str(&b, "kind", row[1]);                              buf_putc(&b, ',');
        json_kv_str(&b, "title", row[2]);                             buf_putc(&b, ',');
        json_kv_str(&b, "body", row[3]);                              buf_putc(&b, ',');
        json_kv_str(&b, "link", row[4][0] ? row[4] : NULL);           buf_putc(&b, ',');
        json_kv_bool(&b, "read", atoi(row[5]));                       buf_putc(&b, ',');
        json_kv_str(&b, "created_at", row[6]);                        buf_putc(&b, ',');
        json_kv_str(&b, "actor_nickname", row[7][0] ? row[7] : NULL);
        if (!strcmp(row[1], "invite")) {
            buf_putc(&b, ',');
            json_kv_str(&b, "invite_state", invite_state(row[4], u.id));
        }
        buf_putc(&b, '}');
    }
    mysql_free_result(qr);
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* GET /api/notifications/{id} - 알림 하나 (목록과 같은 형태 + unread). 내 알림만, 열면 읽음으로 표시한다. */
static void handle_detail(Request *req, Response *res, unsigned nid)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE notifications SET read_at = NOW() WHERE id = %u AND user_id = %u AND read_at IS NULL",
            nid, u.id);
    db_exec_buf(&sql);
    buf_reset(&sql);
    db_sqlf(&sql,
            "SELECT n.id, n.kind, n.title, n.body, n.link, "
            "       DATE_FORMAT(n.created_at, '%%Y-%%m-%%d %%H:%%i'), IFNULL(a.nickname, '') "
            "FROM notifications n LEFT JOIN users a ON a.id = n.actor_id "
            "WHERE n.id = %u AND n.user_id = %u", nid, u.id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (!row) {
        if (qr)
            mysql_free_result(qr);
        res_error(res, 404, "not_found", "알림이 없습니다.");
        return;
    }
    buf_init(&b);
    buf_puts(&b, "{\"notification\":{");
    json_kv_int(&b, "id", atoll(row[0]));                        buf_putc(&b, ',');
    json_kv_str(&b, "kind", row[1]);                             buf_putc(&b, ',');
    json_kv_str(&b, "title", row[2]);                            buf_putc(&b, ',');
    json_kv_str(&b, "body", row[3]);                             buf_putc(&b, ',');
    json_kv_str(&b, "link", row[4][0] ? row[4] : NULL);          buf_putc(&b, ',');
    json_kv_bool(&b, "read", 1);                                 buf_putc(&b, ',');
    json_kv_str(&b, "created_at", row[5]);                       buf_putc(&b, ',');
    json_kv_str(&b, "actor_nickname", row[6][0] ? row[6] : NULL);
    if (!strcmp(row[1], "invite")) {
        buf_putc(&b, ',');
        json_kv_str(&b, "invite_state", invite_state(row[4], u.id));
    }
    mysql_free_result(qr);
    buf_puts(&b, "},");
    json_kv_int(&b, "unread", unread_count(u.id));
    buf_putc(&b, '}');
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* GET /api/notifications/unread-count */
static void handle_unread(Request *req, Response *res)
{
    CurrentUser u;
    char out[48];

    if (!auth_require(req, res, &u))
        return;
    snprintf(out, sizeof out, "{\"unread\":%lld}", unread_count(u.id));
    res_json(res, 200, out);
}

/* ------------------------------------------------------------ 읽음 · 지우기 */

/* POST /api/notifications/{id}/read, POST /api/notifications/read-all (nid = 0) */
static void handle_read(Request *req, Response *res, unsigned nid)
{
    CurrentUser u;
    Buf sql;
    char out[48];

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE notifications SET read_at = NOW() WHERE user_id = %u AND read_at IS NULL", u.id);
    if (nid)
        db_sqlf(&sql, " AND id = %u", nid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "알림을 읽음으로 표시할 수 없습니다.");
        return;
    }
    buf_free(&sql);
    snprintf(out, sizeof out, "{\"ok\":true,\"unread\":%lld}", unread_count(u.id));
    res_json(res, 200, out);
}

/* DELETE /api/notifications/{id} - 내 알림만 */
static void handle_delete(Request *req, Response *res, unsigned nid)
{
    CurrentUser u;
    Buf sql;
    char out[48];

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM notifications WHERE id = %u AND user_id = %u", nid, u.id);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "알림을 지울 수 없습니다.");
        return;
    }
    buf_free(&sql);
    if (db_affected() == 0) {
        res_error(res, 404, "not_found", "알림이 없습니다.");
        return;
    }
    snprintf(out, sizeof out, "{\"ok\":true,\"unread\":%lld}", unread_count(u.id));
    res_json(res, 200, out);
}

/* ------------------------------------------------------------ 라우터 */

int route_notify(Request *req, Response *res)
{
    unsigned id;
    const char *m = req->method;

    if (req_is(req, "GET", "/api/notifications"))               { handle_list(req, res);    return 1; }
    if (req_is(req, "GET", "/api/notifications/unread-count"))  { handle_unread(req, res);  return 1; }
    if (req_is(req, "POST", "/api/notifications/read-all"))     { handle_read(req, res, 0); return 1; }
    if (!strcmp(m, "GET") && path_id(req->path, "/api/notifications/", NULL, &id)) {
        handle_detail(req, res, id);
        return 1;
    }
    if (!strcmp(m, "POST") && path_id(req->path, "/api/notifications/", "/read", &id)) {
        handle_read(req, res, id);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/notifications/", NULL, &id)) {
        handle_delete(req, res, id);
        return 1;
    }
    return 0;
}
