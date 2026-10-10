/* 디스패처, 정적 파일 제공, 인증(가입/로그인/세션), 학과 목록
 *
 * 이 파일이 담당하는 API:
 *   POST /api/register/send-code     학교 이메일로 6자리 인증 코드 발송
 *   POST /api/register/verify-code   인증 코드 확인
 *   POST /api/register               회원가입
 *   POST /api/login                  로그인 (세션 쿠키 발급)
 *   POST /api/logout                 로그아웃 (세션 삭제)
 *   GET  /api/me                     현재 로그인한 사용자 정보
 *   GET  /api/departments            단과대학별 학과 목록 (가입 화면의 선택 상자용)
 * 그리고 /api/ 가 아닌 모든 GET 요청은 www 폴더의 정적 파일로 응답한다.
 *
 * 세션 방식 인증 흐름:
 *   1) 로그인 성공 → 추측 불가능한 난수 토큰을 만들어 sessions 테이블에 (토큰, 사용자, 만료시각) 저장
 *   2) 토큰을 "sid" 쿠키로 브라우저에 보낸다
 *   3) 이후 모든 요청에 브라우저가 쿠키를 자동으로 붙여 보낸다
 *   4) 서버는 쿠키의 토큰으로 sessions 를 조회해 누구인지 알아낸다 (auth_current)
 */
#include "api.h"
#include "db.h"
#include "ai.h"
#include "mailer.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char           g_webroot[260] = "www";               /* 정적 파일 폴더 (260 = Windows MAX_PATH) */
static char           g_canonical_host[128] = "www.sku14.com";  /* 대표 호스트 이름 */
static unsigned short g_canonical_port = 80;                /* 대표 주소의 포트 */

void api_set_webroot(const char *dir)
{
    str_copy(g_webroot, sizeof g_webroot, dir);
}

void api_set_canonical_host(const char *host, unsigned short port)
{
    str_copy(g_canonical_host, sizeof g_canonical_host, host ? host : "");
    g_canonical_port = port;
}

/* 요청이 "www." 를 뗀 정규 호스트로 들어왔으면 www 쪽으로 보낼 주소를 만든다.
 * 그 외(127.0.0.1, localhost, 이미 www 인 경우)는 건드리지 않고 0 을 돌려준다.
 * 예: http://sku14.com/board?page=2  →  http://www.sku14.com/board?page=2 */
static int canonical_redirect_url(const Request *req, char *out, size_t outsz)
{
    char host[128];
    const char *bare;

    /* 대표 호스트가 www. 로 시작할 때만 의미가 있다 */
    if (!g_canonical_host[0] || strncmp(g_canonical_host, "www.", 4) != 0)
        return 0;

    bare = g_canonical_host + 4;              /* "sku14.com" */
    host_name_only(req->host, host, sizeof host);   /* Host 헤더에서 포트 제거 */
    if (!str_ieq(host, bare))                 /* www 없는 이름으로 온 요청만 넘긴다 */
        return 0;

    if (g_canonical_port == 80)               /* 80 포트는 주소에 쓰지 않는 것이 관례 */
        snprintf(out, outsz, "http://%s%s", g_canonical_host, req->path);
    else
        snprintf(out, outsz, "http://%s:%u%s", g_canonical_host,
                 g_canonical_port, req->path);

    if (req->query[0]) {                      /* 쿼리 문자열도 그대로 보존 */
        size_t n = strlen(out);
        snprintf(out + n, outsz - n, "?%s", req->query);
    }
    return 1;
}

/* ------------------------------------------------------------------ 인증 */

/* 세션 쿠키 → 사용자 정보. 설명은 api.h 참고. */
int auth_current(const Request *req, CurrentUser *u)
{
    char token[SESSION_TOKEN_LEN + 1];
    Buf sql;
    MYSQL_RES *res;
    MYSQL_ROW row;

    memset(u, 0, sizeof *u);   /* 기본은 비로그인(id = 0) */
    if (!req_cookie(req, SESSION_COOKIE, token, sizeof token) || !token[0])
        return 0;

    /* 세션 → 사용자 → 학과 → 단과대학을 한 번에 조인한다.
     * 학과가 없는 관리자도 나오도록 학과/단과대학은 LEFT JOIN.
     * 만료된 세션(expires_at <= NOW())은 조건에서 걸러지므로 자동으로 로그아웃된 셈이 된다.
     * u.role = 'admin' 은 비교식이라 MySQL 에서 1 또는 0 으로 나온다. */
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT u.id, u.student_no, u.name, u.nickname, "
            "       IFNULL(u.department_id, 0), IFNULL(d.name, ''), IFNULL(c.name, ''), "
            "       u.role = 'admin' "
            "FROM sessions s "
            "JOIN users u ON u.id = s.user_id "
            "LEFT JOIN departments d ON d.id = u.department_id "
            "LEFT JOIN colleges c ON c.id = d.college_id "
            "WHERE s.token = %Q AND s.expires_at > NOW()", token);

    res = db_query_buf(&sql);
    buf_free(&sql);
    if (!res)
        return 0;

    row = mysql_fetch_row(res);
    if (row) {
        u->id = (unsigned)strtoul(row[0], NULL, 10);
        str_copy(u->student_no, sizeof u->student_no, row[1]);
        str_copy(u->name, sizeof u->name, row[2]);
        str_copy(u->nickname, sizeof u->nickname, row[3]);
        u->department_id = (unsigned)strtoul(row[4], NULL, 10);
        str_copy(u->department_name, sizeof u->department_name, row[5]);
        str_copy(u->college_name, sizeof u->college_name, row[6]);
        u->is_admin = (row[7][0] == '1');
    }
    mysql_free_result(res);
    return u->id != 0;
}

