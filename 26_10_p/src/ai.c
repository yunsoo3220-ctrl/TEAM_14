/* AI 게시물 분석 (Claude API + WinHTTP) */
#include "ai.h"
#include "db.h"
#include "json.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define API_HOST      L"api.anthropic.com"
#define API_PATH      L"/v1/messages"
#define USER_AGENT    L"SKU-Contest-Board/1.0"
/* 분류 위주의 짧은 작업이라 non-streaming 기본값으로 충분하다. */
#define MAX_TOKENS    16000
#define MAX_RETRIES   2
#define MAX_DEPT_LIST 256
#define MAX_PICKED    8
#define MAX_TAGS      6

/* ------------------------------------------------------------ 작업 상태 */

static volatile LONG     g_init;       /* 0 = 미초기화, 1 = 초기화 중, 2 = 완료 */
static CRITICAL_SECTION  g_lock;
static AiStatus          g_status;

static void ensure_init(void)
{
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        memset(&g_status, 0, sizeof g_status);
        InterlockedExchange(&g_init, 2);
    }
    while (g_init != 2)
        Sleep(0);
}

int ai_enabled(void)
{
    const char *key = getenv("ANTHROPIC_API_KEY");
    return key && key[0];
}

void ai_status(AiStatus *out)
{
    ensure_init();
    EnterCriticalSection(&g_lock);
    *out = g_status;
    LeaveCriticalSection(&g_lock);
    out->enabled = ai_enabled();
}

/* ------------------------------------------------------------ 문자열 도우미 */

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

/* UTF-8 글자 중간을 피해 maxbytes 이하로 자른다. */
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

/* 문자열을 복사해 다듬고 길이를 제한한다. 호출자가 free. */
static char *clean_copy(const char *s, size_t maxbytes)
{
    char *c = str_dup(s ? s : "");

    if (!c)
        return NULL;
    str_trim(c);
    utf8_cut(c, maxbytes);
    return c;
}

static int is_iso_date(const char *s)
{
    int i;

    if (!s || strlen(s) != 10)
        return 0;
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return 0;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------ HTTPS POST */

/* api.anthropic.com 에 body 를 POST 한다. 전송이 되면 1 (HTTP 상태와 무관),
 * 응답 본문은 out, 상태 코드는 status. */
static int anthropic_post(const char *body, Buf *out, DWORD *status, char *err, size_t errsz)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD statlen = sizeof *status;
    DWORD len = (DWORD)strlen(body);
    wchar_t *wheaders = NULL;
    Buf headers;
    int ok = 0;

    *status = 0;
    buf_init(&headers);
    buf_printf(&headers,
               "Content-Type: application/json\r\n"
               "x-api-key: %s\r\n"
               "anthropic-version: 2023-06-01\r\n"
               /* 안전 분류기가 거절하면 Anthropic 이 권하는 모델로 같은 요청을 다시 돌린다. */
               "anthropic-beta: server-side-fallback-2026-07-01\r\n",
               getenv("ANTHROPIC_API_KEY"));
    wheaders = utf8_to_wide(headers.data);
    buf_free(&headers);
    if (!wheaders) {
        str_copy(err, errsz, "헤더 변환 실패");
        return 0;
    }

    ses = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        snprintf(err, errsz, "WinHttpOpen 실패 (%lu)", GetLastError());
        goto done;
    }
    /* 분석 한 건이 수십 초 걸릴 수 있어 넉넉히 둔다. */
    WinHttpSetTimeouts(ses, 15000, 15000, 30000, 180000);

    con = WinHttpConnect(ses, API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) {
        snprintf(err, errsz, "WinHttpConnect 실패 (%lu)", GetLastError());
        goto done;
    }
    req = WinHttpOpenRequest(con, L"POST", API_PATH, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        snprintf(err, errsz, "WinHttpOpenRequest 실패 (%lu)", GetLastError());
        goto done;
    }

    if (!WinHttpSendRequest(req, wheaders, (DWORD)-1L, (LPVOID)body, len, len, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        snprintf(err, errsz, "Claude API 요청 실패 (%lu)", GetLastError());
        goto done;
    }

    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, status, &statlen, WINHTTP_NO_HEADER_INDEX);

    for (;;) {
        DWORD avail = 0, got = 0;
        char chunk[8192];

        if (!WinHttpQueryDataAvailable(req, &avail)) {
            snprintf(err, errsz, "응답 읽기 실패 (%lu)", GetLastError());
            goto done;
        }
        if (avail == 0)
            break;
        if (avail > sizeof chunk)
            avail = sizeof chunk;
        if (!WinHttpReadData(req, chunk, avail, &got) || got == 0)
            break;
        buf_add(out, chunk, got);
    }
    ok = 1;

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(wheaders);
    return ok;
}

