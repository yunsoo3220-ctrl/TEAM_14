/* 학교 홈페이지 공지 수집 (WinHTTP + WordPress REST API) */
#include "crawler.h"
#include "common.h"
#include "json.h"
#include "db.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SKU_HOST   L"www.skuniv.ac.kr"
#define USER_AGENT L"SKU-Contest-Board/1.0"

/* 수집에 쓰는 검색어. 과제 명세의 검색 링크(공모전/해커톤)와 같다. */
static const char *KEYWORDS[] = { "공모전", "해커톤" };
#define KEYWORD_COUNT ((int)(sizeof KEYWORDS / sizeof KEYWORDS[0]))

/* ------------------------------------------------------------ 문자열 변환 */

/* URL 경로/쿼리에 안전하지 않은 바이트를 퍼센트 인코딩한다. */
static void url_encode(Buf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            buf_putc(b, (char)c);
        } else {
            buf_putc(b, '%');
            buf_putc(b, hex[c >> 4]);
            buf_putc(b, hex[c & 0xF]);
        }
    }
}

/* UTF-8 -> UTF-16. 반환값은 호출자가 free. */
static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;

    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

void html_entity_decode(char *s)
{
    char *w = s;

    while (*s) {
        if (*s != '&') {
            *w++ = *s++;
            continue;
        }

        {
            char *semi = strchr(s, ';');
            size_t len;

            if (!semi || (size_t)(semi - s) > 10) {
                *w++ = *s++;
                continue;
            }
            len = (size_t)(semi - s) - 1;      /* '&' 와 ';' 사이 길이 */

            if (s[1] == '#') {
                /* 숫자 참조: &#8216; 또는 &#x2018; */
                unsigned long cp;
                char *end;

                if (s[2] == 'x' || s[2] == 'X')
                    cp = strtoul(s + 3, &end, 16);
                else
                    cp = strtoul(s + 2, &end, 10);

                if (end != semi || cp == 0) {
                    *w++ = *s++;
                    continue;
                }
                /* UTF-8 로 기록 (w <= s 이므로 제자리 쓰기가 안전하다) */
                if (cp < 0x80) {
                    *w++ = (char)cp;
                } else if (cp < 0x800) {
                    *w++ = (char)(0xC0 | (cp >> 6));
                    *w++ = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    *w++ = (char)(0xE0 | (cp >> 12));
                    *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (cp & 0x3F));
                } else {
                    *w++ = (char)(0xF0 | (cp >> 18));
                    *w++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                    *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (cp & 0x3F));
                }
                s = semi + 1;
                continue;
            }

            /* 이름 참조 */
            if (len == 3 && strncmp(s + 1, "amp", 3) == 0)       { *w++ = '&';  s = semi + 1; continue; }
            if (len == 2 && strncmp(s + 1, "lt", 2) == 0)        { *w++ = '<';  s = semi + 1; continue; }
            if (len == 2 && strncmp(s + 1, "gt", 2) == 0)        { *w++ = '>';  s = semi + 1; continue; }
            if (len == 4 && strncmp(s + 1, "quot", 4) == 0)      { *w++ = '"';  s = semi + 1; continue; }
            if (len == 4 && strncmp(s + 1, "apos", 4) == 0)      { *w++ = '\''; s = semi + 1; continue; }
            if (len == 4 && strncmp(s + 1, "nbsp", 4) == 0)      { *w++ = ' ';  s = semi + 1; continue; }

            *w++ = *s++;
        }
    }
    *w = '\0';
}

/* HTML 태그를 제거한다 (제목에 <strong> 등이 섞여 오는 경우). */
static void strip_tags(char *s)
{
    char *w = s;
    int in_tag = 0;

    for (; *s; s++) {
        if (*s == '<')       in_tag = 1;
        else if (*s == '>')  in_tag = 0;
        else if (!in_tag)    *w++ = *s;
    }
    *w = '\0';
}

/* ---------------------------------------------------------------- HTTP GET */

