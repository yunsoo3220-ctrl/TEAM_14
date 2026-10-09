/* 관심 키워드 · 프로필 · 잘 맞는 회원 · 팀원 모집 게시판
 *
 *   GET    /api/tags                       고를 수 있는 키워드 (분야/역할/성향)
 *   GET    /api/profile                    내 키워드와 자기소개 (로그인)
 *   PUT    /api/profile                    {interest_ids:[...], bio}
 *   GET    /api/members/matches            나와 성향이 비슷한 회원 (로그인)
 *   GET    /api/recruits                   모집글 목록 ?status=open|all&tag=&post_id=&sort=new|match
 *   POST   /api/recruits                   모집글 쓰기 (로그인)
 *   GET    /api/recruits/{id}              모집글 + 댓글
 *   PUT    /api/recruits/{id}/status       {status:"open"|"closed"} (작성자·관리자)
 *   DELETE /api/recruits/{id}              (작성자·관리자)
 *   POST   /api/recruits/{id}/comments     (로그인)
 *   DELETE /api/recruit-comments/{id}      (작성자·관리자)
 *   POST   /api/recruits/{id}/like         마음에 들어요 (로그인, 내 글 제외) -> {liked, like_count}
 *   DELETE /api/recruits/{id}/like         취소
 *
 * 마음에 들어요는 추천 모델(src/ml.c)이 관련 공모전·잘 맞는 팀원을 찾는 근거로도 쓴다.
 *
 * 다른 회원에게 보이는 것은 표시 이름·학과·키워드·자기소개뿐이다. 실명·전화번호·학번은 내보내지 않는다. */
#include "api.h"
#include "db.h"
#include "ml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TAGS        8
#define BIO_MAX_BYTES   1500      /* VARCHAR(500) 글자 = 한글 1500 바이트 */
#define MATCH_LIMIT_MAX 30
#define RECRUIT_LIST_MAX 200

/* ------------------------------------------------------------ 도우미 */

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

static const char *clean_date(const char *s)
{
    int i;

    if (!s || strlen(s) != 10)
        return NULL;
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return NULL;
        } else if (s[i] < '0' || s[i] > '9') {
            return NULL;
        }
    }
    return s;
}

/* "영상,기획" -> ["영상","기획"] */
static void write_csv_array(Buf *b, const char *csv)
{
    char buf[1024], *p, *next;
    int first = 1;

    buf_putc(b, '[');
    str_copy(buf, sizeof buf, csv ? csv : "");
    for (p = buf; p && *p; p = next) {
        next = strchr(p, ',');
        if (next)
            *next++ = '\0';
        if (!*p)
            continue;
        if (!first)
            buf_putc(b, ',');
        first = 0;
        json_write_str(b, p);
    }
    buf_putc(b, ']');
}

/* 회원이 고른 키워드 이름들 (쉼표로 이음). 호출자가 free. */
static char *user_tag_csv(unsigned user_id)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char *out = NULL;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT IFNULL(GROUP_CONCAT(t.name ORDER BY t.sort SEPARATOR ','), '') "
            "FROM user_interests ui JOIN interest_tags t ON t.id = ui.tag_id "
            "WHERE ui.user_id = %u", user_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        row = mysql_fetch_row(qr);
        out = str_dup(row && row[0] ? row[0] : "");
        mysql_free_result(qr);
    }
    return out ? out : str_dup("");
}