/* 로그인 필수 검사 */
int auth_require(const Request *req, Response *res, CurrentUser *u)
{
    if (auth_current(req, u))
        return 1;
    res_error(res, 401, "login_required", "로그인이 필요합니다.");
    return 0;
}

/* 관리자 필수 검사 (먼저 로그인 여부, 그다음 권한) */
int auth_require_admin(const Request *req, Response *res, CurrentUser *u)
{
    if (!auth_require(req, res, u))
        return 0;
    if (u->is_admin)
        return 1;
    res_error(res, 403, "admin_only", "관리자만 할 수 있습니다.");
    return 0;
}

/* 요청 본문을 JSON 객체로 파싱. 객체가 아니면(배열, 숫자 등) 거절한다. */
Json *body_object(const Request *req, Response *res)
{
    Json *j;

    if (!req->body || !req->body_len) {
        res_error(res, 400, "empty_body", "요청 본문이 비었습니다.");
        return NULL;
    }
    j = json_parse(req->body);
    if (!j || j->type != JS_OBJ) {
        json_free(j);
        res_error(res, 400, "bad_json", "JSON 형식이 올바르지 않습니다.");
        return NULL;
    }
    return j;
}

/* [1, "2", 3] 같은 배열에서 양의 정수만 모은다. 설명은 api.h 참고. */
int json_uint_array(const Json *arr, unsigned *out, int max)
{
    int i, n = 0;

    if (!arr || arr->type != JS_ARR)
        return 0;
    for (i = 0; i < arr->n && n < max; i++) {
        const Json *v = arr->items[i];
        long id = 0;

        if (!v)
            continue;
        if (v->type == JS_NUM)
            id = (long)v->num;
        else if (v->type == JS_STR)              /* 폼 값처럼 문자열로 온 숫자도 받아 준다 */
            id = strtol(v->str, NULL, 10);

        if (id > 0)
            out[n++] = (unsigned)id;
    }
    return n;
}

/* 사용자 정보를 JSON 객체로 쓴다.
 * 비밀번호 해시·전화번호·이메일 같은 민감 정보는 여기 넣지 않는다. */
static void write_user(Buf *b, const CurrentUser *u)
{
    buf_putc(b, '{');
    json_kv_int(b, "id", u->id);
    buf_putc(b, ',');
    json_kv_str(b, "student_no", u->student_no);
    buf_putc(b, ',');
    json_kv_str(b, "name", u->name);
    buf_putc(b, ',');
    json_kv_str(b, "nickname", u->nickname);
    buf_putc(b, ',');
    json_kv_int(b, "department_id", u->department_id);
    buf_putc(b, ',');
    json_kv_str(b, "department_name", u->department_name[0] ? u->department_name : NULL);
    buf_putc(b, ',');
    json_kv_str(b, "college_name", u->college_name[0] ? u->college_name : NULL);
    buf_putc(b, ',');
    json_kv_bool(b, "is_admin", u->is_admin);
    buf_putc(b, '}');
}

/* ---------------------------------------------------------------- 가입 */

/* ------------------------------------------------- 게시판 표시 이름 만들기
 *
 * 실명과 전화번호를 그대로 드러내지 않으면서 같은 학과 안에서 서로를 알아볼 수
 * 있을 만큼만 남긴다.
 *
 *   손동권 + 010-9948-9687  ->  손*권_9948
 *
 * 이름은 가운데 글자를 가리고(한글은 UTF-8 3바이트이므로 글자 단위로 센다),
 * 전화번호는 가운데 묶음만 쓴다. */

/* UTF-8 글자 하나의 바이트 수 (첫 바이트의 앞쪽 비트 패턴으로 판단)
 *   0xxxxxxx → 1,  110xxxxx → 2,  1110xxxx → 3,  11110xxx → 4 */
static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;                      /* 깨진 바이트는 1바이트로 보고 넘어간다 */
}

/* 이름을 손*권 형태로 가린다.
 *   1글자  -> 그대로        2글자 -> 손*
 *   3글자  -> 손*권         4글자 이상 -> 첫 글자 + * 여러 개 + 끝 글자 */
static void mask_name(const char *name, Buf *out)
{
    const char *starts[32];   /* 각 글자의 시작 위치 */
    int lens[32];             /* 각 글자의 바이트 수 */
    int n = 0;                /* 글자 수 (최대 32글자까지만 셈) */
    const char *p = name;
    int i;

    /* 1) 바이트열을 글자 단위로 쪼갠다 */
    while (*p && n < 32) {
        int len = utf8_len((unsigned char)*p);
        starts[n] = p;
        lens[n] = len;
        n++;
        p += len;
    }

    if (n <= 1) {
        buf_puts(out, name);
        return;
    }

    /* 2) 첫 글자 + 가운데 글자 수만큼 '*' */
    buf_add(out, starts[0], (size_t)lens[0]);
    for (i = 1; i < n - 1; i++)
        buf_putc(out, '*');

    /* 3) 끝 글자 (2글자 이름은 끝 글자를 보이면 전부 보이므로 가린다) */
    if (n == 2)
        buf_putc(out, '*');        /* 2글자는 끝 글자를 가린다 */
    else
        buf_add(out, starts[n - 1], (size_t)lens[n - 1]);
}

/* 전화번호에서 가운데 묶음을 꺼낸다. 010-9948-9687 -> 9948
 * 숫자만 남겨 11자리면 4자리, 10자리면 3자리를 쓴다. 형식이 아니면 0. */