static int retryable(DWORD status)
{
    return status == 429 || status == 500 || status == 502 || status == 503 ||
           status == 504 || status == 529;
}

/* ------------------------------------------------------------ 프롬프트 */

typedef struct {
    unsigned ids[MAX_DEPT_LIST];
    int      n;
    Buf      listing;          /* "12: 이공대학 / 컴퓨터공학과" 줄들 */
} DeptTable;

static int load_departments(DeptTable *t)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;

    t->n = 0;
    buf_init(&t->listing);
    qr = db_query("SELECT d.id, c.name, d.name FROM departments d "
                  "JOIN colleges c ON c.id = d.college_id ORDER BY c.sort, d.sort, d.id");
    if (!qr)
        return 0;
    while ((row = mysql_fetch_row(qr)) != NULL && t->n < MAX_DEPT_LIST) {
        t->ids[t->n++] = (unsigned)strtoul(row[0], NULL, 10);
        buf_printf(&t->listing, "%s: %s / %s\n", row[0], row[1], row[2]);
    }
    mysql_free_result(qr);
    return t->n > 0;
}

static int dept_known(const DeptTable *t, unsigned id)
{
    int i;

    for (i = 0; i < t->n; i++)
        if (t->ids[i] == id)
            return 1;
    return 0;
}

/* 학과 목록이 바뀌지 않는 한 같은 문자열이라 캐시 접두부로 재사용된다. */
static void build_system_prompt(Buf *b, const DeptTable *t)
{
    buf_puts(b,
        "당신은 서경대학교 학생 커뮤니티에 올라온 공모전·해커톤 게시물을 분석합니다. "
        "게시물은 대부분 학교 공지사항에서 가져온 외부 공모전 안내입니다. "
        "분석 결과는 학생에게 자기 학과와 관련 있는 공모전을 추천하는 데 쓰입니다.\n\n"
        "각 항목을 다음 기준으로 채우세요.\n"
        "- summary: 학생이 목록에서 보고 참여 여부를 판단할 수 있도록 무엇을 모집하는지, "
        "누가 참여할 수 있는지, 언제까지인지, 혜택이 무엇인지를 한국어 2~3문장, "
        "180자 이내로 요약합니다.\n"
        "- tags: 분야와 결과물 형식을 나타내는 짧은 한국어 키워드 3~5개입니다 "
        "(예: 영상, AI, 아이디어, 디자인, 창업, 논문). '공모전', '안내'처럼 모든 글에 "
        "해당하는 말은 넣지 않습니다.\n"
        "- host: 주최 또는 주관 기관 이름입니다. 본문에 없으면 빈 문자열입니다.\n"
        "- deadline: 접수·제출 마감일을 YYYY-MM-DD 형식으로 씁니다. '~10.31.'처럼 "
        "연도가 빠져 있으면 게시일을 기준으로 연도를 정합니다. 알 수 없으면 빈 문자열입니다.\n"
        "- departments: 아래 학과 목록 가운데, 이 공모전의 주제와 요구 역량이 그 학과 "
        "학생의 전공 지식이나 관심사와 실제로 맞닿는 학과입니다. 누구나 참여할 수 있는 "
        "공모전이어도 주제에 잘 맞는 학과일수록 score(0~100)를 높게 줍니다. reason 에는 "
        "그 학과 학생에게 왜 맞는지를 한국어 한 문장, 80자 이내로 씁니다. score 가 40 "
        "미만인 학과는 넣지 않고, 많아야 8개까지 고릅니다. department_id 는 반드시 "
        "아래 목록에 있는 번호만 씁니다.\n\n"
        "본문에 없는 사실은 지어내지 않습니다.\n\n"
        "학과 목록 (department_id: 대학 / 학과)\n");
    buf_puts(b, t->listing.data ? t->listing.data : "");
}

