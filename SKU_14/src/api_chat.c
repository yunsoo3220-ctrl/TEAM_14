/* 채팅방 (팀원 모집 팀 · 그룹 스터디에 들어간 사람끼리 대화, 전공책 사는 사람 ↔ 판매자 1:1 대화)
 *
 *   GET    /api/chats                              내 채팅방 목록 + 방마다 마지막 메시지·안 읽은 수 (로그인)
 *   GET    /api/chats/{recruit|study|book}/{id}    방 정보·멤버 + 최근 메시지 CHAT_PAGE 개 (멤버만)
 *            ?after=N    N 보다 새 메시지만 (화면이 몇 초마다 새 메시지를 받아 갈 때)
 *            ?before=N   N 보다 오래된 메시지 CHAT_PAGE 개 ([이전 대화 더 보기])
 *   POST   /api/chats/{recruit|study|book}/{id}/messages {body}   메시지 보내기 (멤버만)
 *   DELETE /api/chat-messages/{id}                 내 메시지 지우기 (쓴 사람·관리자)
 *   POST   /api/books/{id}/chat                    판매자와 1:1 채팅 시작 (이미 있으면 그 방) → {chat_id}
 *
 * 모임 방은 따로 만들지 않는다. 모집글 하나·스터디 하나가 곧 방 하나다.
 *   recruit 방 = 모집글 팀 (작성자 + 작성자가 받은 팀원, recruit_members)
 *   study 방   = 그룹 스터디 멤버 (study_members)
 *   book 방    = (판매 글, 사는 사람) 하나 (book_chats). 멤버는 사는 사람 + 판매자 두 명.
 *                사는 사람이 [판매자와 채팅하기] 를 누르면 생긴다. 판매자의 목록에는 메시지가 온 방만 보인다.
 * 그래서 모임에 들어가면 바로 그 방에 들어갈 수 있고, 나가거나 내보내지면 더는 볼 수 없다.
 * 모임이 지워지면 메시지도 자동 정리(cleanup.c)가 지운다.
 * 관리자도 멤버가 아니면 대화를 볼 수 없다 (회원끼리의 대화이므로).
 *
 * 안 읽은 수: chat_reads 에 방마다 "어디까지 읽었는지(last_read_id)" 를 두고,
 * 그보다 큰 id 의 남의 메시지 수를 센다. 방을 열거나 새 메시지를 받아 갈 때 갱신한다.
 * 실시간 연결(웹소켓) 대신 화면이 몇 초마다 ?after= 로 새 메시지를 묻는다 (서버가 단순해진다).
 *
 * 관련 테이블:
 *   chat_messages  (방 종류, 방 id, 쓴 회원, 내용)
 *   chat_reads     (방 종류, 방 id, 회원, 마지막으로 읽은 메시지 id)
 *   book_chats     (판매 글, 사는 사람) - book 방
 */
#include "api.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_PAGE        100    /* 한 번에 보여 줄 메시지 수 */
#define CHAT_AFTER_MAX   200    /* ?after= 로 한 번에 받아 갈 최대 수 */
#define CHAT_ROOMS_MAX   200    /* 채팅방 목록 최대 개수 */
#define CHAT_BODY_MAX    3000   /* 메시지 길이 (바이트, 한글 1000자) */
#define CHAT_PER_MINUTE  30     /* 한 사람이 1분 동안 보낼 수 있는 메시지 수 (도배 방지) */

/* ------------------------------------------------------------ 방 */

typedef struct {
    const char *kind;        /* "recruit" / "study" / "book" (chat_messages.room_kind, 코드 상수) */
    char     subkind[12];    /* "recruit" / "study" / "book" (화면 이름·주소용) */
    unsigned id;
    unsigned owner_id;       /* 모집글 작성자 / 스터디 방장 / 책 판매자 */
    unsigned buyer_id;       /* book 방: 사는 사람 (다른 방은 0) */
    unsigned link_id;        /* 방이 가리키는 화면의 id (book 방: 판매 글 id, 다른 방: 방 id 와 같다) */
    char     title[304];
    char     status[12];     /* 모집글 open/closed, 책 selling/reserved/sold, 스터디 open */
    int      open;           /* 모집글: 모집 중이면 1, 책: 판매 완료가 아니면 1 (스터디는 늘 1) */
} ChatRoom;