static int phone_middle(const char *phone, char *out, size_t outsz)
{
    char digits[24];
    int n = 0;
    int start, count;

    /* 하이픈·공백 등을 무시하고 숫자만 모은다 */
    for (; *phone && n < (int)sizeof digits - 1; phone++)
        if (isdigit((unsigned char)*phone))
            digits[n++] = *phone;
    digits[n] = '\0';

    if (n == 11)      { start = 3; count = 4; }   /* 010-9948-9687 */
    else if (n == 10) { start = 3; count = 3; }   /* 010-123-4567  */
    else              return 0;

    if ((size_t)count >= outsz)
        return 0;
    memcpy(out, digits + start, (size_t)count);
    out[count] = '\0';
    return 1;
}

/* 표시 이름을 만든다. 성공 시 1. (예: "손*권_9948") */
static int make_display_name(const char *name, const char *phone, Buf *out)
{
    char mid[8];

    if (!phone_middle(phone, mid, sizeof mid))
        return 0;

    buf_reset(out);
    mask_name(name, out);
    buf_putc(out, '_');
    buf_puts(out, mid);
    return 1;
}

/* 학번은 숫자만, 4~20자 */
static int valid_student_no(const char *s)
{
    size_t n = strlen(s);
    size_t i;

    if (n < 4 || n > 20)
        return 0;
    for (i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

/* ---------------------------------------------------- 학교 이메일 인증
 *
 *   POST /api/register/send-code   {email}        인증 코드 6자리를 보낸다 (10분 유효, 60초에 한 번)
 *   POST /api/register/verify-code {email, code}  [확인] 버튼. 맞으면 인증 완료 (30분 안에 가입)
 *   POST /api/register {..., email}               인증을 마친 이메일이어야 가입된다
 *                                                 (확인을 건너뛰고 code 를 함께 보내도 된다)
 * 코드는 5번 틀리면 다시 받아야 한다. 소금 친 해시로만 저장하고, 가입에 성공하면 지운다.
 *
 * 왜 이메일 인증을 하는가? 서경대 학생만 가입할 수 있게 하기 위해서다.
 * @skuniv.ac.kr 메일함은 학교 구성원만 열 수 있으므로, 거기로 간 코드를 입력할 수 있다면
 * 학교 구성원이라고 볼 수 있다. */

#define SCHOOL_DOMAIN    "@skuniv.ac.kr"   /* 허용하는 이메일 도메인 */
#define CODE_TTL_MIN     10                /* 인증 코드 유효 시간(분) */
#define VERIFIED_TTL_MIN 30                /* 인증 완료 후 가입을 마쳐야 하는 시간(분) */
#define CODE_RESEND_SEC  60                /* 재발송 최소 간격(초) - 메일 폭탄 방지 */
#define CODE_MAX_TRIES   5                 /* 틀릴 수 있는 최대 횟수 - 무차별 대입 방지 */

/* 학교 이메일만 받는다. 소문자로 바꿔 out 에 담는다. */
static int school_email(const char *in, char *out, size_t outsz)
{
    size_t n = strlen(in), dn = strlen(SCHOOL_DOMAIN), i;

    /* 도메인보다 길어야 하고(아이디가 있어야), 버퍼와 상한(120자)을 넘지 않아야 한다 */
    if (n <= dn || n >= outsz || n > 120)
        return 0;
    for (i = 0; i < n; i++)                 /* 이메일은 대소문자를 구분하지 않으므로 소문자로 통일 */
        out[i] = (char)tolower((unsigned char)in[i]);
    out[n] = '\0';
    if (strcmp(out + n - dn, SCHOOL_DOMAIN) != 0)   /* 끝이 @skuniv.ac.kr 인지 */
        return 0;
    for (i = 0; i < n - dn; i++) {          /* 아이디 부분: 영문 소문자·숫자·. _ - */
        char c = out[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    /* 아이디가 점으로 시작하거나 끝나면 안 된다 */
    return out[0] != '.' && out[n - dn - 1] != '.';
}

/* POST /api/register/send-code {"email":"..."} */
static void handle_send_code(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    char email[128], code[8], hex[9], salt[PW_SALT_HEX_LEN + 1], hash[PW_HASH_HEX_LEN + 1], err[200];
    Buf sql, text;
    long long wait;   /* 재발송까지 남은 초 */

    if (!in)
        return;
    if (!school_email(json_str(in, "email", ""), email, sizeof email)) {
        json_free(in);
        res_error(res, 400, "bad_email", "학교 이메일(@skuniv.ac.kr)만 쓸 수 있습니다.");
        return;
    }
    json_free(in);
    /* 메일을 보낼 수단이 아예 없으면 코드를 만들어도 소용없으므로 먼저 알린다 */
    if (!mail_enabled() && !mail_dev_mode()) {
        log_err("메일 서버(SKU_SMTP_URL)가 설정되지 않아 인증 메일을 보낼 수 없습니다. smtp.env 를 확인하세요.");
        res_error(res, 503, "mail_not_configured",
                  "메일 서버가 설정되지 않아 지금은 인증 메일을 보낼 수 없습니다. 관리자에게 문의하세요.");
        return;
    }

    /* 이미 가입한 이메일이면 코드를 보내지 않는다 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM users WHERE email = %Q", email);
    if (db_scalar(sql.data, 0) > 0) {
        buf_free(&sql);
        res_error(res, 409, "duplicate_email", "이미 가입한 이메일입니다.");
        return;
    }
    /* 재발송 제한: 60 - (지난번 발송 후 지난 초). 양수면 아직 기다려야 한다.
     * 기록이 없으면 결과 행이 없어 db_scalar 가 기본값 0 을 돌려준다. */
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT %d - TIMESTAMPDIFF(SECOND, sent_at, NOW()) FROM email_verifications "
                  "WHERE email = %Q", CODE_RESEND_SEC, email);
    wait = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (wait > 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "%lld초 뒤에 다시 보낼 수 있습니다.", wait);
        res_error(res, 429, "too_soon", msg);   /* 429 Too Many Requests */
        return;
    }

    /* 6자리 코드 (OS 난수원)
     * 16진 8글자(32비트) 난수를 정수로 바꿔 1,000,000 으로 나눈 나머지를 쓴다.
     * %06lu 로 앞자리 0 을 채워 항상 6자리가 되게 한다 (예: 7 → "000007"). */
    if (!random_hex(hex, 8) || !random_hex(salt, PW_SALT_HEX_LEN)) {
        res_error(res, 500, "no_entropy", "보안 처리에 실패했습니다. 잠시 후 다시 시도하세요.");
        return;
    }
    snprintf(code, sizeof code, "%06lu", strtoul(hex, NULL, 16) % 1000000ul);
    /* 코드도 비밀번호처럼 해시로만 저장한다 (DB 가 유출돼도 코드를 바로 알 수 없게) */
    pw_hash(salt, code, hash);

    /* 메일 본문을 만들어 보낸다 */
    buf_init(&text);
    buf_printf(&text,
               "SKU14 공모전 팀원 모집 커뮤니티 가입 인증 코드입니다.\n\n"
               "    %s\n\n"
               "가입 화면에 이 코드를 입력하세요. %d분 동안 유효합니다.\n"
               "직접 요청하지 않았다면 이 메일은 무시하세요.\n", code, CODE_TTL_MIN);
    if (!mail_send(email, "[SKU14] 이메일 인증 코드", text.data, err, sizeof err)) {
        buf_free(&text);
        res_error(res, 502, "mail_failed", err);
        return;
    }
    buf_free(&text);

    /* 발송에 성공한 뒤에만 저장한다 (보내지 못한 코드가 남지 않게).
     * 이메일이 기본 키라서, 이미 행이 있으면 ON DUPLICATE KEY UPDATE 로 새 코드로 덮어쓰고
     * 틀린 횟수·인증 상태를 초기화한다. */
    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO email_verifications (email, code_salt, code_hash, attempts, verified_at, "
            "                                 sent_at, expires_at) "
            "VALUES (%Q, %Q, %Q, 0, NULL, NOW(), NOW() + INTERVAL %d MINUTE) "
            "ON DUPLICATE KEY UPDATE code_salt = VALUES(code_salt), code_hash = VALUES(code_hash), "
            "attempts = 0, verified_at = NULL, sent_at = NOW(), expires_at = VALUES(expires_at)",
            email, salt, hash, CODE_TTL_MIN);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "인증 코드를 저장하지 못했습니다.");
        return;
    }
    buf_free(&sql);

    /* 화면이 남은 시간 카운트다운을 보여 줄 수 있게 초 단위 정보를 돌려준다 */
    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "expires_in", CODE_TTL_MIN * 60);   buf_putc(&b, ',');
        json_kv_int(&b, "resend_in", CODE_RESEND_SEC);      buf_putc(&b, ',');
        json_kv_bool(&b, "dev_mode", mail_dev_mode());      /* 시험용 개발 모드면 코드는 서버 로그에 */
        buf_putc(&b, '}');
        res_json_buf(res, 200, &b);
        buf_free(&b);
    }
}