int profile_text(unsigned user_id, Buf *out)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int has = 0;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT IFNULL((SELECT GROUP_CONCAT(t.name SEPARATOR ' ') FROM user_interests ui "
            "               JOIN interest_tags t ON t.id = ui.tag_id WHERE ui.user_id = u.id), ''), "
            "       u.bio FROM users u WHERE u.id = %u", user_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr)
        return 0;
    row = mysql_fetch_row(qr);
    if (row) {
        buf_puts(out, row[0]);
        buf_putc(out, '\n');
        buf_puts(out, row[1]);
        has = row[0][0] || row[1][0];
    }
    mysql_free_result(qr);

    /* 최근에 마음에 들어한 모집글 10개: 제목, 찾는 키워드, 함께 나갈 공모전 제목.
     * 개인 맞춤 추천이 "끌린 팀 프로젝트"와 비슷한 공모전을 위로 올린다. */
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT r.title, IFNULL((SELECT GROUP_CONCAT(t.name SEPARATOR ' ') FROM recruit_tags rt "
            "                        JOIN interest_tags t ON t.id = rt.tag_id WHERE rt.recruit_id = r.id), ''), "
            "       IFNULL(p.title, '') "
            "FROM recruit_likes l JOIN recruits r ON r.id = l.recruit_id "
            "LEFT JOIN posts p ON p.id = r.post_id "
            "WHERE l.user_id = %u ORDER BY l.created_at DESC LIMIT 10", user_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            buf_putc(out, '\n');
            buf_puts(out, row[0]);
            buf_putc(out, ' ');
            buf_puts(out, row[1]);
            buf_putc(out, ' ');
            buf_puts(out, row[2]);
            has = 1;
        }
        mysql_free_result(qr);
    }
    return has;
}

int profile_save(unsigned user_id, const unsigned *tag_ids, int ntags, const char *bio)
{
    Buf sql;
    char *b = str_dup(bio ? bio : "");
    int ok, i;

    if (!b)
        return 0;
    str_trim(b);
    utf8_cut(b, BIO_MAX_BYTES);
    if (ntags > MAX_TAGS)
        ntags = MAX_TAGS;

    if (!db_begin()) {
        free(b);
        return 0;
    }
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE users SET bio = %Q WHERE id = %u", b, user_id);
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    free(b);

    if (ok) {
        buf_init(&sql);
        db_sqlf(&sql, "DELETE FROM user_interests WHERE user_id = %u", user_id);
        ok = db_exec_buf(&sql);
        buf_free(&sql);
    }
    if (ok && ntags > 0) {
        buf_init(&sql);
        db_sqlf(&sql, "INSERT IGNORE INTO user_interests (user_id, tag_id) "
                      "SELECT %u, id FROM interest_tags WHERE id IN (", user_id);
        for (i = 0; i < ntags; i++)
            db_sqlf(&sql, i ? ",%u" : "%u", tag_ids[i]);
        buf_putc(&sql, ')');
        ok = db_exec_buf(&sql);
        buf_free(&sql);
    }
    if (ok)
        return db_commit();
    db_rollback();
    return 0;
}

/* ------------------------------------------------------------ 키워드 · 프로필 */

