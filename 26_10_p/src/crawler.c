/* 학교 홈페이지 공지 수집 (WinHTTP + WordPress REST API) */
#include "crawler.h"
#include "common.h"
#include "json.h"
#include "db.h"

#include <windows.h>
#include <winhttp.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SKU_HOST   L"www.skuniv.ac.kr"
#define USER_AGENT L"SKU-Contest-Board/1.0"

/* 수집에 쓰는 검색어. 학교 검색은 본문까지 뒤지므로 결과를 제목으로 한 번 더 거른다. */
static const char *KEYWORDS[] = {
    "공모전", "해커톤", "경진대회", "공모", "대회", "챌린지", "아이디어", "콘테스트"
};
#define KEYWORD_COUNT ((int)(sizeof KEYWORDS / sizeof KEYWORDS[0]))
#define PER_PAGE      100          /* WordPress REST 의 최대값 */
#define MAX_PAGES     10

/* 제목에 이 가운데 하나가 있어야 공모전·대회로 본다. */
static const char *TITLE_INCLUDE[] = {
    "공모전", "해커톤", "경진대회", "경연대회", "공모", "대회", "챌린지", "콘테스트",
    "어워드", "AWARD", "Award", "Hack", "HACK", "대전", "전람회", NULL
};
/* 참가할 수 없는 글 (결과 발표, 장학금, 서포터즈 모집, 교직원 대상 등) */
static const char *TITLE_EXCLUDE[] = {
    "수상자", "결과 발표", "결과발표", "심사결과", "심사 결과", "결과 안내", "장학", "서포터즈",
    "봉사", "관람안내", "교직원", "채용", "학술대회", "동아리", "이용교육", "이용 교육", NULL
};

static int contains_any(const char *s, const char *const *list)
{
    for (; *list; list++)
        if (strstr(s, *list))
            return 1;
    return 0;
}

/* 학생이 참가할 수 있는 공모전·대회 공지인지 제목으로 가린다. */
static int is_contest_title(const char *title)
{
    return contains_any(title, TITLE_INCLUDE) && !contains_any(title, TITLE_EXCLUDE);
}

static int is_hackathon_title(const char *title)
{
    static const char *const HACK[] = { "해커톤", "Hack", "HACK", "hack", NULL };
    return contains_any(title, HACK);
}

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

/* ------------------------------------------------------------- 공지 본문 */

/* posts.body 는 TEXT(64KB) 다. 여유를 두고 이 길이(바이트)에서 자른다. */
#define BODY_MAX   60000
#define BLANK_MARK '\x01'          /* 원문의 빈 문단(<p>&nbsp;</p>) 자리 */

static const char *const BLOCK_TAGS[] = {
    "p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "table",
    "ul", "ol", "blockquote", "section", "article", "hr", NULL
};
static const char *const SKIP_TAGS[] = {
    "script", "style", "noscript", "iframe", "caption", NULL
};

static int tag_in(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(name, *list) == 0)
            return 1;
    return 0;
}

/* 태그 사이의 글자 조각을 붙인다. 엔티티를 풀고, 소스의 줄바꿈과 U+00A0 은
 * 공백으로, U+200B 는 지운다. 공백이 아닌 글자가 있었으면 1. */
static int append_text(Buf *out, const char *s, size_t n)
{
    char *t = (char *)malloc(n + 1);
    char *r, *w;
    int has = 0;

    if (!t)
        return 0;
    memcpy(t, s, n);
    t[n] = '\0';
    html_entity_decode(t);

    for (r = w = t; *r; r++) {
        unsigned char c = (unsigned char)*r;

        if (c == 0xC2 && (unsigned char)r[1] == 0xA0) {
            *w++ = ' ';
            r++;
        } else if (c == 0xE2 && (unsigned char)r[1] == 0x80 && (unsigned char)r[2] == 0x8B) {
            r += 2;
        } else if (c == '\r' || c == '\n' || c == '\t') {
            *w++ = ' ';
        } else {
            if (c != ' ')
                has = 1;
            *w++ = (char)c;
        }
    }
    *w = '\0';
    buf_puts(out, t);
    free(t);
    return has;
}

/* HTML 을 줄 단위 텍스트로 편다. 블록 태그는 줄바꿈, <li> 는 "• ",
 * 표 칸은 " | " 로 잇고, 이미지와 스크립트 등은 버린다. */