/* 종류 이름을 코드 상수로 바꾼다 (요청 문자열을 그대로 SQL 에 넣지 않으려고) */
static const char *kind_const(const char *kind)
{
    if (!strcmp(kind, "recruit")) return "recruit";
    if (!strcmp(kind, "book"))    return "book";
    return "study";
}

/* 방(모집글·스터디)을 찾아 r 을 채운다. 있으면 1. */
static int room_find(const char *kind, unsigned id, ChatRoom *r)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int found = 0;

    memset(r, 0, sizeof *r);
    kind = kind_const(kind);
    /* 열: 주인, 제목, subkind, 열려 있는지, 화면 id, 사는 사람, 상태 */
    buf_init(&sql);
    if (!strcmp(kind, "recruit"))
        db_sqlf(&sql, "SELECT author_id, title, 'recruit', status = 'open', id, 0, status "
                      "FROM recruits WHERE id = %u", id);
    else if (!strcmp(kind, "book"))
        db_sqlf(&sql, "SELECT b.seller_id, b.title, 'book', b.status <> 'sold', b.id, c.buyer_id, b.status "
                      "FROM book_chats c JOIN books b ON b.id = c.book_id WHERE c.id = %u", id);
    else
        db_sqlf(&sql, "SELECT owner_id, name, 'study', 1, id, 0, 'open' FROM study_groups WHERE id = %u", id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (row) {
        r->kind = kind;
        r->id = id;
        r->owner_id = (unsigned)strtoul(row[0], NULL, 10);
        str_copy(r->title, sizeof r->title, row[1]);
        str_copy(r->subkind, sizeof r->subkind, row[2]);
        r->open = row[3][0] == '1';
        r->link_id = (unsigned)strtoul(row[4], NULL, 10);
        r->buyer_id = (unsigned)strtoul(row[5], NULL, 10);
        str_copy(r->status, sizeof r->status, row[6]);
        found = 1;
    }
    if (qr)
        mysql_free_result(qr);
    return found;
}

/* uid 가 방의 멤버면 1 (모집글: 작성자이거나 팀원, 스터디: 멤버, 책: 판매자이거나 사는 사람) */
static int room_member(const ChatRoom *r, unsigned uid)
{
    Buf sql;
    long long n;

    if (!uid)
        return 0;
    if (!strcmp(r->kind, "book"))
        return uid == r->owner_id || uid == r->buyer_id;
    if (!strcmp(r->kind, "recruit") && r->owner_id == uid)
        return 1;
    buf_init(&sql);
    if (!strcmp(r->kind, "recruit"))
        db_sqlf(&sql, "SELECT COUNT(*) FROM recruit_members WHERE recruit_id = %u AND user_id = %u", r->id, uid);
    else
        db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u AND user_id = %u", r->id, uid);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

/* 방 종류의 화면 주소 앞부분 (#/recruits/3, #/studies/4, #/books/5) */
static const char *link_path(const char *subkind)
{
    if (!strcmp(subkind, "study")) return "studies";
    if (!strcmp(subkind, "book"))  return "books";
    return "recruits";
}

static void room_link(const ChatRoom *r, char *out, size_t outsz)
{
    snprintf(out, outsz, "#/%s/%u", link_path(r->subkind), r->link_id);
}

/* "/api/chats/recruit/12" (suffix 가 있으면 "/api/chats/recruit/12/messages") 에서 방을 꺼낸다.
 * 형식이 맞으면 1 과 종류 코드·id */
static int room_path(const char *path, const char *suffix, const char **kind, unsigned *id)
{
    if (path_id(path, "/api/chats/recruit/", suffix, id)) { *kind = "recruit"; return 1; }
    if (path_id(path, "/api/chats/study/", suffix, id))   { *kind = "study";   return 1; }
    if (path_id(path, "/api/chats/book/", suffix, id))    { *kind = "book";    return 1; }
    return 0;
}

/* 요청한 회원이 들어갈 수 있는 방인지 확인한다. 되면 1, 아니면 응답을 채우고 0. */
static int room_require(const Request *req, Response *res, const char *kind, unsigned id,
                        CurrentUser *u, ChatRoom *r)
{
    if (!auth_require(req, res, u))
        return 0;
    if (!room_find(kind, id, r)) {
        res_error(res, 404, "not_found", "채팅방이 없습니다. 모임이 지워졌을 수 있습니다.");
        return 0;
    }
    if (!room_member(r, u->id)) {
        res_error(res, 403, "not_member", !strcmp(r->kind, "book")
                  ? "이 대화의 판매자와 사는 사람만 들어갈 수 있습니다."
                  : "이 모임에 들어간 사람만 채팅방에 들어갈 수 있습니다.");
        return 0;
    }
    return 1;
}

/* s 를 maxbytes 이하로 자르되 UTF-8 글자 중간에서 자르지 않는다 (api_community.c 의 같은 이름 함수와 동일) */
static void utf8_cut(char *s, size_t maxbytes)
{
    size_t n = strlen(s);

    if (n <= maxbytes)
        return;
    n = maxbytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    s[n] = '\0';
}

/* ------------------------------------------------------------ 안 읽은 수 (api.h 참고) */

long long chat_unread_total(unsigned user_id)
{
    Buf sql;
    long long n;

    if (!user_id)
        return 0;
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT COUNT(*) FROM chat_messages x "
            "WHERE x.user_id <> %u "
            "  AND ((x.room_kind = 'study' AND x.room_id IN "
            "          (SELECT group_id FROM study_members WHERE user_id = %u)) "
            "    OR (x.room_kind = 'recruit' AND (x.room_id IN (SELECT id FROM recruits WHERE author_id = %u) "
            "          OR x.room_id IN (SELECT recruit_id FROM recruit_members WHERE user_id = %u))) "
            "    OR (x.room_kind = 'book' AND x.room_id IN "
            "          (SELECT bc.id FROM book_chats bc JOIN books b ON b.id = bc.book_id "
            "           WHERE bc.buyer_id = %u OR b.seller_id = %u))) "
            "  AND x.id > IFNULL((SELECT cr.last_read_id FROM chat_reads cr WHERE cr.room_kind = x.room_kind "
            "                     AND cr.room_id = x.room_id AND cr.user_id = %u), 0)",
            user_id, user_id, user_id, user_id, user_id, user_id, user_id);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n;
}