static void handle_tags(Response *res)
{
    static const char *CATS[][2] = {
        { "field", "관심 분야" }, { "role", "맡고 싶은 역할" }, { "style", "협업 성향" }
    };
    MYSQL_RES *qr;
    MYSQL_ROW row;
    Buf b;
    int c;

    buf_init(&b);
    buf_puts(&b, "{\"groups\":[");
    for (c = 0; c < 3; c++) {
        Buf sql;
        int first = 1;

        if (c)
            buf_putc(&b, ',');
        buf_putc(&b, '{');
        json_kv_str(&b, "category", CATS[c][0]);   buf_putc(&b, ',');
        json_kv_str(&b, "label", CATS[c][1]);      buf_puts(&b, ",\"tags\":[");

        buf_init(&sql);
        db_sqlf(&sql, "SELECT id, name FROM interest_tags WHERE category = '%s' ORDER BY sort, id",
                CATS[c][0]);
        qr = db_query_buf(&sql);
        buf_free(&sql);
        if (qr) {
            while ((row = mysql_fetch_row(qr)) != NULL) {
                if (!first)
                    buf_putc(&b, ',');
                first = 0;
                buf_putc(&b, '{');
                json_kv_int(&b, "id", atoll(row[0]));  buf_putc(&b, ',');
                json_kv_str(&b, "name", row[1]);
                buf_putc(&b, '}');
            }
            mysql_free_result(qr);
        }
        buf_puts(&b, "]}");
    }
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

static void handle_profile_get(Request *req, Response *res)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    if (!auth_require(req, res, &u))
        return;

    buf_init(&b);
    buf_putc(&b, '{');
    buf_init(&sql);
    db_sqlf(&sql, "SELECT bio FROM users WHERE id = %u", u.id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    json_kv_str(&b, "bio", row ? row[0] : "");
    if (qr)
        mysql_free_result(qr);

    buf_puts(&b, ",\"interest_ids\":[");
    buf_init(&sql);
    db_sqlf(&sql, "SELECT tag_id FROM user_interests WHERE user_id = %u ORDER BY tag_id", u.id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_puts(&b, row[0]);
        }
        mysql_free_result(qr);
    }
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

static void handle_profile_put(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    unsigned ids[MAX_TAGS + 8];
    int n;

    if (!auth_require(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    n = json_uint_array(json_get(in, "interest_ids"), ids, (int)(sizeof ids / sizeof ids[0]));
    if (n > MAX_TAGS) {
        json_free(in);
        res_error(res, 400, "too_many_tags", "키워드는 8개까지 고를 수 있습니다.");
        return;
    }
    if (!profile_save(u.id, ids, n, json_str(in, "bio", ""))) {
        json_free(in);
        res_error(res, 500, "db_error", "프로필을 저장할 수 없습니다.");
        return;
    }
    json_free(in);
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 잘 맞는 회원 */

static void handle_matches(Request *req, Response *res)
{
    CurrentUser u;
    char limit_s[8];
    int limit, n, i, first = 1;
    MlHit hits[MATCH_LIMIT_MAX];
    char *mine;
    Buf b;

    if (!auth_require(req, res, &u))
        return;
    req_query(req, "limit", limit_s, sizeof limit_s);
    limit = atoi(limit_s);
    if (limit <= 0 || limit > MATCH_LIMIT_MAX)
        limit = 12;

    mine = user_tag_csv(u.id);
    n = u.is_admin ? 0 : ml_match_users(u.id, 20, hits, limit);
    if (n < 0) {
        free(mine);
        res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"my_tags\":");
    write_csv_array(&b, mine);
    free(mine);
    buf_puts(&b, ",\"members\":[");

    for (i = 0; i < n; i++) {
        Buf sql;
        MYSQL_RES *qr;
        MYSQL_ROW row;

        buf_init(&sql);
        db_sqlf(&sql,
                "SELECT u.nickname, IFNULL(d.name, ''), IFNULL(c.name, ''), u.bio, "
                "  IFNULL((SELECT GROUP_CONCAT(t.name ORDER BY t.sort SEPARATOR ',') "
                "          FROM user_interests ui JOIN interest_tags t ON t.id = ui.tag_id "
                "          WHERE ui.user_id = u.id), ''), "
                "  IFNULL((SELECT GROUP_CONCAT(t.name ORDER BY t.sort SEPARATOR ',') "
                "          FROM user_interests a JOIN user_interests o ON o.tag_id = a.tag_id "
                "          JOIN interest_tags t ON t.id = a.tag_id "
                "          WHERE a.user_id = u.id AND o.user_id = %u), ''), "
                "  (SELECT COUNT(*) FROM recruits r WHERE r.author_id = u.id AND r.status = 'open') "
                "FROM users u LEFT JOIN departments d ON d.id = u.department_id "
                "LEFT JOIN colleges c ON c.id = d.college_id WHERE u.id = %u",
                u.id, hits[i].id);
        qr = db_query_buf(&sql);
        buf_free(&sql);
        row = qr ? mysql_fetch_row(qr) : NULL;
        if (row) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_putc(&b, '{');
            json_kv_int(&b, "id", hits[i].id);                          buf_putc(&b, ',');
            json_kv_str(&b, "nickname", row[0]);                        buf_putc(&b, ',');
            json_kv_str(&b, "department_name", row[1][0] ? row[1] : NULL); buf_putc(&b, ',');
            json_kv_str(&b, "college_name", row[2][0] ? row[2] : NULL); buf_putc(&b, ',');
            json_kv_str(&b, "bio", row[3]);                             buf_putc(&b, ',');
            buf_puts(&b, "\"tags\":");         write_csv_array(&b, row[4]); buf_putc(&b, ',');
            buf_puts(&b, "\"shared_tags\":");  write_csv_array(&b, row[5]); buf_putc(&b, ',');
            json_kv_int(&b, "open_recruits", atoll(row[6]));            buf_putc(&b, ',');
            json_kv_int(&b, "score", hits[i].score);                    buf_putc(&b, ',');
            ml_write_terms(&b, &hits[i]);
            buf_putc(&b, '}');
        }
        if (qr)
            mysql_free_result(qr);
    }
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 모집 게시판 */

/* 목록 행: id, title, status, need_people, deadline, created_at, author_id, nickname,
 *          dept, post_id, post_title, tags, comment_count, body 앞부분, like_count, 내가 눌렀는지
 * db_sqlf 의 첫 인자로 지금 회원 번호(%u, 비로그인은 0)를 준다. */
#define RECRUIT_SELECT \
    "SELECT r.id, r.title, r.status, r.need_people, " \
    "       IFNULL(DATE_FORMAT(r.deadline, '%%Y-%%m-%%d'), ''), " \
    "       DATE_FORMAT(r.created_at, '%%Y-%%m-%%d %%H:%%i'), r.author_id, u.nickname, " \
    "       IFNULL(d.name, ''), IFNULL(r.post_id, 0), IFNULL(p.title, ''), " \
    "       IFNULL((SELECT GROUP_CONCAT(t.name ORDER BY t.sort SEPARATOR ',') " \
    "               FROM recruit_tags rt JOIN interest_tags t ON t.id = rt.tag_id " \
    "               WHERE rt.recruit_id = r.id), ''), " \
    "       (SELECT COUNT(*) FROM recruit_comments rc WHERE rc.recruit_id = r.id), " \
    "       LEFT(REPLACE(REPLACE(r.body, '\\r', ''), '\\n', ' '), 120), " \
    "       (SELECT COUNT(*) FROM recruit_likes lk WHERE lk.recruit_id = r.id), " \
    "       EXISTS (SELECT 1 FROM recruit_likes lk WHERE lk.recruit_id = r.id AND lk.user_id = %u) " \
    "FROM recruits r JOIN users u ON u.id = r.author_id " \
    "LEFT JOIN departments d ON d.id = u.department_id " \
    "LEFT JOIN posts p ON p.id = r.post_id "

static void write_recruit_row(Buf *b, MYSQL_ROW row, const MlHit *match)
{
    buf_putc(b, '{');
    json_kv_int(b, "id", atoll(row[0]));                            buf_putc(b, ',');
    json_kv_str(b, "title", row[1]);                                buf_putc(b, ',');
    json_kv_str(b, "status", row[2]);                               buf_putc(b, ',');
    json_kv_int(b, "need_people", atoll(row[3]));                   buf_putc(b, ',');
    json_kv_str(b, "deadline", row[4][0] ? row[4] : NULL);          buf_putc(b, ',');
    json_kv_str(b, "created_at", row[5]);                           buf_putc(b, ',');
    json_kv_int(b, "author_id", atoll(row[6]));                     buf_putc(b, ',');
    json_kv_str(b, "author_nickname", row[7]);                      buf_putc(b, ',');
    json_kv_str(b, "author_department", row[8][0] ? row[8] : NULL); buf_putc(b, ',');
    json_kv_int(b, "post_id", atoll(row[9]));                       buf_putc(b, ',');
    json_kv_str(b, "post_title", row[10][0] ? row[10] : NULL);      buf_putc(b, ',');
    buf_puts(b, "\"tags\":");
    write_csv_array(b, row[11]);                                    buf_putc(b, ',');
    json_kv_int(b, "comment_count", atoll(row[12]));                buf_putc(b, ',');
    json_kv_str(b, "excerpt", row[13]);                             buf_putc(b, ',');
    json_kv_int(b, "like_count", atoll(row[14]));                   buf_putc(b, ',');
    json_kv_bool(b, "liked", atoi(row[15]));
    if (match) {
        buf_putc(b, ',');
        json_kv_int(b, "match", match->score);
        buf_putc(b, ',');
        ml_write_terms(b, match);
    }
    buf_putc(b, '}');
}

typedef struct { int idx; int score; } Order;

static int order_desc(const void *a, const void *b)
{
    const Order *x = (const Order *)a, *y = (const Order *)b;
    return y->score != x->score ? y->score - x->score : x->idx - y->idx;
}

static void handle_recruit_list(Request *req, Response *res)
{
    CurrentUser u;
    char status[12], tag[12], post[12], sort[12];
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW rows[RECRUIT_LIST_MAX];
    unsigned ids[RECRUIT_LIST_MAX];
    MlHit hits[RECRUIT_LIST_MAX];
    Order order[RECRUIT_LIST_MAX];
    int n = 0, i, by_match, scored = 0;

    auth_current(req, &u);
    req_query(req, "status", status, sizeof status);
    req_query(req, "tag", tag, sizeof tag);
    req_query(req, "post_id", post, sizeof post);
    req_query(req, "sort", sort, sizeof sort);

    buf_init(&sql);
    db_sqlf(&sql, RECRUIT_SELECT "WHERE 1 = 1", u.id);
    if (strcmp(status, "all") != 0)
        buf_puts(&sql, " AND r.status = 'open'");
    if (strtoul(tag, NULL, 10) > 0)
        db_sqlf(&sql, " AND EXISTS (SELECT 1 FROM recruit_tags x WHERE x.recruit_id = r.id "
                      "AND x.tag_id = %u)", (unsigned)strtoul(tag, NULL, 10));
    if (strtoul(post, NULL, 10) > 0)
        db_sqlf(&sql, " AND r.post_id = %u", (unsigned)strtoul(post, NULL, 10));
    db_sqlf(&sql, " ORDER BY r.status = 'closed', r.created_at DESC, r.id DESC LIMIT %d",
            RECRUIT_LIST_MAX);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "모집글을 읽을 수 없습니다.");
        return;
    }
    while (n < RECRUIT_LIST_MAX && (rows[n] = mysql_fetch_row(qr)) != NULL) {
        ids[n] = (unsigned)strtoul(rows[n][0], NULL, 10);
        order[n].idx = n;
        order[n].score = 0;
        n++;
    }

    /* 로그인한 학생에게는 모집글마다 나와 맞는 정도를 함께 준다. */
    if (u.id && !u.is_admin && n > 0)
        scored = ml_score_recruits(u.id, ids, n, hits) == 1;
    by_match = scored && strcmp(sort, "match") == 0;
    if (by_match) {
        for (i = 0; i < n; i++)
            order[i].score = hits[i].score;
        qsort(order, (size_t)n, sizeof *order, order_desc);
    }

    buf_init(&b);
    buf_puts(&b, "{");
    json_kv_bool(&b, "scored", scored);
    buf_puts(&b, ",\"recruits\":[");
    for (i = 0; i < n; i++) {
        int k = order[i].idx;
        if (i)
            buf_putc(&b, ',');
        write_recruit_row(&b, rows[k], scored ? &hits[k] : NULL);
    }
    buf_puts(&b, "]}");
    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

static int recruit_tags_save(unsigned rid, const unsigned *ids, int n)
{
    Buf sql;
    int ok, i;

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM recruit_tags WHERE recruit_id = %u", rid);
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    if (!ok || n <= 0)
        return ok;
    buf_init(&sql);
    db_sqlf(&sql, "INSERT IGNORE INTO recruit_tags (recruit_id, tag_id) "
                  "SELECT %u, id FROM interest_tags WHERE id IN (", rid);
    for (i = 0; i < n; i++)
        db_sqlf(&sql, i ? ",%u" : "%u", ids[i]);
    buf_putc(&sql, ')');
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    return ok;
}

static void handle_recruit_create(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    char *title = NULL, *body = NULL;
    const char *deadline;
    unsigned tag_ids[MAX_TAGS + 8], post_id;
    int ntags, need;
    Buf sql;
    unsigned long long rid;

    if (!auth_require(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    title = str_dup(json_str(in, "title", ""));
    body = str_dup(json_str(in, "body", ""));
    if (!title || !body) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        goto out;
    }
    str_trim(title);
    str_trim(body);
    deadline = clean_date(json_str(in, "deadline", NULL));
    need = (int)json_int(in, "need_people", 1);
    post_id = (unsigned)json_int(in, "post_id", 0);
    ntags = json_uint_array(json_get(in, "tag_ids"), tag_ids, (int)(sizeof tag_ids / sizeof tag_ids[0]));

    if (strlen(title) < 2 || strlen(title) > 600) {
        res_error(res, 400, "bad_title", "제목을 2자 이상 입력하세요.");
        goto out;
    }
    if (strlen(body) < 5) {
        res_error(res, 400, "bad_body", "어떤 팀원을 찾는지 내용을 적어 주세요.");
        goto out;
    }
    if (need < 1 || need > 20) {
        res_error(res, 400, "bad_need", "모집 인원은 1~20명으로 정하세요.");
        goto out;
    }
    if (ntags > MAX_TAGS) {
        res_error(res, 400, "too_many_tags", "키워드는 8개까지 고를 수 있습니다.");
        goto out;
    }
    if (post_id) {
        buf_init(&sql);
        db_sqlf(&sql, "SELECT COUNT(*) FROM posts WHERE id = %u", post_id);
        if (!db_scalar(sql.data, 0))
            post_id = 0;
        buf_free(&sql);
    }

    if (!db_begin()) {
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }
    buf_init(&sql);
    if (post_id)
        db_sqlf(&sql, "INSERT INTO recruits (author_id, post_id, title, body, need_people, deadline) "
                      "VALUES (%u, %u, %Q, %Q, %d, %Q)", u.id, post_id, title, body, need, deadline);
    else
        db_sqlf(&sql, "INSERT INTO recruits (author_id, title, body, need_people, deadline) "
                      "VALUES (%u, %Q, %Q, %d, %Q)", u.id, title, body, need, deadline);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "모집글을 저장할 수 없습니다.");
        goto out;
    }
    buf_free(&sql);
    rid = db_last_id();
    if (!recruit_tags_save((unsigned)rid, tag_ids, ntags) || !db_commit()) {
        db_rollback();
        res_error(res, 500, "db_error", "모집글을 저장할 수 없습니다.");
        goto out;
    }

    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "id", (long long)rid);
        buf_putc(&b, '}');
        res_json_buf(res, 201, &b);
        buf_free(&b);
    }

out:
    free(title);
    free(body);
    json_free(in);
}

