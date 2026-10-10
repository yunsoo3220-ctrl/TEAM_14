/* 공모전 게시물 조회와 댓글.
 *
 * 댓글 권한의 핵심 규칙: 관리자가 게시물에 체크한 학과(post_departments)에
 * 소속된 사용자만 댓글을 쓸 수 있다. 관리자는 항상 쓸 수 있다.
 *
 * 이 파일이 담당하는 API:
 *   GET    /api/posts                  게시물 목록 (?department_id=, ?kind=, ?mine=1 로 거르기)
 *   GET    /api/posts/{id}             게시물 상세 + 댓글 + 권한 + AI/모델 분석 + 관련 모집글
 *   POST   /api/posts/{id}/comments    댓글 작성 {"body":"..."}
 *   DELETE /api/comments/{id}          댓글 삭제 (본인 또는 관리자)
 *
 * 관련 테이블:
 *   posts             게시물 (kind = contest/hackathon/etc, author_id 가 NULL 이면 자동 수집된 학교 공지)
 *   post_departments  (게시물, 대상 학과) - 관리자가 체크한 "댓글 가능 학과"
 *   comments          댓글
 */
#include "api.h"
#include "db.h"
#include "ml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMENT_MAX_LEN 1000   /* 댓글 최대 길이 (바이트) */

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

    if (!department_id)        /* 학과 없는 사용자는 어떤 대상 학과에도 해당하지 않는다 */
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
 *   - 대상 학과 지정됨 -> 체크된 학과 소속만 가능  <- 과제의 핵심 제약
 *
 * 이 함수는 화면 표시(목록·상세의 can_comment)와 실제 저장(handle_comment_create)
 * 양쪽에서 쓰인다. 화면에서 버튼을 숨기는 것만으로는 부족하고, 서버가 저장 직전에
 * 다시 검사해야 개발자 도구로 요청을 직접 보내는 우회를 막을 수 있다. */
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

/* GET /api/posts?department_id=3&kind=contest&mine=1
 * 로그인하지 않아도 볼 수 있다(auth_current 결과를 검사하지 않음). */