static void html_flatten(const char *h, Buf *out)
{
    int skip = 0, in_cell = 0, cell = 0, had_text = 0;

    while (*h) {
        const char *lt = strchr(h, '<');
        size_t n = lt ? (size_t)(lt - h) : strlen(h);

        if (n && !skip && append_text(out, h, n))
            had_text = 1;
        if (!lt)
            break;

        if (strncmp(lt, "<!--", 4) == 0) {
            const char *end = strstr(lt + 4, "-->");
            h = end ? end + 3 : lt + strlen(lt);
            continue;
        }

        {
            const char *gt = strchr(lt, '>');
            const char *p = lt + 1;
            char name[16];
            size_t k = 0;
            int closing = 0;

            if (!gt)
                break;
            h = gt + 1;

            if (*p == '/') {
                closing = 1;
                p++;
            }
            while (p < gt && k + 1 < sizeof name && isalnum((unsigned char)*p))
                name[k++] = (char)tolower((unsigned char)*p++);
            name[k] = '\0';
            if (!k)
                continue;

            if (tag_in(name, SKIP_TAGS)) {
                skip += closing ? -1 : 1;
                if (skip < 0)
                    skip = 0;
            } else if (skip) {
                /* 버리는 구간 안의 태그 */
            } else if (!strcmp(name, "td") || !strcmp(name, "th")) {
                if (!closing && cell++)
                    buf_puts(out, " | ");
                in_cell = !closing;
            } else if (tag_in(name, BLOCK_TAGS)) {
                if (in_cell) {
                    buf_putc(out, ' ');             /* 칸 안의 문단은 한 줄로 */
                } else if (!strcmp(name, "br")) {
                    buf_putc(out, '\n');
                } else if (!closing) {
                    buf_putc(out, '\n');
                    had_text = 0;
                    if (!strcmp(name, "tr"))
                        cell = 0;
                    if (!strcmp(name, "li"))
                        buf_puts(out, "• ");
                } else if (!strcmp(name, "p") && !had_text) {
                    buf_puts(out, "\n\x01\n");      /* BLANK_MARK */
                } else {
                    buf_putc(out, '\n');
                }
            }
        }
    }
}