/* 방을 마지막 메시지까지 읽은 것으로 표시한다 */
static void mark_read(const ChatRoom *r, unsigned uid)
{
    Buf sql;

    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO chat_reads (room_kind, room_id, user_id, last_read_id) "
            "SELECT '%s', %u, %u, IFNULL(MAX(id), 0) FROM chat_messages WHERE room_kind = '%s' AND room_id = %u "
            "ON DUPLICATE KEY UPDATE last_read_id = GREATEST(last_read_id, VALUES(last_read_id))",
            r->kind, r->id, uid, r->kind, r->id);
    db_exec_buf(&sql);
    buf_free(&sql);
}

/* ------------------------------------------------------------ 목록 */

/* 목록 한 줄 (방 + 마지막 메시지) */
typedef struct {
    char     kind[8], subkind[12], status[12];
    unsigned id, link_id;
    char     title[304];
    char     peer[64];      /* book 방: 대화 상대 닉네임 (다른 방은 "") */
    long long members, last_id, unread;
    char     last_body[256], last_nickname[64], last_at[20];
} RoomRow;

/* 두 방 종류가 같이 쓰는 열: 마지막 메시지 id·내용 앞부분·쓴 사람·시각, 안 읽은 수.
 * kind 는 코드 상수, id_expr 는 바깥 질의의 방 id 열 이름 (둘 다 코드에서 정한 값) */
static void append_last_cols(Buf *sql, const char *kind, const char *id_expr, unsigned uid)
{
    db_sqlf(sql,
            "IFNULL(c.id, 0), IFNULL(LEFT(REPLACE(REPLACE(c.body, '\\r', ''), '\\n', ' '), 60), ''), "
            "IFNULL(cu.nickname, ''), IFNULL(DATE_FORMAT(c.created_at, '%%Y-%%m-%%d %%H:%%i'), ''), "
            "(SELECT COUNT(*) FROM chat_messages x WHERE x.room_kind = '%s' AND x.room_id = %s AND x.user_id <> %u "
            "   AND x.id > IFNULL((SELECT cr.last_read_id FROM chat_reads cr WHERE cr.room_kind = '%s' "
            "                      AND cr.room_id = %s AND cr.user_id = %u), 0)) ",
            kind, id_expr, uid, kind, id_expr, uid);
}

