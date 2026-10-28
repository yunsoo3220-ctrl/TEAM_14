/* 전공책 장터 (중고 전공책 사고팔기)
 *
 *   GET    /api/books                     책 목록 ?q=검색어&dept=학과id&status=on|all&mine=1&sort=new|price_asc|price_desc
 *                                         ?reco=학과id 면 판매 중인 책을 그 학과와 관련 있는 순으로 (자체 모델, 학과별 추천)
 *   POST   /api/books                     판매 글 쓰기 {title, author?, course?, department_id?, price,
 *                                                       condition, description?, photos:["data:image/jpeg;base64,..."]} (로그인)
 *   GET    /api/books/{id}                책 + 사진 번호 + 문의 댓글
 *   PUT    /api/books/{id}/status         판매 상태 {status:"selling"|"reserved"|"sold"} (판매자·관리자)
 *   DELETE /api/books/{id}                판매 글 삭제 (판매자·관리자)
 *   POST   /api/books/{id}/comments       문의 댓글 {body} (로그인)
 *   DELETE /api/book-comments/{id}        댓글 삭제 (작성자·관리자)
 *   GET    /api/book-photos/{id}          사진 파일 (이미지 그대로)
 *
 * 사진은 브라우저가 미리 줄여(긴 변 1280px JPEG) base64 data URL 로 보내고,
 * 서버는 MySQL 의 FROM_BASE64 로 풀어 book_photos.data(MEDIUMBLOB)에 저장한다.
 * 받는 형식은 JPEG·PNG·WEBP 뿐이고, 내용의 첫 바이트(파일 서명)까지 확인한다.
 * 다른 회원에게 보이는 것은 표시 이름·학과뿐이다 (api_community.c 와 같은 원칙).
 * 판매 완료한 책은 30일 뒤 자동으로 지워진다 (cleanup.c).
 *
 * 관련 테이블:
 *   books          판매 글 (seller_id = 판매자)
 *   book_photos    책 사진 (한 권에 최대 BOOK_PHOTO_MAX 장)
 *   book_comments  문의 댓글
 */
#include "api.h"
#include "db.h"
#include "ml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOK_LIST_MAX     200               /* 목록 최대 개수 */
#define BOOK_PHOTO_MAX    5                 /* 한 권에 올릴 수 있는 사진 수 */
#define BOOK_PHOTO_B64MAX (1400 * 1024)     /* 사진 한 장의 base64 길이 상한 (원본 약 1MB) */
#define BOOK_PRICE_MAX    1000000           /* 가격 상한 (원) */

/* ------------------------------------------------------------ 도우미 */

/* s 를 maxbytes 이하로 자르되 UTF-8 글자 중간에서 자르지 않는다 (api_community.c 와 동일) */
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

/* 책 상태 이름을 코드 상수로. 모르는 값이면 NULL. SQL 에는 이 상수만 넣는다. */
static const char *condition_code(const char *s)
{
    if (!s)                    return NULL;
    if (!strcmp(s, "new"))     return "new";      /* 새 책 */
    if (!strcmp(s, "good"))    return "good";     /* 깨끗함 */
    if (!strcmp(s, "fair"))    return "fair";     /* 필기 조금 */
    if (!strcmp(s, "poor"))    return "poor";     /* 필기 많음 */
    return NULL;
}

static const char *status_code(const char *s)
{
    if (!s)                      return NULL;
    if (!strcmp(s, "selling"))   return "selling";    /* 판매 중 */
    if (!strcmp(s, "reserved"))  return "reserved";   /* 예약 중 */
    if (!strcmp(s, "sold"))      return "sold";       /* 판매 완료 */
    return NULL;
}

/* "data:image/jpeg;base64,/9j/..." 를 검사해 MIME 과 base64 본문 위치를 돌려준다.
 * 형식·글자·길이·파일 서명 중 하나라도 맞지 않으면 NULL.
 * base64 문자열의 앞부분은 파일의 첫 바이트를 그대로 나타내므로 풀지 않고도 서명을 볼 수 있다.
 *   JPEG FF D8 FF → "/9j/",  PNG 89 50 4E 47 → "iVBORw0KGgo",  WEBP "RIFF" → "UklGR" */