/* 구조화 출력 스키마. 범위 검사(score 0~100, 학과 번호)는 받은 뒤 서버에서 한다. */
static const char *OUTPUT_SCHEMA =
    "{\"type\":\"object\",\"additionalProperties\":false,"
    "\"required\":[\"summary\",\"tags\",\"host\",\"deadline\",\"departments\"],"
    "\"properties\":{"
      "\"summary\":{\"type\":\"string\"},"
      "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"host\":{\"type\":\"string\"},"
      "\"deadline\":{\"type\":\"string\"},"
      "\"departments\":{\"type\":\"array\",\"items\":{"
        "\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"department_id\",\"score\",\"reason\"],"
        "\"properties\":{"
          "\"department_id\":{\"type\":\"integer\"},"
          "\"score\":{\"type\":\"integer\"},"
          "\"reason\":{\"type\":\"string\"}}}}}}";

static void build_request(Buf *b, const char *system_prompt, const char *user_text)
{
    buf_printf(b, "{\"model\":\"%s\",\"max_tokens\":%d,", AI_MODEL, MAX_TOKENS);
    buf_puts(b, "\"fallbacks\":\"default\",");
    /* 분류·추출 작업이라 깊은 추론은 필요 없다. Opus 5.5 는 기본값이 medium 이라 명시한다. */
    buf_puts(b, "\"output_config\":{\"effort\":\"medium\",\"format\":{\"type\":\"json_schema\",\"schema\":");
    buf_puts(b, OUTPUT_SCHEMA);
    buf_puts(b, "}},\"system\":[{\"type\":\"text\",\"text\":");
    json_write_str(b, system_prompt);
    buf_puts(b, ",\"cache_control\":{\"type\":\"ephemeral\"}}],"
                "\"messages\":[{\"role\":\"user\",\"content\":");
    json_write_str(b, user_text);
    buf_puts(b, "}]}");
}

/* ------------------------------------------------------------ 응답 처리 */

typedef struct {
    unsigned id;
    unsigned score;
    char    *reason;
} Picked;

/* Claude 응답 전체(JSON)에서 구조화 출력 텍스트를 꺼낸다. 호출자가 json_free. */
static Json *extract_result(const char *resp, DWORD status, char *err, size_t errsz)
{
    Json *root = json_parse(resp ? resp : "");
    const Json *content;
    const char *stop;
    Json *out = NULL;
    Buf text;
    int i;

    if (!root || root->type != JS_OBJ) {
        snprintf(err, errsz, "Claude API 응답을 해석할 수 없습니다 (HTTP %lu).", status);
        json_free(root);
        return NULL;
    }
    if (status != 200) {
        const Json *m = json_path(root, "error", "message");
        snprintf(err, errsz, "Claude API 오류 (HTTP %lu): %s", status,
                 m && m->type == JS_STR ? m->str : "알 수 없는 오류");
        json_free(root);
        return NULL;
    }

    /* content 를 읽기 전에 stop_reason 부터 본다. */
    stop = json_str(root, "stop_reason", "");
    if (strcmp(stop, "refusal") == 0) {
        str_copy(err, errsz, "AI 가 이 게시물의 분석을 거절했습니다.");
        json_free(root);
        return NULL;
    }
    if (strcmp(stop, "max_tokens") == 0) {
        str_copy(err, errsz, "AI 응답이 길이 제한에 걸려 잘렸습니다.");
        json_free(root);
        return NULL;
    }

    buf_init(&text);
    content = json_get(root, "content");
    if (content && content->type == JS_ARR) {
        for (i = 0; i < content->n; i++) {
            const Json *blk = content->items[i];
            if (blk && blk->type == JS_OBJ && strcmp(json_str(blk, "type", ""), "text") == 0)
                buf_puts(&text, json_str(blk, "text", ""));
        }
    }

    {
        const Json *in_tok  = json_path(root, "usage", "input_tokens");
        const Json *out_tok = json_path(root, "usage", "output_tokens");
        log_info("  Claude 응답: model=%s, 입력 %.0f / 출력 %.0f 토큰",
                 json_str(root, "model", "?"),
                 in_tok && in_tok->type == JS_NUM ? in_tok->num : 0.0,
                 out_tok && out_tok->type == JS_NUM ? out_tok->num : 0.0);
    }
    json_free(root);

    out = json_parse(text.data ? text.data : "");
    buf_free(&text);
    if (!out || out->type != JS_OBJ) {
        str_copy(err, errsz, "AI 가 돌려준 결과가 JSON 이 아닙니다.");
        json_free(out);
        return NULL;
    }
    return out;
}