static void handle_post_list(Request *req, Response *res)
{
    CurrentUser u;
    char dept[16], kind[20], only_mine[8];   /* 쿼리 파라미터들 */
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    auth_current(req, &u);   /* 비로그인이어도 계속 (u.id == 0) */

    req_query(req, "department_id", dept, sizeof dept);
    req_query(req, "kind", kind, sizeof kind);
    req_query(req, "mine", only_mine, sizeof only_mine);

    /* 기본 SELECT. 여기는 db_sqlf 가 아니라 buf_puts 로 넣으므로 '%Y' 의 % 를 두 번 쓸 필요가 없다.
     * '\\r' 은 C 문자열에서 역슬래시 하나가 되어, MySQL 에는 '\r'(CR 문자) 로 전달된다.
     * 마지막의 "WHERE 1 = 1" 은 항상 참인 조건으로, 뒤에 " AND ..." 조건을 몇 개 붙이든
     * 문법이 맞도록 하는 동적 SQL 관용구다. */
    buf_init(&sql);
    buf_puts(&sql,
             /* 주최·마감일이 비어 있으면 AI 가 본문에서 찾은 값으로 채운다. */
             "SELECT p.id, p.kind, p.title, IFNULL(p.host, IFNULL(a.host, '')), "
             "       IFNULL(DATE_FORMAT(IFNULL(p.deadline, a.deadline), '%Y-%m-%d'), ''), "
             "       p.need_people, "
             "       p.view_count, DATE_FORMAT(p.created_at, '%Y-%m-%d %H:%i'), "
             "       IFNULL(p.source_url, ''), "
             /* 작성자가 없는 글은 학교 공지에서 자동으로 올라온 글이다. */
             "       IFNULL(u.nickname, IF(p.source_url IS NULL, '(탈퇴)', '학교 공지')), "
             "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id), "
             /* 대상 학과 이름들을 "컴퓨터공학과, 소프트웨어학과" 처럼 한 문자열로 합친다 */
             "       (SELECT GROUP_CONCAT(d.name ORDER BY d.sort SEPARATOR ', ') "
             "          FROM post_departments pd JOIN departments d ON d.id = pd.department_id "
             "         WHERE pd.post_id = p.id), "
             /* 마지막 식: 관리자가 마감일을 안 넣었고 AI 가 찾은 값이 있으면 1 (화면에 "AI 추정" 표시) */
             "       a.summary, a.tags, (p.deadline IS NULL AND a.deadline IS NOT NULL), "
             /* AI 요약이 없을 때 목록에 보여줄 본문 앞부분 (LEFT 는 글자 단위) */
             "       LEFT(REPLACE(REPLACE(p.body, '\\r', ''), '\\n', ' '), 160) "
             "FROM posts p LEFT JOIN users u ON u.id = p.author_id "
             "LEFT JOIN post_ai a ON a.post_id = p.id "
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

    /* 종류 필터는 허용 목록(화이트리스트)에 있는 값만 받는다 */
    if (strcmp(kind, "contest") == 0 || strcmp(kind, "hackathon") == 0 ||
        strcmp(kind, "etc") == 0)
        db_sqlf(&sql, " AND p.kind = %Q", kind);

    /* 최신순, 최대 200개 (같은 시각이면 id 큰 것 먼저 - 정렬 결과를 항상 일정하게) */
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
        const char *why;   /* can_comment 의 사유 (목록에서는 쓰지 않음) */

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
        json_kv_str(&b, "departments", row[11] ? row[11] : "");       buf_putc(&b, ',');   /* GROUP_CONCAT 결과가 없으면 NULL */
        json_kv_str(&b, "summary", row[12]);                          buf_putc(&b, ',');
        /* tags 는 ai.c 가 json_write_str 로 만든 배열이라 그대로 싣는다. */
        buf_puts(&b, "\"tags\":");
        buf_puts(&b, row[13] && row[13][0] == '[' ? row[13] : "[]");  buf_putc(&b, ',');
        json_kv_bool(&b, "deadline_by_ai", row[14] && row[14][0] == '1'); buf_putc(&b, ',');
        json_kv_str(&b, "excerpt", row[15]);                          buf_putc(&b, ',');
        /* 목록에서도 내가 댓글을 쓸 수 있는 글인지 바로 보여준다.
         * (글마다 질의 2개가 더 나가지만 최대 200개라 감당할 만하다) */
        json_kv_bool(&b, "can_comment", can_comment(&u, pid, &why));
        buf_putc(&b, '}');
    }
    buf_puts(&b, "]}");

    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 게시물 상세 */

/* 게시물의 대상 학과 목록 [{"id":..,"name":..,"college_name":..}, ...] */
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

/* AI 분석 결과. 아직 분석하지 않은 글이면 null.
 * 형태: {"summary":..,"tags":[..],"host":..,"deadline":..,"model":..,"analyzed_at":..,
 *        "departments":[{"id":..,"score":..,"reason":..}, ...]} */
static void write_post_ai(Buf *b, unsigned post_id)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    /* 1) 게시물 단위 분석 결과 */
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT summary, tags, IFNULL(host, ''), "
            "       IFNULL(DATE_FORMAT(deadline, '%%Y-%%m-%%d'), ''), model, "
            "       DATE_FORMAT(analyzed_at, '%%Y-%%m-%%d %%H:%%i') "
            "FROM post_ai WHERE post_id = %u", post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    row = qr ? mysql_fetch_row(qr) : NULL;
    if (!row) {
        if (qr)
            mysql_free_result(qr);
        buf_puts(b, "null");
        return;
    }

    buf_putc(b, '{');
    json_kv_str(b, "summary", row[0]);                               buf_putc(b, ',');
    buf_puts(b, "\"tags\":");
    buf_puts(b, row[1] && row[1][0] == '[' ? row[1] : "[]");         buf_putc(b, ',');
    json_kv_str(b, "host", row[2][0] ? row[2] : NULL);               buf_putc(b, ',');
    json_kv_str(b, "deadline", row[3][0] ? row[3] : NULL);           buf_putc(b, ',');
    json_kv_str(b, "model", row[4]);                                 buf_putc(b, ',');
    json_kv_str(b, "analyzed_at", row[5]);                           buf_putc(b, ',');
    mysql_free_result(qr);   /* row 를 다 쓴 뒤에 해제 (row 는 결과셋 메모리를 가리킨다) */

    /* 2) 학과별 관련도 점수 (높은 순) */
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT d.id, d.name, c.name, ad.score, ad.reason "
            "FROM post_ai_departments ad "
            "JOIN departments d ON d.id = ad.department_id "
            "JOIN colleges c ON c.id = d.college_id "
            "WHERE ad.post_id = %u ORDER BY ad.score DESC, c.sort, d.sort", post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    buf_puts(b, "\"departments\":[");
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(b, ',');
            first = 0;
            buf_putc(b, '{');
            json_kv_int(b, "id", atoll(row[0]));         buf_putc(b, ',');
            json_kv_str(b, "name", row[1]);              buf_putc(b, ',');
            json_kv_str(b, "college_name", row[2]);      buf_putc(b, ',');
            json_kv_int(b, "score", atoll(row[3]));      buf_putc(b, ',');
            json_kv_str(b, "reason", row[4]);
            buf_putc(b, '}');
        }
        mysql_free_result(qr);
    }
    buf_puts(b, "]}");
}