static const char *photo_payload(const char *url, const char **mime)
{
    static const struct { const char *prefix, *mime, *magic; } kinds[] = {
        { "data:image/jpeg;base64,", "image/jpeg", "/9j/" },
        { "data:image/png;base64,",  "image/png",  "iVBORw0KGgo" },
        { "data:image/webp;base64,", "image/webp", "UklGR" },
    };
    size_t i, n;
    const char *p;

    if (!url)
        return NULL;
    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        size_t plen = strlen(kinds[i].prefix);
        if (strncmp(url, kinds[i].prefix, plen) != 0)
            continue;
        p = url + plen;
        if (strncmp(p, kinds[i].magic, strlen(kinds[i].magic)) != 0)
            return NULL;
        n = strlen(p);
        if (n < 16 || n > BOOK_PHOTO_B64MAX || n % 4 != 0)
            return NULL;
        /* base64 글자만 (SQL 에는 %Q 로 넣지만, FROM_BASE64 가 엉뚱한 값을 만들지 않게 먼저 거른다) */
        for (; *p; p++) {
            char c = *p;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '='))
                return NULL;
        }
        *mime = kinds[i].mime;
        return url + plen;
    }
    return NULL;
}

/* 판매자 id. 책이 없으면 -1. */
static long long book_seller(unsigned bid)
{
    Buf sql;
    long long seller;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT seller_id FROM books WHERE id = %u", bid);
    seller = db_scalar(sql.data, -1);
    buf_free(&sql);
    return seller;
}

/* 판매자이거나 관리자인지. 반환: 1 = 관리 권한 있음, 0 = 없음, -1 = 책 없음 */
static int book_manage_ok(unsigned bid, const CurrentUser *u)
{
    long long seller = book_seller(bid);

    if (seller < 0)
        return -1;
    return u->is_admin || (unsigned)seller == u->id;
}

/* ------------------------------------------------------------ 목록 */

/* 목록 행: id, title, author, course, 과목 학과 이름, price, condition, status, created_at,
 *          seller_id, 판매자 표시 이름, 판매자 학과, 대표 사진 id(0 = 없음), 사진 수, 댓글 수, 설명 앞부분, 학과 id,
 *          설명 전체 (학과별 추천에서 쓴다) */
#define BOOK_SELECT \
    "SELECT b.id, b.title, b.author, b.course, IFNULL(d.name, ''), b.price, b.book_condition, b.status, " \
    "       DATE_FORMAT(b.created_at, '%%Y-%%m-%%d %%H:%%i'), b.seller_id, u.nickname, IFNULL(sd.name, ''), " \
    "       IFNULL((SELECT p.id FROM book_photos p WHERE p.book_id = b.id ORDER BY p.sort_order, p.id LIMIT 1), 0), " \
    "       (SELECT COUNT(*) FROM book_photos p WHERE p.book_id = b.id), " \
    "       (SELECT COUNT(*) FROM book_comments c WHERE c.book_id = b.id), " \
    "       LEFT(REPLACE(REPLACE(b.description, '\\r', ''), '\\n', ' '), 100), IFNULL(b.department_id, 0), " \
    "       b.description " \
    "FROM books b JOIN users u ON u.id = b.seller_id " \
    "LEFT JOIN departments d ON d.id = b.department_id " \
    "LEFT JOIN departments sd ON sd.id = u.department_id "

/* BOOK_SELECT 결과 한 행을 JSON 객체로 쓴다.
 * hit 가 있으면(학과별 추천) 학과 관련도 점수와 근거 키워드도 덧붙인다. */