static void append_last_join(Buf *sql, const char *kind, const char *id_expr)
{
    db_sqlf(sql,
            "LEFT JOIN chat_messages c ON c.id = (SELECT MAX(y.id) FROM chat_messages y "
            "                                     WHERE y.room_kind = '%s' AND y.room_id = %s) "
            "LEFT JOIN users cu ON cu.id = c.user_id ",
            kind, id_expr);
}

/* 질의 결과를 rows 뒤에 붙인다. 붙인 뒤의 개수를 돌려준다.
 * 열: kind, id, subkind, title, members, status, 화면 id, 대화 상대, (append_last_cols 의 다섯 열) */
static int collect_rooms(Buf *sql, RoomRow *rows, int n)
{
    MYSQL_RES *qr = db_query_buf(sql);
    MYSQL_ROW row;

    if (!qr)
        return n;
    while (n < CHAT_ROOMS_MAX && (row = mysql_fetch_row(qr)) != NULL) {
        RoomRow *r = &rows[n++];
        str_copy(r->kind, sizeof r->kind, row[0]);
        r->id = (unsigned)strtoul(row[1], NULL, 10);
        str_copy(r->subkind, sizeof r->subkind, row[2]);
        str_copy(r->title, sizeof r->title, row[3]);
        r->members = atoll(row[4]);
        str_copy(r->status, sizeof r->status, row[5]);
        r->link_id = (unsigned)strtoul(row[6], NULL, 10);
        str_copy(r->peer, sizeof r->peer, row[7]);
        r->last_id = atoll(row[8]);
        str_copy(r->last_body, sizeof r->last_body, row[9]);
        str_copy(r->last_nickname, sizeof r->last_nickname, row[10]);
        str_copy(r->last_at, sizeof r->last_at, row[11]);
        r->unread = atoll(row[12]);
    }
    mysql_free_result(qr);
    return n;
}

/* 최근 대화가 있는 방이 위로. 대화가 없는 방은 그 아래에 새 모임부터 */
static int room_order(const void *a, const void *b)
{
    const RoomRow *x = (const RoomRow *)a, *y = (const RoomRow *)b;

    if (x->last_id != y->last_id)
        return x->last_id < y->last_id ? 1 : -1;
    return x->id < y->id ? 1 : (x->id > y->id ? -1 : 0);
}

/* GET /api/chats
 * 응답: {"rooms":[{kind, subkind, id, title, link, members, closed, status, peer, unread,
 *                  last:{body, nickname, created_at} 또는 null}, ...], "unread": 전체 안 읽은 수} */