/* 인증 코드를 확인한다. 맞으면 1, 아니면 응답을 채우고 0 (틀리면 시도 횟수를 늘린다). */
static int check_code(const char *email, const char *code, Response *res)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char salt[PW_SALT_HEX_LEN + 1] = { 0 }, want[PW_HASH_HEX_LEN + 1] = { 0 }, got[PW_HASH_HEX_LEN + 1];
    int attempts = 0, expired = 1, found = 0;
    size_t i;

    /* 6자리 숫자가 아니면 기회를 깎지 않고 돌려보낸다. */
    for (i = 0; code[i] && isdigit((unsigned char)code[i]); i++)
        ;   /* 앞에서부터 숫자가 몇 개 이어지는지 센다 (빈 반복문) */
    if (i != 6 || code[i]) {   /* 정확히 6자리이고 그 뒤에 아무것도 없어야 한다 */
        res_error(res, 400, "bad_code_format", "메일로 받은 6자리 인증 코드를 입력하세요.");
        return 0;
    }

    /* 저장된 솔트·해시·시도 횟수·만료 여부를 읽는다 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT code_salt, code_hash, attempts, expires_at < NOW() "
                  "FROM email_verifications WHERE email = %Q", email);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        if ((row = mysql_fetch_row(qr)) != NULL) {
            found = 1;
            str_copy(salt, sizeof salt, row[0]);
            str_copy(want, sizeof want, row[1]);
            attempts = atoi(row[2]);
            expired = atoi(row[3]);
        }
        mysql_free_result(qr);
    }
    if (!found) {
        res_error(res, 400, "no_code", "먼저 이메일 인증 코드를 받으세요.");
        return 0;
    }
    if (expired || attempts >= CODE_MAX_TRIES) {
        res_error(res, 400, "code_expired", "인증 코드가 만료되었습니다. 다시 받으세요.");
        return 0;
    }
    /* 입력한 코드를 같은 솔트로 해시해 저장된 해시와 비교 */
    pw_hash(salt, code, got);
    if (strcmp(got, want) != 0) {
        char msg[96];
        /* 틀린 횟수 +1 (attempts = attempts + 1 은 DB 안에서 원자적으로 증가) */
        buf_init(&sql);
        db_sqlf(&sql, "UPDATE email_verifications SET attempts = attempts + 1 WHERE email = %Q", email);
        db_exec_buf(&sql);
        buf_free(&sql);
        snprintf(msg, sizeof msg, "인증 코드가 맞지 않습니다. (남은 기회 %d번)",
                 CODE_MAX_TRIES - attempts - 1);
        res_error(res, 400, "bad_code", msg);
        return 0;
    }
    return 1;
}