static void write_book_row(Buf *b, MYSQL_ROW row, const MlHit *hit)
{
    buf_putc(b, '{');
    json_kv_int(b, "id", atoll(row[0]));                           buf_putc(b, ',');
    json_kv_str(b, "title", row[1]);                               buf_putc(b, ',');
    json_kv_str(b, "author", row[2]);                              buf_putc(b, ',');
    json_kv_str(b, "course", row[3]);                              buf_putc(b, ',');
    json_kv_str(b, "department", row[4][0] ? row[4] : NULL);       buf_putc(b, ',');
    json_kv_int(b, "price", atoll(row[5]));                        buf_putc(b, ',');
    json_kv_str(b, "condition", row[6]);                           buf_putc(b, ',');
    json_kv_str(b, "status", row[7]);                              buf_putc(b, ',');
    json_kv_str(b, "created_at", row[8]);                          buf_putc(b, ',');
    json_kv_int(b, "seller_id", atoll(row[9]));                    buf_putc(b, ',');
    json_kv_str(b, "seller_nickname", row[10]);                    buf_putc(b, ',');
    json_kv_str(b, "seller_department", row[11][0] ? row[11] : NULL); buf_putc(b, ',');
    json_kv_int(b, "photo_id", atoll(row[12]));                    buf_putc(b, ',');   /* 0 = 사진 없음 */
    json_kv_int(b, "photo_count", atoll(row[13]));                 buf_putc(b, ',');
    json_kv_int(b, "comment_count", atoll(row[14]));               buf_putc(b, ',');
    json_kv_str(b, "excerpt", row[15]);                            buf_putc(b, ',');
    json_kv_int(b, "department_id", atoll(row[16]));
    if (hit) {
        buf_putc(b, ',');
        json_kv_int(b, "score", hit->score);
        buf_putc(b, ',');
        ml_write_terms(b, hit);
    }
    buf_putc(b, '}');
}

/* 학과별 추천: rows 의 책을 학과 dept 와 관련 있는 순으로 b 에 쓴다 (ML_MIN_SCORE 미만은 뺀다).
 * 모델에 넘기는 글 = 제목: 책 제목, 본문: 과목 · 저자 · 설명 · 학과 이름. 모델을 만들 수 없으면 0.
 * (api_study.c 의 write_reco_studies 와 같은 틀) */
static int write_reco_books(Buf *b, MYSQL_ROW *rows, int n, unsigned dept)
{
    unsigned *ids = (unsigned *)calloc((size_t)n, sizeof *ids);
    const char **titles = (const char **)calloc((size_t)n, sizeof *titles);
    char **bodies = (char **)calloc((size_t)n, sizeof *bodies);
    MlHit *hits = (MlHit *)calloc((size_t)n, sizeof *hits);
    int i, k, m = -1;

    if (ids && titles && bodies && hits) {
        for (i = 0; i < n; i++) {
            Buf t;
            buf_init(&t);
            buf_printf(&t, "%s\n%s\n%s\n%s", rows[i][3], rows[i][2], rows[i][17], rows[i][4]);
            ids[i] = (unsigned)strtoul(rows[i][0], NULL, 10);
            titles[i] = rows[i][1];
            bodies[i] = t.data;   /* 메모리가 모자라 NULL 이면 모델이 빈 글로 본다 */
        }
        m = ml_rank_texts(dept, ids, titles, (const char *const *)bodies, n, ML_MIN_SCORE, hits, NULL);
        for (k = 0; k < m; k++)
            for (i = 0; i < n; i++)
                if (ids[i] == hits[k].id) {
                    if (k)
                        buf_putc(b, ',');
                    write_book_row(b, rows[i], &hits[k]);
                    break;
                }
        for (i = 0; i < n; i++)
            free(bodies[i]);
    }
    free(ids);
    free(titles);
    free(bodies);
    free(hits);
    return m >= 0;
}

/* GET /api/books?q=&dept=&status=on|all&mine=1&sort=new|price_asc|price_desc&reco=학과id
 * status 기본값 on = 판매 중·예약 중만 (판매 완료는 all 일 때만 보인다)
 * reco 가 있으면 다른 조건으로 고른 책을 그 학과와 관련 있는 순으로 다시 늘어놓는다 (학과별 추천) */