static void handle_rooms(Request *req, Response *res)
{
    CurrentUser u;
    Buf sql, b;
    RoomRow *rows;
    int n = 0, i;
    long long total = 0;

    if (!auth_require(req, res, &u))
        return;
    rows = calloc(CHAT_ROOMS_MAX, sizeof *rows);
    if (!rows) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        return;
    }

    /* 모집글 팀: 내가 쓴 글 + 팀원으로 들어간 글 */
    buf_init(&sql);
    buf_puts(&sql, "SELECT 'recruit', r.id, 'recruit', r.title, "
                   "1 + (SELECT COUNT(*) FROM recruit_members m WHERE m.recruit_id = r.id), r.status, r.id, '', ");
    append_last_cols(&sql, "recruit", "r.id", u.id);
    buf_puts(&sql, "FROM recruits r ");
    append_last_join(&sql, "recruit", "r.id");
    db_sqlf(&sql, "WHERE r.author_id = %u OR EXISTS (SELECT 1 FROM recruit_members m "
                  "WHERE m.recruit_id = r.id AND m.user_id = %u)", u.id, u.id);
    n = collect_rooms(&sql, rows, n);
    buf_free(&sql);

    /* 스터디: 내가 멤버인 것 */
    buf_init(&sql);
    buf_puts(&sql, "SELECT 'study', g.id, 'study', g.name, "
                   "(SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id), 'open', g.id, '', ");
    append_last_cols(&sql, "study", "g.id", u.id);
    buf_puts(&sql, "FROM study_groups g ");
    append_last_join(&sql, "study", "g.id");
    db_sqlf(&sql, "WHERE EXISTS (SELECT 1 FROM study_members me WHERE me.group_id = g.id AND me.user_id = %u)",
            u.id);
    n = collect_rooms(&sql, rows, n);
    buf_free(&sql);

    /* 전공책 1:1: 내가 채팅을 건 방 + 내 책에 온 방 (판매자에게는 메시지가 하나라도 있는 방만) */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT 'book', bc.id, 'book', b.title, 2, b.status, b.id, "
                  "IF(b.seller_id = %u, bu.nickname, su.nickname), ", u.id);
    append_last_cols(&sql, "book", "bc.id", u.id);
    buf_puts(&sql, "FROM book_chats bc JOIN books b ON b.id = bc.book_id "
                   "JOIN users bu ON bu.id = bc.buyer_id JOIN users su ON su.id = b.seller_id ");
    append_last_join(&sql, "book", "bc.id");
    db_sqlf(&sql, "WHERE bc.buyer_id = %u OR (b.seller_id = %u AND EXISTS (SELECT 1 FROM chat_messages z "
                  "WHERE z.room_kind = 'book' AND z.room_id = bc.id))", u.id, u.id);
    n = collect_rooms(&sql, rows, n);
    buf_free(&sql);

    qsort(rows, (size_t)n, sizeof *rows, room_order);

    buf_init(&b);
    buf_puts(&b, "{\"rooms\":[");
    for (i = 0; i < n; i++) {
        const RoomRow *r = &rows[i];
        char link[48];
        snprintf(link, sizeof link, "#/%s/%u", link_path(r->subkind), r->link_id);
        if (i)
            buf_putc(&b, ',');
        buf_putc(&b, '{');
        json_kv_str(&b, "kind", r->kind);                    buf_putc(&b, ',');
        json_kv_str(&b, "subkind", r->subkind);              buf_putc(&b, ',');
        json_kv_int(&b, "id", r->id);                        buf_putc(&b, ',');
        json_kv_str(&b, "title", r->title);                  buf_putc(&b, ',');
        json_kv_str(&b, "link", link);                       buf_putc(&b, ',');
        json_kv_int(&b, "members", r->members);              buf_putc(&b, ',');
        json_kv_bool(&b, "closed", !strcmp(r->status, "closed") || !strcmp(r->status, "sold")); buf_putc(&b, ',');
        json_kv_str(&b, "status", r->status);                buf_putc(&b, ',');
        json_kv_str(&b, "peer", r->peer[0] ? r->peer : NULL); buf_putc(&b, ',');
        json_kv_int(&b, "unread", r->unread);                buf_putc(&b, ',');
        buf_puts(&b, "\"last\":");
        if (r->last_id) {
            buf_putc(&b, '{');
            json_kv_str(&b, "body", r->last_body);           buf_putc(&b, ',');
            json_kv_str(&b, "nickname", r->last_nickname);   buf_putc(&b, ',');
            json_kv_str(&b, "created_at", r->last_at);
            buf_putc(&b, '}');
        } else {
            buf_puts(&b, "null");
        }
        buf_putc(&b, '}');
        total += r->unread;
    }
    buf_puts(&b, "],");
    json_kv_int(&b, "unread", total);
    buf_putc(&b, '}');
    free(rows);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 방 하나 */

/* 메시지 한 줄 JSON. row: id, user_id, nickname, body, created_at */
static void write_message(Buf *b, MYSQL_ROW row, unsigned me)
{
    unsigned uid = (unsigned)strtoul(row[1], NULL, 10);

    buf_putc(b, '{');
    json_kv_int(b, "id", atoll(row[0]));       buf_putc(b, ',');
    json_kv_int(b, "user_id", uid);            buf_putc(b, ',');
    json_kv_str(b, "nickname", row[2]);        buf_putc(b, ',');
    json_kv_str(b, "body", row[3]);            buf_putc(b, ',');
    json_kv_str(b, "created_at", row[4]);      buf_putc(b, ',');
    json_kv_bool(b, "mine", uid == me);
    buf_putc(b, '}');
}

#define MESSAGE_COLS \
    "m.id, m.user_id, u.nickname, m.body, DATE_FORMAT(m.created_at, '%%Y-%%m-%%d %%H:%%i') "