/* 작성자 본인이거나 관리자인지. 글이 없으면 -1. */
static int recruit_owner_ok(unsigned rid, const CurrentUser *u)
{
    Buf sql;
    long long author;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT author_id FROM recruits WHERE id = %u", rid);
    author = db_scalar(sql.data, -1);
    buf_free(&sql);
    if (author < 0)
        return -1;
    return u->is_admin || (unsigned)author == u->id;
}

static void handle_recruit_detail(Request *req, Response *res, unsigned rid)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    MlHit match;
    int scored = 0, first = 1;

    auth_current(req, &u);

    buf_init(&sql);
    db_sqlf(&sql, RECRUIT_SELECT "WHERE r.id = %u", u.id, rid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (!row) {
        if (qr)
            mysql_free_result(qr);
        res_error(res, 404, "not_found", "모집글이 없습니다.");
        return;
    }

    if (u.id && !u.is_admin)
        scored = ml_score_recruits(u.id, &rid, 1, &match) == 1;

    buf_init(&b);
    buf_puts(&b, "{\"recruit\":");
    write_recruit_row(&b, row, scored ? &match : NULL);
    mysql_free_result(qr);

    /* 본문 전체 (목록 행에는 앞부분만 있다) */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT body FROM recruits WHERE id = %u", rid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    buf_puts(&b, ",\"body\":");
    json_write_str(&b, row ? row[0] : "");
    if (qr)
        mysql_free_result(qr);

    buf_putc(&b, ',');
    json_kv_bool(&b, "can_edit", u.id && recruit_owner_ok(rid, &u) == 1);

    buf_puts(&b, ",\"comments\":[");
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT c.id, c.body, DATE_FORMAT(c.created_at, '%%Y-%%m-%%d %%H:%%i'), "
            "       u.nickname, IFNULL(d.name, ''), c.author_id "
            "FROM recruit_comments c JOIN users u ON u.id = c.author_id "
            "LEFT JOIN departments d ON d.id = u.department_id "
            "WHERE c.recruit_id = %u ORDER BY c.created_at, c.id", rid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_putc(&b, '{');
            json_kv_int(&b, "id", atoll(row[0]));                         buf_putc(&b, ',');
            json_kv_str(&b, "body", row[1]);                              buf_putc(&b, ',');
            json_kv_str(&b, "created_at", row[2]);                        buf_putc(&b, ',');
            json_kv_str(&b, "author_nickname", row[3]);                   buf_putc(&b, ',');
            json_kv_str(&b, "author_department", row[4][0] ? row[4] : NULL); buf_putc(&b, ',');
            json_kv_int(&b, "author_id", atoll(row[5]));
            buf_putc(&b, '}');
        }
        mysql_free_result(qr);
    }
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