/* [확인] 으로 인증을 마쳤고 아직 유효한 이메일이면 1 */
static int email_verified(const char *email)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM email_verifications WHERE email = %Q "
                  "AND verified_at IS NOT NULL AND expires_at >= NOW()", email);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

/* POST /api/register/verify-code {"email":"...","code":"123456"} */
static void handle_verify_code(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    char email[128];
    Buf sql, b;

    if (!in)
        return;
    if (!school_email(json_str(in, "email", ""), email, sizeof email)) {
        json_free(in);
        res_error(res, 400, "bad_email", "학교 이메일(@skuniv.ac.kr)만 쓸 수 있습니다.");
        return;
    }
    /* 이미 확인한 주소면 다시 맞출 필요 없다 */
    if (!email_verified(email)) {
        if (!check_code(email, json_str(in, "code", ""), res)) {
            json_free(in);
            return;
        }
        /* 인증 완료 표시. 만료 시각을 "지금부터 30분" 으로 새로 잡아 가입할 시간을 준다 */
        buf_init(&sql);
        db_sqlf(&sql, "UPDATE email_verifications SET verified_at = NOW(), "
                      "expires_at = NOW() + INTERVAL %d MINUTE WHERE email = %Q", VERIFIED_TTL_MIN, email);
        if (!db_exec_buf(&sql)) {
            buf_free(&sql);
            json_free(in);
            res_error(res, 500, "db_error", "인증 결과를 저장하지 못했습니다.");
            return;
        }
        buf_free(&sql);
        log_info("이메일 인증 완료: %s", email);
    }
    json_free(in);

    buf_init(&b);
    buf_puts(&b, "{\"ok\":true,\"verified\":true,");
    json_kv_int(&b, "expires_in", VERIFIED_TTL_MIN * 60);
    buf_putc(&b, '}');
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* POST /api/register
 * 본문: {student_no, name, phone, password, department_id, email, code?, interest_ids?, bio?}
 * 검사 순서: 형식 검사(학번→이름→전화→비밀번호→이메일→키워드→학과) → 이메일 인증 → 저장
 * goto out 패턴: 어느 단계에서 실패하든 한 곳(out:)에서 메모리를 정리한다. */
static void handle_register(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    const char *student_no, *name, *phone, *password;
    char email[128];
    unsigned dept_id;
    char salt[PW_SALT_HEX_LEN + 1];
    char hash[PW_HASH_HEX_LEN + 1];
    Buf sql, display;          /* display: 서버가 만든 표시 이름 */
    long long dept_exists;
    unsigned tag_ids[16];      /* 관심 키워드 id (8개 초과 검사를 위해 16칸) */
    int ntags;

    if (!in)
        return;

    buf_init(&display);

    student_no = json_str(in, "student_no", "");
    name       = json_str(in, "name", "");
    phone      = json_str(in, "phone", "");
    password   = json_str(in, "password", "");
    dept_id    = (unsigned)json_int(in, "department_id", 0);

    if (!valid_student_no(student_no)) {
        res_error(res, 400, "bad_student_no", "학번은 숫자 4~20자로 입력하세요.");
        goto out;
    }
    /* 바이트 길이 기준: 한글 2글자 = 6바이트 이상, 40바이트 ≒ 한글 13글자 */
    if (strlen(name) < 2 || strlen(name) > 40) {
        res_error(res, 400, "bad_name", "이름을 입력하세요.");
        goto out;
    }
    /* 표시 이름은 사용자가 정하지 않는다. 이름과 전화번호로 서버가 만든다. */
    if (!make_display_name(name, phone, &display)) {
        res_error(res, 400, "bad_phone",
                  "전화번호를 010-0000-0000 형식으로 입력하세요.");
        goto out;
    }
    if (strlen(password) < 8) {
        res_error(res, 400, "weak_password", "비밀번호는 8자 이상이어야 합니다.");
        goto out;
    }
    if (!school_email(json_str(in, "email", ""), email, sizeof email)) {
        res_error(res, 400, "bad_email", "학교 이메일(@skuniv.ac.kr)을 입력하세요.");
        goto out;
    }
    /* 관심 키워드는 선택 (0~8개). 고르면 팀원 추천과 개인 맞춤 공모전 추천의 근거가 된다. */
    ntags = json_uint_array(json_get(in, "interest_ids"), tag_ids,
                            (int)(sizeof tag_ids / sizeof tag_ids[0]));
    if (ntags > 8) {
        res_error(res, 400, "bad_interests", "관심 키워드는 8개까지 고를 수 있습니다.");
        goto out;
    }

    /* 체크된 학과와 대조할 수 있어야 하므로 소속 학과는 필수다. */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM departments WHERE id = %u", dept_id);
    dept_exists = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (!dept_exists) {
        res_error(res, 400, "bad_department", "소속 학과를 선택하세요.");
        goto out;
    }

    /* 다른 항목을 다 확인한 뒤 마지막에 이메일 인증을 본다 (입력 실수로 기회를 깎지 않게).
     * [확인] 으로 이미 인증했으면 통과, 아니면 함께 보낸 코드를 맞춰 본다. */
    if (!email_verified(email)) {
        if (!json_str(in, "code", "")[0]) {
            res_error(res, 400, "not_verified", "이메일 인증 코드를 입력하고 [확인]을 눌러 주세요.");
            goto out;
        }
        if (!check_code(email, json_str(in, "code", ""), res))
            goto out;
    }

    /* 비밀번호 해시 만들기: 사용자마다 새 솔트 */
    if (!random_hex(salt, PW_SALT_HEX_LEN)) {
        res_error(res, 500, "no_entropy", "보안 처리에 실패했습니다. 잠시 후 다시 시도하세요.");
        goto out;
    }
    pw_hash(salt, password, hash);

    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO users (student_no, name, phone, email, nickname, department_id, "
            "                   role, pw_salt, pw_hash) "
            "VALUES (%Q, %Q, %Q, %Q, %Q, %u, 'student', %Q, %Q)",
            student_no, name, phone, email, display.data, dept_id, salt, hash);

    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        /* 유일 키 중복: 오류 메시지에 어떤 키(email/student_no)인지 나오므로 그것으로 구분한다.
         * (미리 SELECT 로 확인하는 대신 INSERT 실패로 판단하면, 동시에 두 명이 같은 학번으로
         *  가입하는 경쟁 상황에서도 DB 의 UNIQUE 제약이 정확히 하나만 통과시킨다.) */
        if (db_errno() == DB_ERR_DUP_ENTRY)
            res_error(res, 409, "duplicate",
                      strstr(db_error(), "email") ? "이미 가입한 이메일입니다." : "이미 등록된 학번입니다.");
        else
            /* 그 밖의 오류는 뭉뚱그리지 않는다. 자세한 내용은 서버 로그에 남는다. */
            res_error(res, 500, "db_error", "가입 처리 중 오류가 났습니다. 서버 로그를 확인하세요.");
        goto out;
    }
    buf_free(&sql);

    /* 계정은 만들어졌으니 키워드 저장이 실패해도 가입은 성공으로 두고 로그만 남긴다
     * (내 프로필에서 다시 고를 수 있다). */
    {
        unsigned uid = (unsigned)db_last_id();   /* 방금 INSERT 한 사용자 id */

        buf_init(&sql);
        db_sqlf(&sql, "DELETE FROM email_verifications WHERE email = %Q", email);   /* 코드는 한 번만 */
        db_exec_buf(&sql);
        buf_free(&sql);
        if (!profile_save(uid, tag_ids, ntags, json_str(in, "bio", "")))
            log_warn("가입한 회원 %s 의 관심 키워드를 저장하지 못했습니다.", student_no);
    }

    /* 201 Created + 만들어진 표시 이름 (화면에서 "손*권_9948 으로 가입되었습니다" 안내) */
    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_str(&b, "nickname", display.data);
        buf_putc(&b, '}');
        res_json_buf(res, 201, &b);
        buf_free(&b);
    }