/* 방 멤버 목록 JSON: [{user_id, nickname, department, role, is_owner}] (방장·작성자·판매자 먼저) */
static void write_members(Buf *b, const ChatRoom *r)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    buf_init(&sql);
    if (!strcmp(r->kind, "recruit"))
        db_sqlf(&sql,
                "SELECT t.user_id, u.nickname, IFNULL(d.name, ''), 'member', t.user_id = %u "
                "FROM (SELECT %u AS user_id, '1000-01-01' AS joined_at "
                "      UNION ALL SELECT user_id, joined_at FROM recruit_members WHERE recruit_id = %u) t "
                "JOIN users u ON u.id = t.user_id LEFT JOIN departments d ON d.id = u.department_id "
                "ORDER BY t.joined_at, t.user_id", r->owner_id, r->owner_id, r->id);
    else if (!strcmp(r->kind, "book"))
        db_sqlf(&sql,
                "SELECT u.id, u.nickname, IFNULL(d.name, ''), IF(u.id = %u, 'seller', 'buyer'), u.id = %u "
                "FROM users u LEFT JOIN departments d ON d.id = u.department_id "
                "WHERE u.id IN (%u, %u) ORDER BY u.id <> %u",
                r->owner_id, r->owner_id, r->owner_id, r->buyer_id, r->owner_id);
    else
        db_sqlf(&sql,
                "SELECT m.user_id, u.nickname, IFNULL(d.name, ''), m.role, m.user_id = %u "
                "FROM study_members m JOIN users u ON u.id = m.user_id "
                "LEFT JOIN departments d ON d.id = u.department_id "
                "WHERE m.group_id = %u ORDER BY m.user_id <> %u, m.joined_at, m.user_id",
                r->owner_id, r->id, r->owner_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    buf_putc(b, '[');
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(b, ',');
            first = 0;
            buf_putc(b, '{');
            json_kv_int(b, "user_id", atoll(row[0]));                 buf_putc(b, ',');
            json_kv_str(b, "nickname", row[1]);                       buf_putc(b, ',');
            json_kv_str(b, "department", row[2][0] ? row[2] : NULL);  buf_putc(b, ',');
            json_kv_str(b, "role", row[3]);                           buf_putc(b, ',');
            json_kv_bool(b, "is_owner", atoi(row[4]));
            buf_putc(b, '}');
        }
        mysql_free_result(qr);
    }
    buf_putc(b, ']');
}

/* GET /api/chats/{kind}/{id}[?after=N | ?before=N]
 * 응답: {"messages":[{id, user_id, nickname, body, created_at, mine}, ...] (오래된 것부터),
 *        "has_more": 더 오래된 메시지가 있는지 (after 가 없을 때만),
 *        "room":{kind, subkind, id, title, link, owner_id, closed, status}, "members":[...] (처음 열 때만)} */