static int save_result(unsigned post_id, const Json *r, const DeptTable *t,
                       char *err, size_t errsz)
{
    char *summary = clean_copy(json_str(r, "summary", ""), 1800);
    char *host    = clean_copy(json_str(r, "host", ""), 300);
    char *dl      = clean_copy(json_str(r, "deadline", ""), 16);
    const Json *tags  = json_get(r, "tags");
    const Json *depts = json_get(r, "departments");
    Picked picked[MAX_PICKED];
    int npicked = 0, ntags = 0, i, ok = 0;
    Buf tagjson, sql;

    buf_init(&tagjson);
    if (!summary || !host || !dl || !summary[0]) {
        str_copy(err, errsz, "AI 결과에 요약이 없습니다.");
        goto out;
    }

    /* 태그: JSON 배열 문자열로 저장해 응답에 그대로 싣는다 (서버가 만든 값). */
    buf_putc(&tagjson, '[');
    if (tags && tags->type == JS_ARR) {
        for (i = 0; i < tags->n && ntags < MAX_TAGS; i++) {
            char *tag;
            if (!tags->items[i] || tags->items[i]->type != JS_STR)
                continue;
            tag = clean_copy(tags->items[i]->str, 60);
            if (tag && tag[0]) {
                if (ntags++)
                    buf_putc(&tagjson, ',');
                json_write_str(&tagjson, tag);
            }
            free(tag);
        }
    }
    buf_putc(&tagjson, ']');

    /* 학과: 목록에 있는 번호만, 중복 없이, 점수는 0~100 으로 묶는다. */
    if (depts && depts->type == JS_ARR) {
        for (i = 0; i < depts->n && npicked < MAX_PICKED; i++) {
            const Json *d = depts->items[i];
            long id, score;
            int j, dup = 0;

            if (!d || d->type != JS_OBJ)
                continue;
            id = json_int(d, "department_id", 0);
            score = json_int(d, "score", 0);
            if (id <= 0 || !dept_known(t, (unsigned)id))
                continue;
            for (j = 0; j < npicked; j++)
                if (picked[j].id == (unsigned)id)
                    dup = 1;
            if (dup)
                continue;
            if (score < 0)   score = 0;
            if (score > 100) score = 100;

            picked[npicked].id = (unsigned)id;
            picked[npicked].score = (unsigned)score;
            picked[npicked].reason = clean_copy(json_str(d, "reason", ""), 900);
            if (!picked[npicked].reason)
                continue;
            npicked++;
        }
    }

    if (!db_begin()) {
        str_copy(err, errsz, "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM post_ai_departments WHERE post_id = %u", post_id);
    ok = db_exec_buf(&sql);
    buf_free(&sql);

    if (ok) {
        buf_init(&sql);
        db_sqlf(&sql,
                "INSERT INTO post_ai (post_id, summary, tags, host, deadline, model, analyzed_at) "
                "VALUES (%u, %Q, %Q, %Q, %Q, %Q, NOW()) "
                "ON DUPLICATE KEY UPDATE summary = VALUES(summary), tags = VALUES(tags), "
                "host = VALUES(host), deadline = VALUES(deadline), model = VALUES(model), "
                "analyzed_at = NOW()",
                post_id, summary, tagjson.data, host[0] ? host : NULL,
                is_iso_date(dl) ? dl : NULL, AI_MODEL);
        ok = db_exec_buf(&sql);
        buf_free(&sql);
    }

    for (i = 0; ok && i < npicked; i++) {
        buf_init(&sql);
        db_sqlf(&sql,
                "INSERT INTO post_ai_departments (post_id, department_id, score, reason) "
                "VALUES (%u, %u, %u, %Q)",
                post_id, picked[i].id, picked[i].score, picked[i].reason);
        ok = db_exec_buf(&sql);
        buf_free(&sql);
    }

    if (ok) {
        ok = db_commit();
    } else {
        db_rollback();
    }
    if (!ok)
        str_copy(err, errsz, "분석 결과를 저장할 수 없습니다.");
    else
        log_info("AI 분석 저장: 게시물 %u (관련 학과 %d개, 태그 %d개)", post_id, npicked, ntags);

out:
    for (i = 0; i < npicked; i++)
        free(picked[i].reason);
    buf_free(&tagjson);
    free(summary);
    free(host);
    free(dl);
    return ok;
}

/* ------------------------------------------------------------ 분석 한 건 */

static int analyze_with(unsigned post_id, const DeptTable *t, const char *system_prompt,
                        char *err, size_t errsz)
{
    static const char *KIND_KO[] = { "contest", "공모전", "hackathon", "해커톤", "etc", "기타" };
    MYSQL_RES *qr;
    MYSQL_ROW row;
    Buf sql, user, req, resp;
    Json *result = NULL;
    DWORD status = 0;
    const char *kind_ko = "공모전";
    int attempt, ok = 0, i;

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT p.kind, p.title, p.body, DATE_FORMAT(p.created_at, '%%Y-%%m-%%d'), "
            "       IFNULL(DATE_FORMAT(n.published_at, '%%Y-%%m-%%d'), '') "
            "FROM posts p LEFT JOIN notices n ON n.post_id = p.id WHERE p.id = %u", post_id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        str_copy(err, errsz, "게시물을 읽을 수 없습니다.");
        return 0;
    }
    row = mysql_fetch_row(qr);
    if (!row) {
        mysql_free_result(qr);
        str_copy(err, errsz, "게시물이 없습니다.");
        return 0;
    }

    for (i = 0; i < 6; i += 2)
        if (strcmp(row[0], KIND_KO[i]) == 0)
            kind_ko = KIND_KO[i + 1];

    buf_init(&user);
    buf_printf(&user, "게시일: %s\n종류: %s\n제목: %s\n\n본문:\n",
               row[4][0] ? row[4] : row[3], kind_ko, row[1]);
    buf_puts(&user, row[2]);
    mysql_free_result(qr);

    buf_init(&req);
    build_request(&req, system_prompt, user.data);
    buf_free(&user);

    buf_init(&resp);
    for (attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        buf_reset(&resp);
        err[0] = '\0';
        if (anthropic_post(req.data, &resp, &status, err, errsz) && !retryable(status))
            break;
        if (attempt < MAX_RETRIES) {
            log_warn("Claude API 재시도 %d (게시물 %u, HTTP %lu) %s",
                     attempt + 1, post_id, status, err);
            Sleep(attempt == 0 ? 3000 : 10000);
        }
    }
    buf_free(&req);

    if (status) {
        result = extract_result(resp.data, status, err, errsz);
        if (result) {
            ok = save_result(post_id, result, t, err, errsz);
            json_free(result);
        }
    } else if (!err[0]) {
        str_copy(err, errsz, "Claude API 에 연결할 수 없습니다.");
    }
    buf_free(&resp);
    return ok;
}