/* 자체 모델이 고른 관련 학과 (점수 순 최대 5개)
 * 25점 미만은 관련이 약하다고 보고 뺀다. prob 은 Platt 보정한 "맞을 확률",
 * terms 는 점수의 근거가 된 키워드다 (ml.h 참고). */
static void write_post_related(Buf *b, unsigned post_id)
{
    MlHit hits[5];
    int n = ml_rank_departments(post_id, 25, hits, 5, NULL);   /* 모델을 만들 수 없으면 -1 → 반복 없음 */
    int i, first = 1;

    buf_putc(b, '[');
    for (i = 0; i < n; i++) {
        Buf sql;
        MYSQL_RES *qr;
        MYSQL_ROW row;

        /* 모델은 학과 id 만 주므로 이름은 DB 에서 찾는다 */
        buf_init(&sql);
        db_sqlf(&sql, "SELECT d.name, c.name FROM departments d "
                      "JOIN colleges c ON c.id = d.college_id WHERE d.id = %u", hits[i].id);
        qr = db_query_buf(&sql);
        buf_free(&sql);
        row = qr ? mysql_fetch_row(qr) : NULL;
        if (row) {   /* 학습 후 학과가 삭제됐을 수도 있으므로 있는 것만 */
            if (!first)
                buf_putc(b, ',');
            first = 0;
            buf_putc(b, '{');
            json_kv_int(b, "id", hits[i].id);           buf_putc(b, ',');
            json_kv_str(b, "name", row[0]);             buf_putc(b, ',');
            json_kv_str(b, "college_name", row[1]);     buf_putc(b, ',');
            json_kv_int(b, "score", hits[i].score);     buf_putc(b, ',');
            buf_printf(b, "\"prob\":%.4f,", hits[i].prob);
            ml_write_terms(b, &hits[i]);
            buf_putc(b, '}');
        }
        if (qr)
            mysql_free_result(qr);
    }
    buf_putc(b, ']');
}

/* 이 공모전으로 팀원을 모집하는 글 (모집 중인 것 먼저)
 * ORDER BY r.status = 'closed' : 마감(1)이 모집 중(0)보다 뒤로 간다 */
static void write_post_recruits(Buf *b, unsigned post_id)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT r.id, r.title, r.status, r.need_people, u.nickname, IFNULL(d.name, ''), "
            "       (SELECT COUNT(*) FROM recruit_comments c WHERE c.recruit_id = r.id) "
            "FROM recruits r JOIN users u ON u.id = r.author_id "
            "LEFT JOIN departments d ON d.id = u.department_id "
            "WHERE r.post_id = %u ORDER BY r.status = 'closed', r.created_at DESC LIMIT 10",
            post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    buf_putc(b, '[');
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(b, ',');
            first = 0;
            buf_putc(b, '{');
            json_kv_int(b, "id", atoll(row[0]));                        buf_putc(b, ',');
            json_kv_str(b, "title", row[1]);                            buf_putc(b, ',');
            json_kv_str(b, "status", row[2]);                           buf_putc(b, ',');
            json_kv_int(b, "need_people", atoll(row[3]));               buf_putc(b, ',');
            json_kv_str(b, "author_nickname", row[4]);                  buf_putc(b, ',');
            json_kv_str(b, "author_department", row[5][0] ? row[5] : NULL); buf_putc(b, ',');
            json_kv_int(b, "comment_count", atoll(row[6]));
            buf_putc(b, '}');
        }
        mysql_free_result(qr);
    }
    buf_putc(b, ']');
}