out:
    buf_free(&display);
    json_free(in);
}

/* ---------------------------------------------------------------- 로그인 */

/* POST /api/login {"student_no":"...","password":"..."} */
static void handle_login(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    const char *student_no, *password;
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char salt[PW_SALT_HEX_LEN + 1] = { 0 };
    char hash[PW_HASH_HEX_LEN + 1] = { 0 };
    unsigned uid = 0;
    char token[SESSION_TOKEN_LEN + 1];
    CurrentUser u;

    if (!in)
        return;

    student_no = json_str(in, "student_no", "");
    password   = json_str(in, "password", "");

    /* 학번으로 솔트와 해시를 읽는다 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT id, pw_salt, pw_hash FROM users WHERE student_no = %Q", student_no);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    if (qr) {
        row = mysql_fetch_row(qr);
        if (row) {
            uid = (unsigned)strtoul(row[0], NULL, 10);
            str_copy(salt, sizeof salt, row[1]);
            str_copy(hash, sizeof hash, row[2]);
        }
        mysql_free_result(qr);
    }

    /* 학번이 없는 경우와 비밀번호가 틀린 경우를 같은 메시지로 답한다.
     * 다르게 답하면 공격자가 "어떤 학번이 가입되어 있는지" 를 알아낼 수 있다. */
    if (!uid || !pw_verify(salt, hash, password)) {
        res_error(res, 401, "bad_credentials", "학번 또는 비밀번호가 맞지 않습니다.");
        goto out;
    }

    /* 만료된 세션을 치우고 새 세션을 만든다. (로그인할 때마다 청소해 테이블이 커지지 않게) */
    db_exec("DELETE FROM sessions WHERE expires_at <= NOW()");

    if (!random_hex(token, SESSION_TOKEN_LEN)) {
        res_error(res, 500, "no_entropy", "세션을 만들 수 없습니다.");
        goto out;
    }
    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO sessions (token, user_id, expires_at) "
            "VALUES (%Q, %u, DATE_ADD(NOW(), INTERVAL %d HOUR))",
            token, uid, SESSION_HOURS);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "session_failed", "세션을 만들 수 없습니다.");
        goto out;
    }
    buf_free(&sql);

    /* 쿠키 유효 시간 = 세션 유효 시간 (초 단위) */
    res_set_cookie(res, SESSION_COOKIE, token, SESSION_HOURS * 3600L);

    /* 응답에 사용자 정보를 함께 담는다.
     * 방금 만든 토큰을 쿠키로 가진 가짜 요청(tmp)을 만들어 auth_current 를 재사용한다.
     * 이렇게 하면 /api/me 와 똑같은 사용자 정보 형식을 별도 코드 없이 얻는다. */
    {
        Request tmp;
        Buf out_body;

        memset(&tmp, 0, sizeof tmp);
        snprintf(tmp.cookie, sizeof tmp.cookie, "%s=%s", SESSION_COOKIE, token);
        auth_current(&tmp, &u);

        buf_init(&out_body);
        buf_puts(&out_body, "{\"ok\":true,\"user\":");
        write_user(&out_body, &u);
        buf_putc(&out_body, '}');
        res_json_buf(res, 200, &out_body);
        buf_free(&out_body);
    }