int ai_analyze_post(unsigned post_id, char *err, size_t errsz)
{
    DeptTable t;
    Buf system_prompt;
    int ok;

    err[0] = '\0';
    if (!ai_enabled()) {
        str_copy(err, errsz, "ANTHROPIC_API_KEY 가 설정되지 않았습니다.");
        return 0;
    }
    if (!load_departments(&t)) {
        buf_free(&t.listing);
        str_copy(err, errsz, "학과 목록을 읽을 수 없습니다.");
        return 0;
    }
    buf_init(&system_prompt);
    build_system_prompt(&system_prompt, &t);
    ok = analyze_with(post_id, &t, system_prompt.data, err, errsz);
    buf_free(&system_prompt);
    buf_free(&t.listing);
    return ok;
}

/* ------------------------------------------------------------ 일괄 작업 */

/* 분석 대상 게시물 번호를 모은다. 호출자가 free. */
static unsigned *collect_targets(int all, int *n)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    unsigned *ids;
    my_ulonglong rows;
    int k = 0;

    *n = 0;
    qr = db_query(all
        ? "SELECT p.id FROM posts p ORDER BY p.id"
        : "SELECT p.id FROM posts p LEFT JOIN post_ai a ON a.post_id = p.id "
          "WHERE a.post_id IS NULL ORDER BY p.id");
    if (!qr)
        return NULL;
    rows = mysql_num_rows(qr);
    ids = (unsigned *)malloc((size_t)(rows ? rows : 1) * sizeof *ids);
    if (ids)
        while ((row = mysql_fetch_row(qr)) != NULL)
            ids[k++] = (unsigned)strtoul(row[0], NULL, 10);
    mysql_free_result(qr);
    *n = k;
    return ids;
}