/* path 는 "/wp-json/..." 형태의 UTF-8 문자열. 본문을 out 에 담고 1 을 돌려준다. */
static int https_get(const char *path, Buf *out, char *err, size_t errsz)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    wchar_t *wpath = utf8_to_wide(path);
    DWORD status = 0, statlen = sizeof status;
    int ok = 0;

    if (!wpath) {
        str_copy(err, errsz, "경로 변환 실패");
        return 0;
    }

    ses = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        snprintf(err, errsz, "WinHttpOpen 실패 (%lu)", GetLastError());
        goto done;
    }

    {
        DWORD to = 20000;
        WinHttpSetTimeouts(ses, (int)to, (int)to, (int)to, (int)to);
    }

    con = WinHttpConnect(ses, SKU_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        snprintf(err, errsz, "WinHttpConnect 실패 (%lu)", GetLastError());
        goto done;
    }

    req = WinHttpOpenRequest(con, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        snprintf(err, errsz, "WinHttpOpenRequest 실패 (%lu)", GetLastError());
        goto done;
    }

    if (!WinHttpSendRequest(req, L"Accept: application/json\r\n", (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        snprintf(err, errsz, "학교 홈페이지 요청 실패 (%lu)", GetLastError());
        goto done;
    }

    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statlen, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        snprintf(err, errsz, "학교 홈페이지가 HTTP %lu 를 반환했습니다.", status);
        goto done;
    }

    for (;;) {
        DWORD avail = 0, read = 0;
        char chunk[8192];

        if (!WinHttpQueryDataAvailable(req, &avail)) {
            snprintf(err, errsz, "응답 읽기 실패 (%lu)", GetLastError());
            goto done;
        }
        if (avail == 0)
            break;
        if (avail > sizeof chunk)
            avail = sizeof chunk;
        if (!WinHttpReadData(req, chunk, avail, &read) || read == 0)
            break;
        buf_add(out, chunk, read);
    }
    ok = 1;

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(wpath);
    return ok;
}

/* ------------------------------------------------------------------ 수집 */

/* "2026-09-30T10:00:00" -> "2026-09-30" */
static void iso_date_only(const char *iso, char *out, size_t outsz)
{
    str_copy(out, outsz, iso ? iso : "");
    if (strlen(out) >= 10)
        out[10] = '\0';
}

/* 공지 하나를 notices 에 넣는다. 새로 넣었으면 1, 이미 있으면 0. */
static int upsert_notice(long ext_id, const char *keyword, const char *title,
                         const char *url, const char *date)
{
    Buf sql;
    int inserted;

    buf_init(&sql);
    /* 이미 있는 공지는 제목/링크만 새로 고치고 status 는 건드리지 않는다. */
    db_sqlf(&sql,
            "INSERT INTO notices (ext_id, keyword, title, url, published_at, fetched_at) "
            "VALUES (%L, %Q, %Q, %Q, %Q, NOW()) "
            "ON DUPLICATE KEY UPDATE title = VALUES(title), url = VALUES(url), "
            "published_at = VALUES(published_at), fetched_at = NOW()",
            (long long)ext_id, keyword, title, url, date);

    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        return 0;
    }
    /* MySQL 은 INSERT 면 1, UPDATE 면 2(값이 같으면 0)를 돌려준다. */
    inserted = (db_affected() == 1);
    buf_free(&sql);
    return inserted;
}

/* 아직 게시물이 되지 않은(pending) 공지를 모두 게시물로 올린다.
 *
 * 공모전 정보 자체는 로그인 없이 누구나 볼 수 있어야 하므로 수집한 공지는
 * 바로 공개한다. 대상 학과는 비워 두고, 관리자가 게시물에서 체크한다.
 * 학과가 비어 있는 동안에는 로그인한 사람 누구나 댓글을 쓸 수 있다.
 *
 * 올린 건수를 돌려준다. */
static int publish_pending_notices(void)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    int count = 0;

    res = db_query(
        "SELECT id, keyword, title, url, DATE_FORMAT(published_at, '%Y-%m-%d') "
        "FROM notices WHERE status = 'pending' ORDER BY published_at, id");
    if (!res)
        return 0;

    while ((row = mysql_fetch_row(res)) != NULL) {
        unsigned notice_id = (unsigned)strtoul(row[0], NULL, 10);
        const char *keyword = row[1] ? row[1] : "";
        const char *title   = row[2] ? row[2] : "";
        const char *url     = row[3] ? row[3] : "";
        const char *date    = row[4] ? row[4] : "";
        const char *kind    = (strcmp(keyword, "해커톤") == 0) ? "hackathon" : "contest";
        Buf body, sql;
        unsigned long long post_id;

        buf_init(&body);
        buf_printf(&body, "학교 공지 (%s, %s 게시)\n%s", keyword, date, url);

        if (!db_begin()) {
            buf_free(&body);
            break;
        }

        /* 작성자는 비워 둔다. 사람이 쓴 글이 아니라 학교 공지에서 온 글이다. */
        buf_init(&sql);
        db_sqlf(&sql,
                "INSERT INTO posts (author_id, kind, title, body, source_url, host, "
                "                   deadline, need_people) "
                "VALUES (NULL, '%s', %Q, %Q, %Q, NULL, NULL, 0)",
                kind, title, body.data, url);
        buf_free(&body);

        if (!db_exec_buf(&sql)) {
            buf_free(&sql);
            db_rollback();
            continue;
        }
        buf_free(&sql);

        post_id = db_last_id();

        buf_init(&sql);
        db_sqlf(&sql,
                "UPDATE notices SET status = 'published', post_id = %L WHERE id = %u",
                (long long)post_id, notice_id);
        if (!db_exec_buf(&sql)) {
            buf_free(&sql);
            db_rollback();
            continue;
        }
        buf_free(&sql);

        if (db_commit())
            count++;
    }

    mysql_free_result(res);
    return count;
}