out:
    json_free(in);
}

/* POST /api/logout - 서버의 세션 행을 지우고 브라우저 쿠키도 지운다(Max-Age=0) */
static void handle_logout(Request *req, Response *res)
{
    char token[SESSION_TOKEN_LEN + 1];

    if (req_cookie(req, SESSION_COOKIE, token, sizeof token) && token[0]) {
        Buf sql;
        buf_init(&sql);
        db_sqlf(&sql, "DELETE FROM sessions WHERE token = %Q", token);
        db_exec_buf(&sql);
        buf_free(&sql);
    }
    res_set_cookie(res, SESSION_COOKIE, "", 0);
    res_json(res, 200, "{\"ok\":true}");
}

/* GET /api/me - 페이지를 열 때 프론트엔드가 가장 먼저 불러 로그인 상태를 확인한다.
 * 비로그인도 오류가 아니라 {"user":null} 로 정상 응답한다. */
static void handle_me(Request *req, Response *res)
{
    CurrentUser u;
    Buf b;

    buf_init(&b);
    /* 화면이 AI 메뉴를 보일지 정할 수 있게 함께 알려준다. */
    buf_putc(&b, '{');
    json_kv_bool(&b, "ai_enabled", ai_enabled());
    buf_puts(&b, ",\"user\":");
    if (!auth_current(req, &u))
        buf_puts(&b, "null");
    else
        write_user(&b, &u);
    buf_putc(&b, '}');
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 학과 목록 */

/* GET /api/departments
 * 응답: {"colleges":[{"id":1,"name":"공과대학","departments":[{"id":3,"name":"..."},...]},...]}
 * 조인 결과는 (단과대학, 학과) 행의 평평한 목록이므로, 단과대학 id 가 바뀔 때마다
 * 새 객체를 열고 앞 객체를 닫는 방식으로 중첩 구조를 만든다. (ORDER BY 로 같은
 * 단과대학의 학과가 연속해서 나오는 것이 전제) */
static void handle_departments(Response *res)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    Buf b;
    char last_college[64] = "";   /* 직전 행의 단과대학 id (문자열로 비교) */
    int first_college = 1;

    qr = db_query(
        "SELECT c.id, c.name, d.id, d.name "
        "FROM colleges c "
        "JOIN departments d ON d.college_id = c.id "
        "ORDER BY c.sort, c.id, d.sort, d.id");
    if (!qr) {
        res_error(res, 500, "db_error", "학과 목록을 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"colleges\":[");

    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (strcmp(last_college, row[0]) != 0) {
            /* 새 단과대학 시작: 앞 단과대학의 학과 배열과 객체를 닫는다 */
            if (!first_college)
                buf_puts(&b, "]},");
            first_college = 0;
            buf_putc(&b, '{');
            json_kv_int(&b, "id", atoll(row[0]));
            buf_putc(&b, ',');
            json_kv_str(&b, "name", row[1]);
            buf_puts(&b, ",\"departments\":[");
            str_copy(last_college, sizeof last_college, row[0]);
        } else {
            buf_putc(&b, ',');            /* 같은 단과대학의 다음 학과 */
        }
        buf_putc(&b, '{');
        json_kv_int(&b, "id", atoll(row[2]));
        buf_putc(&b, ',');
        json_kv_str(&b, "name", row[3]);
        buf_putc(&b, '}');
    }
    if (!first_college)                   /* 마지막 단과대학 닫기 */
        buf_puts(&b, "]}");
    buf_puts(&b, "]}");

    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* 인증·가입·학과 목록 경로 라우터 */
int route_auth(Request *req, Response *res)
{
    if (req_is(req, "POST", "/api/register")) { handle_register(req, res); return 1; }
    if (req_is(req, "POST", "/api/register/send-code")) { handle_send_code(req, res); return 1; }
    if (req_is(req, "POST", "/api/register/verify-code")) { handle_verify_code(req, res); return 1; }
    if (req_is(req, "POST", "/api/login"))    { handle_login(req, res);    return 1; }
    if (req_is(req, "POST", "/api/logout"))   { handle_logout(req, res);   return 1; }
    if (req_is(req, "GET",  "/api/me"))       { handle_me(req, res);       return 1; }
    if (req_is(req, "GET",  "/api/departments")) { handle_departments(res); return 1; }
    return 0;
}

/* ------------------------------------------------------------ 정적 파일 */

/* 확장자 → Content-Type. 브라우저는 이 값을 보고 파일을 어떻게 해석할지 정한다.
 * (nosniff 헤더를 쓰므로 JS 파일의 타입이 틀리면 브라우저가 실행을 거부한다) */
static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');   /* 마지막 점 = 확장자 시작 */

    if (!dot)
        return "application/octet-stream";  /* 알 수 없는 이진 파일 */
    if (str_ieq(dot, ".html")) return "text/html; charset=utf-8";
    if (str_ieq(dot, ".css"))  return "text/css; charset=utf-8";
    if (str_ieq(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (str_ieq(dot, ".json")) return "application/json; charset=utf-8";
    if (str_ieq(dot, ".svg"))  return "image/svg+xml";
    if (str_ieq(dot, ".png"))  return "image/png";
    if (str_ieq(dot, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

/* 경로 탈출(.. 또는 절대경로)을 막는다.
 * 예: GET /../server.exe 나 /..\..\Windows\win.ini 로 webroot 밖의 파일을 읽으려는 시도,
 *     C:\ 같은 드라이브 지정(':')을 모두 거부한다. */
static int path_is_safe(const char *p)
{
    if (strstr(p, ".."))
        return 0;
    if (strchr(p, ':') || strchr(p, '\\'))
        return 0;
    return 1;
}

/* webroot 아래의 파일을 읽어 응답 본문으로 보낸다. */
static void serve_static(Request *req, Response *res)
{
    char full[sizeof g_webroot + HTTP_PATH_MAX + 2];   /* webroot + 경로 */
    const char *rel = req->path;
    FILE *f;
    long size;

    if (strcmp(rel, "/") == 0)          /* 루트 요청은 index.html 로 */
        rel = "/index.html";

    if (!path_is_safe(rel)) {
        res_error(res, 400, "bad_path", "잘못된 경로입니다.");
        return;
    }

    snprintf(full, sizeof full, "%s%s", g_webroot, rel);   /* 예: "www/app.js" */

    f = fopen(full, "rb");              /* 이진 모드: 줄바꿈 변환 없이 그대로 읽는다 */
    if (!f) {
        res_error(res, 404, "not_found", "페이지를 찾을 수 없습니다.");
        return;
    }

    /* 파일 크기 구하기: 끝으로 이동해 위치를 읽고 다시 처음으로 */
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > 8 * 1024 * 1024) {   /* 8MB 넘는 파일은 제공하지 않는다 */
        fclose(f);
        res_error(res, 500, "too_large", "파일이 너무 큽니다.");
        return;
    }

    /* 본문 버퍼에 파일 전체를 바로 읽어 넣는다 */
    buf_reset(&res->body);
    if (buf_reserve(&res->body, (size_t)size)) {
        size_t got = fread(res->body.data, 1, (size_t)size, f);
        res->body.len = got;
        res->body.data[got] = '\0';
    }
    fclose(f);

    res->status = 200;
    str_copy(res->content_type, sizeof res->content_type, mime_for(full));
    /* no-cache: 브라우저가 캐시해도 되지만 쓰기 전에 매번 서버에 확인하게 한다.
     * 개발 중 app.js 를 고쳤는데 옛 파일이 계속 보이는 문제를 막는다. */
    res_header(res, "Cache-Control", "no-cache");
}

/* ------------------------------------------------------------ 디스패처 */

/* 모든 요청의 입구. 설명은 api.h 참고. */
void api_dispatch(Request *req, Response *res)
{
    /* "http://" + 호스트 + ":포트" + 경로 + "?" + 쿼리 가 모두 들어갈 크기
     * sizeof(((Request *)0)->query) 는 "Request 구조체의 query 필드 크기" 를
     * 실제 객체 없이 구하는 C 관용구다 (sizeof 는 식을 계산하지 않으므로 안전). */
    char redirect[sizeof g_canonical_host + HTTP_PATH_MAX + sizeof(((Request *)0)->query) + 32];

    /* sku14.com -> www.sku14.com 으로 주소를 하나로 모은다.
     * GET 만 넘기는 이유: POST 를 301 로 넘기면 브라우저가 본문을 버리고 GET 으로 바꿔 보낼 수 있다. */
    if (strcmp(req->method, "GET") == 0 &&
        canonical_redirect_url(req, redirect, sizeof redirect)) {
        res_redirect(res, 301, redirect);
        return;
    }

    /* API 요청: 각 모듈 라우터에 차례로 물어본다 (먼저 처리한 곳에서 멈춤) */
    if (strncmp(req->path, "/api/", 5) == 0) {
        if (route_auth(req, res))  return;
        if (route_posts(req, res)) return;
        if (route_admin(req, res)) return;
        if (route_ai(req, res))    return;
        if (route_ml(req, res))    return;
        if (route_community(req, res)) return;
        res_error(res, 404, "no_route", "그런 API 가 없습니다.");
        return;
    }

    /* 정적 파일은 읽기(GET)만 허용 */
    if (strcmp(req->method, "GET") != 0) {
        res_error(res, 405, "method_not_allowed", "허용되지 않는 메서드입니다.");
        return;
    }
    serve_static(req, res);
}

/* ------------------------------------------------------- 관리자 계정 생성 */

/* server.exe --add-admin 학번 이름 비밀번호
 * 같은 학번이 이미 있으면(ON DUPLICATE KEY) 그 계정을 관리자로 바꾸고 비밀번호를 재설정한다.
 * 그래서 관리자 비밀번호를 잊었을 때도 이 명령으로 다시 정할 수 있다. */
int api_create_admin(const char *student_no, const char *name, const char *password)
{
    char salt[PW_SALT_HEX_LEN + 1];
    char hash[PW_HASH_HEX_LEN + 1];
    Buf sql;
    int ok;

    if (!valid_student_no(student_no)) {
        log_err("관리자 학번은 숫자 4~20자여야 합니다.");
        return 0;
    }
    if (strlen(password) < 8) {
        log_err("관리자 비밀번호는 8자 이상이어야 합니다.");
        return 0;
    }

    if (!random_hex(salt, PW_SALT_HEX_LEN)) {
        log_err("난수를 얻지 못해 관리자 계정을 만들지 못했습니다.");
        return 0;
    }
    pw_hash(salt, password, hash);

    /* 관리자는 학과가 없으므로 department_id = NULL, 표시 이름(nickname)은 실명 그대로 */
    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO users (student_no, name, nickname, department_id, role, pw_salt, pw_hash) "
            "VALUES (%Q, %Q, %Q, NULL, 'admin', %Q, %Q) "
            "ON DUPLICATE KEY UPDATE name = VALUES(name), role = 'admin', "
            "pw_salt = VALUES(pw_salt), pw_hash = VALUES(pw_hash)",
            student_no, name, name, salt, hash);
    ok = db_exec_buf(&sql);
    buf_free(&sql);

    if (ok)
        log_info("관리자 계정 준비 완료: %s (%s)", student_no, name);
    return ok;
}