static void handle_recruit_status(Request *req, Response *res, unsigned rid)
{
    CurrentUser u;
    Json *in;
    const char *st;
    Buf sql;
    int own;

    if (!auth_require(req, res, &u))
        return;
    own = recruit_owner_ok(rid, &u);
    if (own < 0) { res_error(res, 404, "not_found", "모집글이 없습니다."); return; }
    if (!own)    { res_error(res, 403, "not_allowed", "작성자만 바꿀 수 있습니다."); return; }

    in = body_object(req, res);
    if (!in)
        return;
    st = json_str(in, "status", "");
    if (strcmp(st, "open") != 0 && strcmp(st, "closed") != 0) {
        json_free(in);
        res_error(res, 400, "bad_status", "상태는 open 또는 closed 입니다.");
        return;
    }
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE recruits SET status = '%s' WHERE id = %u",
            strcmp(st, "open") == 0 ? "open" : "closed", rid);
    json_free(in);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "상태를 바꿀 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 200, "{\"ok\":true}");
}

static void handle_recruit_delete(Request *req, Response *res, unsigned rid)
{
    CurrentUser u;
    Buf sql;
    int own;

    if (!auth_require(req, res, &u))
        return;
    own = recruit_owner_ok(rid, &u);
    if (own < 0) { res_error(res, 404, "not_found", "모집글이 없습니다."); return; }
    if (!own)    { res_error(res, 403, "not_allowed", "작성자만 지울 수 있습니다."); return; }

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM recruits WHERE id = %u", rid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "모집글을 지울 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 200, "{\"ok\":true}");
}