static void handle_room(Request *req, Response *res, const char *kind, unsigned id)
{
    CurrentUser u;
    ChatRoom r;
    char q[16] = "";
    unsigned long after = 0, before = 0;
    long long oldest = 0;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    if (!room_require(req, res, kind, id, &u, &r))
        return;
    if (req_query(req, "after", q, sizeof q))
        after = strtoul(q, NULL, 10);
    if (req_query(req, "before", q, sizeof q))
        before = strtoul(q, NULL, 10);

    buf_init(&sql);
    if (after)
        db_sqlf(&sql, "SELECT " MESSAGE_COLS "FROM chat_messages m JOIN users u ON u.id = m.user_id "
                      "WHERE m.room_kind = '%s' AND m.room_id = %u AND m.id > %u ORDER BY m.id LIMIT %d",
                r.kind, r.id, (unsigned)after, CHAT_AFTER_MAX);
    else {
        /* 최근 CHAT_PAGE 개를 거꾸로 뽑아 다시 오래된 순으로 */
        db_sqlf(&sql, "SELECT * FROM (SELECT " MESSAGE_COLS "FROM chat_messages m JOIN users u ON u.id = m.user_id "
                      "WHERE m.room_kind = '%s' AND m.room_id = %u ", r.kind, r.id);
        if (before)
            db_sqlf(&sql, "AND m.id < %u ", (unsigned)before);
        db_sqlf(&sql, "ORDER BY m.id DESC LIMIT %d) t ORDER BY t.id", CHAT_PAGE);
    }
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "메시지를 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"messages\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (first)
            oldest = atoll(row[0]);
        if (!first)
            buf_putc(&b, ',');
        first = 0;
        write_message(&b, row, u.id);
    }
    mysql_free_result(qr);
    buf_putc(&b, ']');

    if (!after) {
        long long more = 0;
        if (oldest) {
            buf_init(&sql);
            db_sqlf(&sql, "SELECT EXISTS (SELECT 1 FROM chat_messages WHERE room_kind = '%s' AND room_id = %u "
                          "AND id < %L)", r.kind, r.id, oldest);
            more = db_scalar(sql.data, 0);
            buf_free(&sql);
        }
        buf_putc(&b, ',');
        json_kv_bool(&b, "has_more", more > 0);
    }

    if (!after && !before) {
        char link[48];
        room_link(&r, link, sizeof link);
        buf_puts(&b, ",\"room\":{");
        json_kv_str(&b, "kind", r.kind);                  buf_putc(&b, ',');
        json_kv_str(&b, "subkind", r.subkind);            buf_putc(&b, ',');
        json_kv_int(&b, "id", r.id);                      buf_putc(&b, ',');
        json_kv_str(&b, "title", r.title);                buf_putc(&b, ',');
        json_kv_str(&b, "link", link);                    buf_putc(&b, ',');
        json_kv_int(&b, "owner_id", r.owner_id);          buf_putc(&b, ',');
        json_kv_bool(&b, "closed", !r.open);              buf_putc(&b, ',');
        json_kv_str(&b, "status", r.status);
        buf_puts(&b, "},\"members\":");
        write_members(&b, &r);
    }
    buf_putc(&b, '}');

    if (!before)   /* 지난 대화를 거슬러 올라가는 것은 읽음과 상관없다 */
        mark_read(&r, u.id);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* POST /api/chats/{kind}/{id}/messages {body} - 응답: {"message":{...}} */