static void handle_book_list(Request *req, Response *res)
{
    CurrentUser u;
    char q[128] = "", dept[12] = "", status[8] = "", mine[4] = "", sort[16] = "", reco[12] = "";
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW rows[BOOK_LIST_MAX];   /* 결과 행들 (고른 정렬 순) */
    int n = 0, i;
    unsigned dept_id, reco_id;

    auth_current(req, &u);
    req_query(req, "q", q, sizeof q);
    req_query(req, "dept", dept, sizeof dept);
    req_query(req, "status", status, sizeof status);
    req_query(req, "mine", mine, sizeof mine);
    req_query(req, "sort", sort, sizeof sort);
    req_query(req, "reco", reco, sizeof reco);
    str_trim(q);
    dept_id = (unsigned)strtoul(dept, NULL, 10);
    reco_id = (unsigned)strtoul(reco, NULL, 10);

    buf_init(&sql);
    db_sqlf(&sql, BOOK_SELECT "WHERE 1 = 1 ");   /* db_sqlf 가 BOOK_SELECT 안의 %% 를 % 로 바꾼다 */
    if (strcmp(status, "all") != 0)
        buf_puts(&sql, "AND b.status <> 'sold' ");
    if (dept_id)
        db_sqlf(&sql, "AND b.department_id = %u ", dept_id);
    if (mine[0] == '1' && u.id)
        db_sqlf(&sql, "AND b.seller_id = %u ", u.id);
    if (q[0]) {
        /* LIKE 의 특수 문자(% _ \)는 글자 그대로 찾도록 \ 를 앞에 붙인다 (api_study.c 와 같은 방식) */
        Buf pat;
        const char *p;
        buf_init(&pat);
        buf_putc(&pat, '%');
        for (p = q; *p; p++) {
            if (*p == '%' || *p == '_' || *p == '\\')
                buf_putc(&pat, '\\');
            buf_putc(&pat, *p);
        }
        buf_putc(&pat, '%');
        db_sqlf(&sql, "AND (b.title LIKE %Q OR b.author LIKE %Q OR b.course LIKE %Q) ",
                pat.data, pat.data, pat.data);
        buf_free(&pat);
    }
    /* 판매 완료는 언제나 뒤로, 그 안에서 고른 정렬 */
    buf_puts(&sql, "ORDER BY b.status = 'sold', ");
    if (!strcmp(sort, "price_asc"))
        buf_puts(&sql, "b.price, ");
    else if (!strcmp(sort, "price_desc"))
        buf_puts(&sql, "b.price DESC, ");
    db_sqlf(&sql, "b.created_at DESC, b.id DESC LIMIT %d", BOOK_LIST_MAX);

    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "책 목록을 읽을 수 없습니다.");
        return;
    }

    while (n < BOOK_LIST_MAX && (rows[n] = mysql_fetch_row(qr)) != NULL)
        n++;

    buf_init(&b);
    buf_puts(&b, "{\"books\":[");
    if (reco_id && n > 0) {
        if (!write_reco_books(&b, rows, n, reco_id)) {
            buf_free(&b);
            mysql_free_result(qr);
            res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
            return;
        }
    } else {
        for (i = 0; i < n; i++) {
            if (i)
                buf_putc(&b, ',');
            write_book_row(&b, rows[i], NULL);
        }
    }
    mysql_free_result(qr);   /* rows 포인터를 다 쓴 뒤에 해제 */
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 판매 글 쓰기 */