static void handle_recruit_comment(Request *req, Response *res, unsigned rid)
{
    CurrentUser u;
    Json *in;
    char *body;
    Buf sql;

    if (!auth_require(req, res, &u))
        return;
    if (recruit_owner_ok(rid, &u) < 0) {
        res_error(res, 404, "not_found", "모집글이 없습니다.");
        return;
    }
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
    if (!body[0] || strlen(body) > 3000) {
        free(body);
        res_error(res, 400, "bad_comment", "댓글은 1~1000자로 써 주세요.");
        return;
    }
    utf8_cut(body, 3000);

    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO recruit_comments (recruit_id, author_id, body) VALUES (%u, %u, %Q)",
            rid, u.id, body);
    free(body);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "댓글을 저장할 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 201, "{\"ok\":true}");
}

static void handle_recruit_comment_delete(Request *req, Response *res, unsigned cid)
{
    CurrentUser u;
    Buf sql;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    if (u.is_admin)
        db_sqlf(&sql, "DELETE FROM recruit_comments WHERE id = %u", cid);
    else
        db_sqlf(&sql, "DELETE FROM recruit_comments WHERE id = %u AND author_id = %u", cid, u.id);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "댓글을 지울 수 없습니다.");
        return;
    }
    buf_free(&sql);
    if (db_affected() == 0) {
        res_error(res, 403, "not_allowed", "본인 댓글만 지울 수 있습니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

/* 마음에 들어요 누르기(on=1) / 취소(on=0). 여러 번 눌러도 결과는 같다. */
static void handle_recruit_like(Request *req, Response *res, unsigned rid, int on)
{
    CurrentUser u;
    Buf sql, b;
    long long author;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    db_sqlf(&sql, "SELECT author_id FROM recruits WHERE id = %u", rid);
    author = db_scalar(sql.data, -1);
    buf_free(&sql);
    if (author < 0) {
        res_error(res, 404, "not_found", "모집글이 없습니다.");
        return;
    }
    if (on && (unsigned)author == u.id) {
        res_error(res, 400, "own_recruit", "내 모집글에는 누를 수 없습니다.");
        return;
    }

    buf_init(&sql);
    if (on)
        db_sqlf(&sql, "INSERT IGNORE INTO recruit_likes (recruit_id, user_id) VALUES (%u, %u)", rid, u.id);
    else
        db_sqlf(&sql, "DELETE FROM recruit_likes WHERE recruit_id = %u AND user_id = %u", rid, u.id);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "저장할 수 없습니다.");
        return;
    }
    buf_free(&sql);

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM recruit_likes WHERE recruit_id = %u", rid);
    buf_init(&b);
    buf_putc(&b, '{');
    json_kv_bool(&b, "liked", on);                            buf_putc(&b, ',');
    json_kv_int(&b, "like_count", db_scalar(sql.data, 0));
    buf_putc(&b, '}');
    buf_free(&sql);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

int route_community(Request *req, Response *res)
{
    unsigned id;
    const char *m = req->method;

    if (req_is(req, "GET", "/api/tags"))              { handle_tags(res);               return 1; }
    if (req_is(req, "GET", "/api/profile"))           { handle_profile_get(req, res);   return 1; }
    if (req_is(req, "PUT", "/api/profile"))           { handle_profile_put(req, res);   return 1; }
    if (req_is(req, "GET", "/api/members/matches"))   { handle_matches(req, res);       return 1; }
    if (req_is(req, "GET", "/api/recruits"))          { handle_recruit_list(req, res);  return 1; }
    if (req_is(req, "POST", "/api/recruits"))         { handle_recruit_create(req, res); return 1; }

    if (!strcmp(m, "PUT") && path_id(req->path, "/api/recruits/", "/status", &id)) {
        handle_recruit_status(req, res, id);
        return 1;
    }
    if ((!strcmp(m, "POST") || !strcmp(m, "DELETE")) &&
        path_id(req->path, "/api/recruits/", "/like", &id)) {
        handle_recruit_like(req, res, id, !strcmp(m, "POST"));
        return 1;
    }
    if (!strcmp(m, "POST") && path_id(req->path, "/api/recruits/", "/comments", &id)) {
        handle_recruit_comment(req, res, id);
        return 1;
    }
    if (!strcmp(m, "GET") && path_id(req->path, "/api/recruits/", NULL, &id)) {
        handle_recruit_detail(req, res, id);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/recruits/", NULL, &id)) {
        handle_recruit_delete(req, res, id);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/recruit-comments/", NULL, &id)) {
        handle_recruit_comment_delete(req, res, id);
        return 1;
    }
    return 0;
}