static void handle_send(Request *req, Response *res, const char *kind, unsigned id)
{
    CurrentUser u;
    ChatRoom r;
    Json *in;
    char *body;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    unsigned long long mid;
    int ok;

    if (!room_require(req, res, kind, id, &u, &r))
        return;
    in = body_object(req, res);
    if (!in)
        return;
    body = str_dup(json_str(in, "body", ""));
    json_free(in);
    if (!body) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        return;
    }
    str_trim(body);
    if (!body[0] || strlen(body) > CHAT_BODY_MAX) {
        free(body);
        res_error(res, 400, "bad_body", "메시지는 1~1000자로 써 주세요.");
        return;
    }
    utf8_cut(body, CHAT_BODY_MAX);

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM chat_messages WHERE user_id = %u AND created_at > NOW() - INTERVAL 1 MINUTE",
            u.id);
    ok = db_scalar(sql.data, 0) < CHAT_PER_MINUTE;
    buf_free(&sql);
    if (!ok) {
        free(body);
        res_error(res, 429, "too_many", "메시지를 너무 빨리 보내고 있습니다. 잠시 뒤에 다시 보내 주세요.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO chat_messages (room_kind, room_id, user_id, body) VALUES ('%s', %u, %u, %Q)",
            r.kind, r.id, u.id, body);
    db_lock();   /* db_last_id 가 다른 스레드의 값이 되지 않게 */
    ok = db_exec_buf(&sql);
    mid = ok ? db_last_id() : 0;
    db_unlock();
    buf_free(&sql);
    if (!ok) {
        free(body);
        res_error(res, 500, "db_error", "메시지를 보낼 수 없습니다.");
        return;
    }
    free(body);   /* 채팅은 알림함에 알리지 않는다 (안 읽은 수는 사이드바 채팅방 배지로 보인다) */

    /* 방금 쓴 메시지를 그대로 돌려줘 화면이 바로 그릴 수 있게 한다 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT " MESSAGE_COLS "FROM chat_messages m JOIN users u ON u.id = m.user_id WHERE m.id = %L",
            (long long)mid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    buf_init(&b);
    buf_puts(&b, "{\"ok\":true,\"message\":");
    if (row)
        write_message(&b, row, u.id);
    else
        buf_puts(&b, "null");
    buf_putc(&b, '}');
    if (qr)
        mysql_free_result(qr);
    res_json_buf(res, 201, &b);
    buf_free(&b);
}

/* DELETE /api/chat-messages/{id} (쓴 사람·관리자) */
static void handle_delete(Request *req, Response *res, unsigned mid)
{
    CurrentUser u;
    Buf sql;
    int ok;
    unsigned long long removed;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    if (u.is_admin)
        db_sqlf(&sql, "DELETE FROM chat_messages WHERE id = %u", mid);
    else
        db_sqlf(&sql, "DELETE FROM chat_messages WHERE id = %u AND user_id = %u", mid, u.id);
    db_lock();
    ok = db_exec_buf(&sql);
    removed = ok ? db_affected() : 0;
    db_unlock();
    buf_free(&sql);
    if (!ok) {
        res_error(res, 500, "db_error", "메시지를 지울 수 없습니다.");
        return;
    }
    if (!removed) {
        res_error(res, 403, "not_allowed", "내가 쓴 메시지만 지울 수 있습니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 전공책 1:1 채팅 시작 */

/* POST /api/books/{id}/chat - 사는 사람이 판매자와의 방을 연다 (이미 있으면 그 방).
 * 판매 완료된 책에는 새 방을 열 수 없다 (이미 연 방은 그대로 쓸 수 있다).
 * 응답: {"ok":true, "chat_id":N} */
static void handle_book_chat_start(Request *req, Response *res, unsigned book_id)
{
    CurrentUser u;
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    unsigned seller = 0;
    int found = 0, sold = 0;
    long long cid;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    db_sqlf(&sql, "SELECT seller_id, status = 'sold' FROM books WHERE id = %u", book_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (row) {
        found = 1;
        seller = (unsigned)strtoul(row[0], NULL, 10);
        sold = row[1][0] == '1';
    }
    if (qr)
        mysql_free_result(qr);
    if (!found) {
        res_error(res, 404, "not_found", "판매 글이 없습니다. 지워졌을 수 있습니다.");
        return;
    }
    if (seller == u.id) {
        res_error(res, 400, "own_book", "내가 올린 책에는 채팅을 걸 수 없습니다. 채팅방 목록에서 온 대화를 보세요.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "SELECT id FROM book_chats WHERE book_id = %u AND buyer_id = %u", book_id, u.id);
    cid = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (!cid) {
        if (sold) {
            res_error(res, 409, "sold", "판매 완료된 책입니다.");
            return;
        }
        /* 두 번 눌러도 방은 하나 (uq_book_chats) */
        buf_init(&sql);
        db_sqlf(&sql, "INSERT IGNORE INTO book_chats (book_id, buyer_id) VALUES (%u, %u)", book_id, u.id);
        db_exec_buf(&sql);
        buf_free(&sql);
        buf_init(&sql);
        db_sqlf(&sql, "SELECT id FROM book_chats WHERE book_id = %u AND buyer_id = %u", book_id, u.id);
        cid = db_scalar(sql.data, 0);
        buf_free(&sql);
        if (!cid) {
            res_error(res, 500, "db_error", "채팅방을 만들 수 없습니다.");
            return;
        }
    }

    buf_init(&sql);
    buf_puts(&sql, "{\"ok\":true,");
    json_kv_int(&sql, "chat_id", cid);
    buf_putc(&sql, '}');
    res_json_buf(res, 200, &sql);
    buf_free(&sql);
}

/* ------------------------------------------------------------ 라우터 */

int route_chat(Request *req, Response *res)
{
    const char *kind;
    unsigned id;
    const char *m = req->method;

    if (req_is(req, "GET", "/api/chats")) { handle_rooms(req, res); return 1; }
    if (!strcmp(m, "POST") && path_id(req->path, "/api/books/", "/chat", &id)) {
        handle_book_chat_start(req, res, id);
        return 1;
    }
    if (!strcmp(m, "POST") && room_path(req->path, "/messages", &kind, &id)) {
        handle_send(req, res, kind, id);
        return 1;
    }
    if (!strcmp(m, "GET") && room_path(req->path, NULL, &kind, &id)) {
        handle_room(req, res, kind, id);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/chat-messages/", NULL, &id)) {
        handle_delete(req, res, id);
        return 1;
    }
    return 0;
}