/* POST /api/books - 글과 사진을 한 트랜잭션으로 저장한다 (사진 하나라도 실패하면 글도 남기지 않는다) */
static void handle_book_create(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    const Json *photos;
    char *title = NULL, *author = NULL, *course = NULL, *desc = NULL;
    const char *cond, *payload[BOOK_PHOTO_MAX], *mime[BOOK_PHOTO_MAX];
    long price;
    unsigned dept_id;
    int i, nphotos = 0;
    Buf sql;
    unsigned long long bid;

    if (!auth_require(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    title  = str_dup(json_str(in, "title", ""));
    author = str_dup(json_str(in, "author", ""));
    course = str_dup(json_str(in, "course", ""));
    desc   = str_dup(json_str(in, "description", ""));
    price  = json_int(in, "price", -1);
    dept_id = (unsigned)json_int(in, "department_id", 0);
    cond = condition_code(json_str(in, "condition", "good"));
    if (!title || !author || !course || !desc) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        goto out;
    }
    str_trim(title);
    str_trim(author);
    str_trim(course);
    str_trim(desc);

    /* 길이는 바이트 기준 (한글 1글자 = 3바이트) */
    if (strlen(title) < 2 || strlen(title) > 600) {
        res_error(res, 400, "bad_title", "책 제목을 2~200자로 써 주세요.");
        goto out;
    }
    if (strlen(author) > 300 || strlen(course) > 300) {
        res_error(res, 400, "bad_field", "저자·과목명은 100자까지 쓸 수 있습니다.");
        goto out;
    }
    if (strlen(desc) > 6000) {
        res_error(res, 400, "bad_description", "설명은 2000자까지 쓸 수 있습니다.");
        goto out;
    }
    utf8_cut(title, 600);
    if (price < 0 || price > BOOK_PRICE_MAX) {
        res_error(res, 400, "bad_price", "가격은 0~1,000,000원으로 정하세요.");
        goto out;
    }
    if (!cond) {
        res_error(res, 400, "bad_condition", "책 상태는 new, good, fair, poor 중 하나입니다.");
        goto out;
    }
    if (dept_id) {
        buf_init(&sql);
        db_sqlf(&sql, "SELECT COUNT(*) FROM departments WHERE id = %u", dept_id);
        if (db_scalar(sql.data, 0) == 0)
            dept_id = 0;   /* 없는 학과면 지정하지 않은 것으로 */
        buf_free(&sql);
    }

    photos = json_get(in, "photos");
    if (photos && photos->type == JS_ARR) {
        if (photos->n > BOOK_PHOTO_MAX) {
            res_error(res, 400, "too_many_photos", "사진은 5장까지 올릴 수 있습니다.");
            goto out;
        }
        for (i = 0; i < photos->n; i++) {
            const Json *p = photos->items[i];
            payload[nphotos] = photo_payload(p && p->type == JS_STR ? p->str : NULL, &mime[nphotos]);
            if (!payload[nphotos]) {
                res_error(res, 400, "bad_photo", "사진은 JPG·PNG·WEBP 형식, 한 장에 1MB 까지 올릴 수 있습니다.");
                goto out;
            }
            nphotos++;
        }
    }

    if (!db_begin()) {
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }
    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO books (seller_id, title, author, course, department_id, price, book_condition, description) "
                  "VALUES (%u, %Q, %Q, %Q, ", u.id, title, author, course);
    if (dept_id)
        db_sqlf(&sql, "%u, ", dept_id);
    else
        buf_puts(&sql, "NULL, ");
    db_sqlf(&sql, "%L, '%s', %Q)", (long long)price, cond, desc);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "판매 글을 저장할 수 없습니다.");
        goto out;
    }
    bid = db_last_id();

    for (i = 0; i < nphotos; i++) {
        buf_reset(&sql);
        db_sqlf(&sql, "INSERT INTO book_photos (book_id, sort_order, mime, data) VALUES (%u, %d, '%s', FROM_BASE64(%Q))",
                (unsigned)bid, i, mime[i], payload[i]);
        if (!db_exec_buf(&sql)) {
            log_err("책 사진 저장 실패: %s", db_error());
            buf_free(&sql);
            db_rollback();
            res_error(res, 500, "db_error", "사진을 저장할 수 없습니다.");
            goto out;
        }
    }
    buf_free(&sql);
    if (!db_commit()) {
        db_rollback();
        res_error(res, 500, "db_error", "판매 글을 저장할 수 없습니다.");
        goto out;
    }

    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "id", (long long)bid);
        buf_putc(&b, '}');
        res_json_buf(res, 201, &b);
        buf_free(&b);
    }