static int run_job(int force)
{
    unsigned failed_ids[512];
    int nfailed = 0, pass;
    DeptTable t;
    Buf system_prompt;

    EnterCriticalSection(&g_lock);
    g_status.running = 1;
    g_status.total = g_status.done = g_status.failed = 0;
    g_status.last_error[0] = '\0';
    LeaveCriticalSection(&g_lock);

    buf_init(&system_prompt);
    if (!load_departments(&t)) {
        EnterCriticalSection(&g_lock);
        str_copy(g_status.last_error, sizeof g_status.last_error, "학과 목록을 읽을 수 없습니다.");
        LeaveCriticalSection(&g_lock);
        goto finish;
    }
    build_system_prompt(&system_prompt, &t);

    /* 작업 도중 새로 올라온 게시물도 이어서 처리한다. 실패한 글은 이번 작업에서 다시 집지 않는다. */
    for (pass = 0; ; pass++) {
        int n, i, j, todo = 0;
        unsigned *ids = collect_targets(force && pass == 0, &n);

        if (!ids)
            break;
        for (i = 0; i < n; i++) {
            for (j = 0; j < nfailed; j++)
                if (failed_ids[j] == ids[i])
                    break;
            if (j == nfailed)
                ids[todo++] = ids[i];
        }
        if (!todo) {
            free(ids);
            break;
        }

        EnterCriticalSection(&g_lock);
        g_status.total += todo;
        LeaveCriticalSection(&g_lock);
        log_info("AI 분석 시작: %d건", todo);

        for (i = 0; i < todo; i++) {
            char err[256];
            int ok = analyze_with(ids[i], &t, system_prompt.data, err, sizeof err);

            EnterCriticalSection(&g_lock);
            if (ok) {
                g_status.done++;
            } else {
                g_status.failed++;
                str_copy(g_status.last_error, sizeof g_status.last_error, err);
            }
            LeaveCriticalSection(&g_lock);

            if (!ok) {
                log_warn("AI 분석 실패: 게시물 %u - %s", ids[i], err);
                if (nfailed < (int)(sizeof failed_ids / sizeof failed_ids[0]))
                    failed_ids[nfailed++] = ids[i];
            }
        }
        free(ids);
    }

finish:
    buf_free(&system_prompt);
    buf_free(&t.listing);
    {
        time_t now = time(NULL);
        struct tm tmv;
        int failed;

        localtime_s(&tmv, &now);
        EnterCriticalSection(&g_lock);
        strftime(g_status.finished_at, sizeof g_status.finished_at, "%Y-%m-%d %H:%M:%S", &tmv);
        g_status.running = 0;
        failed = g_status.failed;
        log_info("AI 분석 끝: 성공 %d, 실패 %d", g_status.done, g_status.failed);
        LeaveCriticalSection(&g_lock);
        return failed;
    }
}

static volatile LONG g_busy;     /* 작업이 하나만 돌도록 */

static DWORD WINAPI worker(LPVOID arg)
{
    db_thread_begin();
    run_job((int)(INT_PTR)arg);
    db_thread_end();
    InterlockedExchange(&g_busy, 0);
    return 0;
}

int ai_start_background(int force)
{
    HANDLE th;

    ensure_init();
    if (!ai_enabled())
        return -1;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return 0;

    th = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)force, 0, NULL);
    if (!th) {
        InterlockedExchange(&g_busy, 0);
        log_err("AI 분석 스레드를 만들 수 없습니다 (%lu)", GetLastError());
        return 0;
    }
    CloseHandle(th);
    return 1;
}

int ai_run_sync(int force)
{
    int failed;

    ensure_init();
    if (!ai_enabled()) {
        log_err("ANTHROPIC_API_KEY 가 설정되지 않아 AI 분석을 할 수 없습니다.");
        return -1;
    }
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return -1;
    failed = run_job(force);
    InterlockedExchange(&g_busy, 0);
    return failed;
}