/* window_days 일 전 날짜를 "YYYY-MM-DDT00:00:00" 으로 만든다. */
static void cutoff_iso(int window_days, char *out, size_t outsz)
{
    time_t t = time(NULL) - (time_t)window_days * 24 * 60 * 60;
    struct tm tmv;

    localtime_s(&tmv, &t);
    strftime(out, outsz, "%Y-%m-%dT00:00:00", &tmv);
}

int crawl_notices(int window_days, CrawlResult *out)
{
    char cutoff[32];
    int k;

    memset(out, 0, sizeof *out);
    if (window_days <= 0)
        window_days = 92;
    cutoff_iso(window_days, cutoff, sizeof cutoff);

    log_info("공지 수집 시작 (%s 이후)", cutoff);

    for (k = 0; k < KEYWORD_COUNT; k++) {
        Buf path, body;
        Json *root;
        int i;

        buf_init(&path);
        buf_init(&body);

        buf_puts(&path, "/wp-json/wp/v2/notice?per_page=50&_fields=id,date,link,title&search=");
        url_encode(&path, KEYWORDS[k]);
        buf_puts(&path, "&after=");
        url_encode(&path, cutoff);

        if (!https_get(path.data, &body, out->error, sizeof out->error)) {
            buf_free(&path);
            buf_free(&body);
            return 0;
        }

        root = json_parse(body.data ? body.data : "");
        if (!root || root->type != JS_ARR) {
            str_copy(out->error, sizeof out->error,
                     "학교 홈페이지 응답을 해석할 수 없습니다.");
            json_free(root);
            buf_free(&path);
            buf_free(&body);
            return 0;
        }

        for (i = 0; i < root->n; i++) {
            const Json *item = root->items[i];
            const Json *title;
            long id;
            char date[16];
            char *clean;

            if (!item || item->type != JS_OBJ)
                continue;

            id = json_int(item, "id", 0);
            title = json_path(item, "title", "rendered");
            if (id <= 0 || !title || title->type != JS_STR)
                continue;

            iso_date_only(json_str(item, "date", ""), date, sizeof date);
            if (strlen(date) != 10)
                continue;

            clean = str_dup(title->str);
            if (!clean)
                continue;
            strip_tags(clean);
            html_entity_decode(clean);
            str_trim(clean);

            out->fetched++;
            if (upsert_notice(id, KEYWORDS[k], clean,
                              json_str(item, "link", ""), date))
                out->inserted++;
            else
                out->skipped++;

            free(clean);
        }

        log_info("  '%s' 검색: %d건 처리", KEYWORDS[k], root->n);

        json_free(root);
        buf_free(&path);
        buf_free(&body);
    }

    /* 받아온 공지를 바로 공개한다. */
    out->published = publish_pending_notices();

    /* 마지막 수집 시각 기록 */
    {
        Buf sql;
        char ts[32];
        time_t now = time(NULL);
        struct tm tmv;

        localtime_s(&tmv, &now);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

        buf_init(&sql);
        db_sqlf(&sql,
                "INSERT INTO app_config (k, v) VALUES ('crawl.fetched_at', %Q) "
                "ON DUPLICATE KEY UPDATE v = VALUES(v)", ts);
        db_exec_buf(&sql);
        buf_free(&sql);
    }

    log_info("공지 수집 완료: 받음 %d, 신규 %d, 기존 %d, 게시 %d",
             out->fetched, out->inserted, out->skipped, out->published);
    return 1;
}