out:
    free(title);
    free(author);
    free(course);
    free(desc);
    json_free(in);
}

/* ------------------------------------------------------------ 상세 · 상태 · 삭제 */

/* GET /api/books/{id}
 * 응답: {"book":{목록과 같은 형태},"description":"전체 설명","photos":[사진 id, ...],
 *        "can_manage":bool,"comments":[{id,body,created_at,author_nickname,author_department,author_id}, ...]} */
static void handle_book_detail(Request *req, Response *res, unsigned bid)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    auth_current(req, &u);

    buf_init(&sql);
    db_sqlf(&sql, BOOK_SELECT "WHERE b.id = %u", bid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (!row) {
        if (qr)
            mysql_free_result(qr);
        res_error(res, 404, "not_found", "판매 글이 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"book\":");
    write_book_row(&b, row, NULL);
    buf_puts(&b, ",\"description\":");
    json_write_str(&b, row[17]);   /* BOOK_SELECT 의 마지막 열 = 설명 전체 */
    mysql_free_result(qr);

    buf_puts(&b, ",\"photos\":[");
    buf_init(&sql);
    db_sqlf(&sql, "SELECT id FROM book_photos WHERE book_id = %u ORDER BY sort_order, id", bid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_puts(&b, row[0]);   /* 숫자 그대로 */
        }
        mysql_free_result(qr);
    }
    buf_putc(&b, ']');

    buf_putc(&b, ',');
    json_kv_bool(&b, "can_manage", u.id && book_manage_ok(bid, &u) == 1);

    /* 문의 댓글 (오래된 순) */
    buf_puts(&b, ",\"comments\":[");
    first = 1;
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT c.id, c.body, DATE_FORMAT(c.created_at, '%%Y-%%m-%%d %%H:%%i'), "
            "       u.nickname, IFNULL(d.name, ''), c.author_id "
            "FROM book_comments c JOIN users u ON u.id = c.author_id "
            "LEFT JOIN departments d ON d.id = u.department_id "
            "WHERE c.book_id = %u ORDER BY c.created_at, c.id", bid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_putc(&b, '{');
            json_kv_int(&b, "id", atoll(row[0]));                            buf_putc(&b, ',');
            json_kv_str(&b, "body", row[1]);                                 buf_putc(&b, ',');
            json_kv_str(&b, "created_at", row[2]);                           buf_putc(&b, ',');
            json_kv_str(&b, "author_nickname", row[3]);                      buf_putc(&b, ',');
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

/* PUT /api/books/{id}/status {status} (판매자·관리자). 판매 완료로 바꾼 시각을 sold_at 에 남긴다. */
static void handle_book_status(Request *req, Response *res, unsigned bid)
{
    CurrentUser u;
    Json *in;
    const char *st;
    Buf sql;
    int ok;

    if (!auth_require(req, res, &u))
        return;
    ok = book_manage_ok(bid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "판매 글이 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "판매자만 바꿀 수 있습니다."); return; }

    in = body_object(req, res);
    if (!in)
        return;
    st = status_code(json_str(in, "status", NULL));
    json_free(in);
    if (!st) {
        res_error(res, 400, "bad_status", "상태는 selling, reserved, sold 중 하나입니다.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "UPDATE books SET status = '%s', sold_at = %s WHERE id = %u",
            st, strcmp(st, "sold") == 0 ? "IFNULL(sold_at, NOW())" : "NULL", bid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "상태를 바꿀 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 200, "{\"ok\":true}");
}

/* DELETE /api/books/{id} (판매자·관리자). 사진·댓글은 CASCADE 로 함께 지워진다. */
static void handle_book_delete(Request *req, Response *res, unsigned bid)
{
    CurrentUser u;
    Buf sql;
    int ok;

    if (!auth_require(req, res, &u))
        return;
    ok = book_manage_ok(bid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "판매 글이 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "판매자만 지울 수 있습니다."); return; }

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM books WHERE id = %u", bid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "판매 글을 지울 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 사진 */

/* GET /api/book-photos/{id} - 저장한 이미지를 그대로 보낸다.
 * 사진은 한 번 올리면 바뀌지 않으므로(같은 id = 같은 내용) 브라우저가 하루 동안 캐시해도 된다. */
static void handle_book_photo(Request *req, Response *res, unsigned pid)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    unsigned long *lens;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT mime, data FROM book_photos WHERE id = %u", pid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    lens = row ? mysql_fetch_lengths(qr) : NULL;
    if (!row || !lens || !row[1]) {
        if (qr)
            mysql_free_result(qr);
        res_error(res, 404, "not_found", "사진이 없습니다.");
        return;
    }

    buf_reset(&res->body);
    buf_add(&res->body, row[1], lens[1]);   /* 이진 데이터이므로 길이를 직접 준다 */
    res->status = 200;
    /* mime 은 서버가 저장할 때 고른 상수(image/jpeg 등)라 그대로 써도 된다 */
    str_copy(res->content_type, sizeof res->content_type, row[0]);
    res_header(res, "Cache-Control", "public, max-age=86400");
    mysql_free_result(qr);
}

/* ------------------------------------------------------------ 문의 댓글 */

/* POST /api/books/{id}/comments {"body":"..."} - 로그인한 누구나 */
static void handle_book_comment(Request *req, Response *res, unsigned bid)
{
    CurrentUser u;
    Json *in;
    char *body;
    Buf sql;

    if (!auth_require(req, res, &u))
        return;
    if (book_seller(bid) < 0) {
        res_error(res, 404, "not_found", "판매 글이 없습니다.");
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

    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO book_comments (book_id, author_id, body) VALUES (%u, %u, %Q)", bid, u.id, body);
    free(body);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "댓글을 저장할 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 201, "{\"ok\":true}");
}

/* DELETE /api/book-comments/{id} (작성자·관리자) */
static void handle_book_comment_delete(Request *req, Response *res, unsigned cid)
{
    CurrentUser u;
    Buf sql;

    if (!auth_require(req, res, &u))
        return;
    buf_init(&sql);
    if (u.is_admin)
        db_sqlf(&sql, "DELETE FROM book_comments WHERE id = %u", cid);
    else
        db_sqlf(&sql, "DELETE FROM book_comments WHERE id = %u AND author_id = %u", cid, u.id);
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

/* ------------------------------------------------------------ 라우터 */

int route_books(Request *req, Response *res)
{
    unsigned id;
    const char *m = req->method;

    if (req_is(req, "GET", "/api/books"))  { handle_book_list(req, res);   return 1; }
    if (req_is(req, "POST", "/api/books")) { handle_book_create(req, res); return 1; }

    if (!strcmp(m, "GET") && path_id(req->path, "/api/book-photos/", NULL, &id)) {
        handle_book_photo(req, res, id);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/book-comments/", NULL, &id)) {
        handle_book_comment_delete(req, res, id);
        return 1;
    }
    if (!strcmp(m, "PUT") && path_id(req->path, "/api/books/", "/status", &id)) {
        handle_book_status(req, res, id);
        return 1;
    }
    if (!strcmp(m, "POST") && path_id(req->path, "/api/books/", "/comments", &id)) {
        handle_book_comment(req, res, id);
        return 1;
    }
    if (!strcmp(m, "GET") && path_id(req->path, "/api/books/", NULL, &id)) {
        handle_book_detail(req, res, id);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/books/", NULL, &id)) {
        handle_book_delete(req, res, id);
        return 1;
    }
    return 0;
}