/* 링크 주소를 지운다. "(주소)" 는 앞 공백과 괄호까지 함께 지운다. */
static void strip_urls(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        const char *q = r;
        int paren = 0;

        if (*q == '(') {
            paren = 1;
            for (q++; *q == ' '; q++)
                ;
        }
        if (strncmp(q, "http://", 7) == 0 || strncmp(q, "https://", 8) == 0) {
            const char *e = q;

            while (*e && *e != ' ' && !(paren && *e == ')'))
                e++;
            if (paren) {
                const char *c = e;

                while (*c == ' ')
                    c++;
                if (*c == ')') {
                    e = c + 1;
                    while (w > s && w[-1] == ' ')
                        w--;
                } else {
                    *w++ = '(';             /* 닫는 괄호가 없으면 주소만 지운다 */
                }
            }
            r = (char *)e;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* 연속 공백을 하나로 줄이고 양끝을 다듬는다. */
static void collapse_spaces(char *s)
{
    char *r, *w;

    for (r = w = s; *r; r++)
        if (*r != ' ' || (w > s && w[-1] != ' '))
            *w++ = *r;
    *w = '\0';
    str_trim(s);
}

/* 공지 HTML 을 게시판 본문용 텍스트로 바꿔 out 끝에 붙인다.
 * 빈 줄은 원문의 빈 문단 자리에만 하나씩 남긴다. 붙인 글이 있으면 1. */
static int html_to_text(const char *html, Buf *out)
{
    size_t start = out->len;
    int blank = 0;
    Buf flat;
    char *line;

    buf_init(&flat);
    html_flatten(html, &flat);

    for (line = flat.data; line; ) {
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';
        strip_urls(line);
        collapse_spaces(line);

        if (line[0] == BLANK_MARK && line[1] == '\0') {
            blank = (out->len > start);
        } else if (line[0] && strcmp(line, "•") && strcmp(line, "|")) {
            char *m;

            while ((m = strchr(line, BLANK_MARK)) != NULL)
                memmove(m, m + 1, strlen(m));
            if (out->len > start)
                buf_puts(out, blank ? "\n\n" : "\n");
            buf_puts(out, line);
            blank = 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    buf_free(&flat);

    if (out->len - start > BODY_MAX) {
        size_t cut = start + BODY_MAX;

        while (cut > start && ((unsigned char)out->data[cut] & 0xC0) == 0x80)
            cut--;                                  /* UTF-8 글자 중간에서 자르지 않는다 */
        out->data[cut] = '\0';
        out->len = cut;
    }
    return out->len > start;
}

int notice_body_text(long ext_id, Buf *out)
{
    char path[96], err[256];
    Buf resp;
    Json *root;
    const Json *content;
    int ok = 0;

    snprintf(path, sizeof path, "/wp-json/wp/v2/notice/%ld?_fields=content", ext_id);
    buf_init(&resp);
    if (!https_get(path, &resp, err, sizeof err)) {
        log_warn("공지 %ld 본문을 받지 못했습니다: %s", ext_id, err);
        buf_free(&resp);
        return 0;
    }

    root = json_parse(resp.data ? resp.data : "");
    content = root ? json_path(root, "content", "rendered") : NULL;
    if (content && content->type == JS_STR)
        ok = html_to_text(content->str, out);
    else
        log_warn("공지 %ld 본문을 해석할 수 없습니다.", ext_id);

    json_free(root);
    buf_free(&resp);
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
        "SELECT id, keyword, title, url, DATE_FORMAT(published_at, '%Y-%m-%d'), ext_id "
        "FROM notices WHERE status = 'pending' ORDER BY published_at, id");
    if (!res)
        return 0;

    while ((row = mysql_fetch_row(res)) != NULL) {
        unsigned notice_id = (unsigned)strtoul(row[0], NULL, 10);
        const char *keyword = row[1] ? row[1] : "";
        const char *title   = row[2] ? row[2] : "";
        const char *url     = row[3] ? row[3] : "";
        const char *date    = row[4] ? row[4] : "";
        long ext_id         = row[5] ? strtol(row[5], NULL, 10) : 0;
        const char *kind    = (strcmp(keyword, "해커톤") == 0 || is_hackathon_title(title))
                              ? "hackathon" : "contest";
        Buf body, sql;
        unsigned long long post_id;

        /* 본문은 공지 원문을 텍스트로 옮긴다. 원문 링크는 source_url 에 따로 둔다.
         * 원문을 못 받으면 출처만 적어 둔다. 학교 서버에 부담을 주지 않게 조금 쉰다. */
        buf_init(&body);
        if (count)
            Sleep(200);
        if (!notice_body_text(ext_id, &body))
            buf_printf(&body, "학교 공지 (%s, %s 게시)", keyword, date);

        if (!db_begin()) {
            buf_free(&body);
            break;
        }

        /* 작성자는 비워 둔다. 사람이 쓴 글이 아니라 학교 공지에서 온 글이다.
         * 등록 시각은 학교에 공지가 올라온 날로 둔다 (옛 공지가 새 글처럼 보이지 않게). */
        buf_init(&sql);
        db_sqlf(&sql,
                "INSERT INTO posts (author_id, kind, title, body, source_url, host, "
                "                   deadline, need_people, created_at) "
                "VALUES (NULL, '%s', %Q, %Q, %Q, NULL, NULL, 0, %Q)",
                kind, title, body.data, url, date);
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
        window_days = 730;
    cutoff_iso(window_days, cutoff, sizeof cutoff);

    log_info("공지 수집 시작 (%s 이후)", cutoff);

    for (k = 0; k < KEYWORD_COUNT; k++) {
      int page, seen = 0, kept = 0;

      for (page = 1; page <= MAX_PAGES; page++) {
        Buf path, body;
        Json *root;
        int i, n;
        char err[256];

        buf_init(&path);
        buf_init(&body);

        buf_printf(&path, "/wp-json/wp/v2/notice?per_page=%d&page=%d&_fields=id,date,link,title&search=",
                   PER_PAGE, page);
        url_encode(&path, KEYWORDS[k]);
        buf_puts(&path, "&after=");
        url_encode(&path, cutoff);

        if (!https_get(path.data, &body, err, sizeof err)) {
            buf_free(&path);
            buf_free(&body);
            if (page > 1)
                break;              /* 마지막 페이지를 넘기면 WordPress 가 400 을 준다 */
            str_copy(out->error, sizeof out->error, err);
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

        n = root->n;
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
            seen++;
            if (!is_contest_title(clean)) {
                free(clean);
                continue;
            }
            kept++;

            out->fetched++;
            if (upsert_notice(id, KEYWORDS[k], clean,
                              json_str(item, "link", ""), date))
                out->inserted++;
            else
                out->skipped++;

            free(clean);
        }

        json_free(root);
        buf_free(&path);
        buf_free(&body);
        if (n < PER_PAGE)
            break;
      }
      log_info("  '%s' 검색: %d건 중 공모전·대회 %d건", KEYWORDS[k], seen, kept);
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