/* 게시물의 댓글 목록 (오래된 순). 탈퇴한 작성자는 "(탈퇴)" 로 표시 */
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
            /* 프론트엔드가 "내 댓글이면 삭제 버튼 표시" 를 판단하는 데 쓴다 */
            json_kv_int(b, "author_id", atoll(row[5]));
            buf_putc(b, '}');
        }
        mysql_free_result(qr);
    }
    buf_putc(b, ']');
}

/* GET /api/posts/{id}
 * 응답: {"post":{...,"departments":[..],"ai":{..}|null,"related_departments":[..],"recruits":[..]},
 *        "comments":[..], "permission":{"can_comment":bool,"reason":"..."}} */
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

    /* qr(첫 결과셋)을 열어 둔 채 아래 write_* 함수들이 다른 질의를 실행한다.
     * mysql_store_result 로 결과를 이미 메모리에 받아 두었기 때문에 가능하다. */
    buf_init(&b);
    buf_puts(&b, "{\"post\":{");
    json_kv_int(&b, "id", atoll(row[0]));                            buf_putc(&b, ',');
    json_kv_str(&b, "kind", row[1]);                                 buf_putc(&b, ',');
    json_kv_str(&b, "title", row[2]);                                buf_putc(&b, ',');
    json_kv_str(&b, "body", row[3]);                                 buf_putc(&b, ',');
    json_kv_str(&b, "host", row[4][0] ? row[4] : NULL);              buf_putc(&b, ',');
    json_kv_str(&b, "deadline", row[5][0] ? row[5] : NULL);          buf_putc(&b, ',');
    json_kv_int(&b, "need_people", atoll(row[6]));                   buf_putc(&b, ',');
    json_kv_int(&b, "view_count", atoll(row[7]) + 1);                buf_putc(&b, ',');   /* 방금 올린 1 을 반영 */
    json_kv_str(&b, "created_at", row[8]);                           buf_putc(&b, ',');
    json_kv_str(&b, "source_url", row[9][0] ? row[9] : NULL);        buf_putc(&b, ',');
    json_kv_str(&b, "author_nickname", row[10]);                     buf_putc(&b, ',');
    buf_puts(&b, "\"departments\":");
    write_post_departments(&b, post_id);
    buf_puts(&b, ",\"ai\":");
    write_post_ai(&b, post_id);
    buf_puts(&b, ",\"related_departments\":");
    write_post_related(&b, post_id);
    buf_puts(&b, ",\"recruits\":");
    write_post_recruits(&b, post_id);
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

/* POST /api/posts/{id}/comments {"body":"..."} */
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

    /* 존재하지 않는 게시물에 댓글을 달 수 없게 먼저 확인 */
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
        /* json_str 이 돌려준 문자열은 트리 소유라 직접 고칠 수 없으므로 복사본을 다듬는다 */
        char *trimmed = str_dup(body);
        if (!trimmed) {
            json_free(in);
            res_error(res, 500, "oom", "메모리가 부족합니다.");
            return;
        }
        str_trim(trimmed);   /* 공백만 있는 댓글을 빈 댓글로 판정하기 위해 */

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
                post_id, u.id, trimmed);   /* 본문은 %Q 로 이스케이프 */
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

/* 작성자 본인 또는 관리자만 삭제할 수 있다.
 * DELETE /api/comments/{id}
 * 일반 사용자는 WHERE 에 author_id 조건을 붙여, 남의 댓글이면 삭제되는 행이 0 이 된다.
 * 그래서 "먼저 조회해 작성자 확인 → 삭제" 두 단계가 아니라 한 문장으로 권한 검사까지 끝난다. */
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

    /* 지워진 행이 없으면: 남의 댓글이거나 이미 없는 댓글 */
    if (db_affected() == 0) {
        res_error(res, 403, "not_allowed", "본인 댓글만 삭제할 수 있습니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

/* 게시물·댓글 경로 라우터 */
int route_posts(Request *req, Response *res)
{
    unsigned id;   /* 경로에서 꺼낸 숫자 id */

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
