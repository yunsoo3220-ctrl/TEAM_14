/* 디스패처, 정적 파일 제공, 인증(가입/로그인/세션), 학과 목록 */
#include "api.h"
#include "db.h"
#include "ai.h"
#include "mailer.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char           g_webroot[260] = "www";
static char           g_canonical_host[128] = "www.sku14.com";
static unsigned short g_canonical_port = 80;

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
 * 그 외(127.0.0.1, localhost, 이미 www 인 경우)는 건드리지 않고 0 을 돌려준다. */
static int canonical_redirect_url(const Request *req, char *out, size_t outsz)
{
    char host[128];
    const char *bare;

    if (!g_canonical_host[0] || strncmp(g_canonical_host, "www.", 4) != 0)
        return 0;

    bare = g_canonical_host + 4;              /* "sku14.com" */
    host_name_only(req->host, host, sizeof host);
    if (!str_ieq(host, bare))
        return 0;

    if (g_canonical_port == 80)
        snprintf(out, outsz, "http://%s%s", g_canonical_host, req->path);
    else
        snprintf(out, outsz, "http://%s:%u%s", g_canonical_host,
                 g_canonical_port, req->path);

    if (req->query[0]) {
        size_t n = strlen(out);
        snprintf(out + n, outsz - n, "?%s", req->query);
    }
    return 1;
}

/* ------------------------------------------------------------------ 인증 */

int auth_current(const Request *req, CurrentUser *u)
{
    char token[SESSION_TOKEN_LEN + 1];
    Buf sql;
    MYSQL_RES *res;
    MYSQL_ROW row;

    memset(u, 0, sizeof *u);
    if (!req_cookie(req, SESSION_COOKIE, token, sizeof token) || !token[0])
        return 0;

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

int auth_require(const Request *req, Response *res, CurrentUser *u)
{
    if (auth_current(req, u))
        return 1;
    res_error(res, 401, "login_required", "로그인이 필요합니다.");
    return 0;
}

int auth_require_admin(const Request *req, Response *res, CurrentUser *u)
{
    if (!auth_require(req, res, u))
        return 0;
    if (u->is_admin)
        return 1;
    res_error(res, 403, "admin_only", "관리자만 할 수 있습니다.");
    return 0;
}

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
        else if (v->type == JS_STR)
            id = strtol(v->str, NULL, 10);

        if (id > 0)
            out[n++] = (unsigned)id;
    }
    return n;
}

/* 사용자 정보를 JSON 객체로 쓴다. */
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

/* UTF-8 글자 하나의 바이트 수 */
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
    const char *starts[32];
    int lens[32];
    int n = 0;
    const char *p = name;
    int i;

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

    buf_add(out, starts[0], (size_t)lens[0]);
    for (i = 1; i < n - 1; i++)
        buf_putc(out, '*');

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

/* 표시 이름을 만든다. 성공 시 1. */
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
 * 코드는 5번 틀리면 다시 받아야 한다. 소금 친 해시로만 저장하고, 가입에 성공하면 지운다. */

#define SCHOOL_DOMAIN    "@skuniv.ac.kr"
#define CODE_TTL_MIN     10
#define VERIFIED_TTL_MIN 30
#define CODE_RESEND_SEC  60
#define CODE_MAX_TRIES   5

/* 학교 이메일만 받는다. 소문자로 바꿔 out 에 담는다. */
static int school_email(const char *in, char *out, size_t outsz)
{
    size_t n = strlen(in), dn = strlen(SCHOOL_DOMAIN), i;

    if (n <= dn || n >= outsz || n > 120)
        return 0;
    for (i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[n] = '\0';
    if (strcmp(out + n - dn, SCHOOL_DOMAIN) != 0)
        return 0;
    for (i = 0; i < n - dn; i++) {          /* 아이디 부분: 영문 소문자·숫자·. _ - */
        char c = out[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return out[0] != '.' && out[n - dn - 1] != '.';
}

static void handle_send_code(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    char email[128], code[8], hex[9], salt[PW_SALT_HEX_LEN + 1], hash[PW_HASH_HEX_LEN + 1], err[200];
    Buf sql, text;
    long long wait;

    if (!in)
        return;
    if (!school_email(json_str(in, "email", ""), email, sizeof email)) {
        json_free(in);
        res_error(res, 400, "bad_email", "학교 이메일(@skuniv.ac.kr)만 쓸 수 있습니다.");
        return;
    }
    json_free(in);
    if (!mail_enabled() && !mail_dev_mode()) {
        log_err("메일 서버(SKU_SMTP_URL)가 설정되지 않아 인증 메일을 보낼 수 없습니다. smtp.env 를 확인하세요.");
        res_error(res, 503, "mail_not_configured",
                  "메일 서버가 설정되지 않아 지금은 인증 메일을 보낼 수 없습니다. 관리자에게 문의하세요.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM users WHERE email = %Q", email);
    if (db_scalar(sql.data, 0) > 0) {
        buf_free(&sql);
        res_error(res, 409, "duplicate_email", "이미 가입한 이메일입니다.");
        return;
    }
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT %d - TIMESTAMPDIFF(SECOND, sent_at, NOW()) FROM email_verifications "
                  "WHERE email = %Q", CODE_RESEND_SEC, email);
    wait = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (wait > 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "%lld초 뒤에 다시 보낼 수 있습니다.", wait);
        res_error(res, 429, "too_soon", msg);
        return;
    }

    /* 6자리 코드 (OS 난수원) */
    if (!random_hex(hex, 8) || !random_hex(salt, PW_SALT_HEX_LEN)) {
        res_error(res, 500, "no_entropy", "보안 처리에 실패했습니다. 잠시 후 다시 시도하세요.");
        return;
    }
    snprintf(code, sizeof code, "%06lu", strtoul(hex, NULL, 16) % 1000000ul);
    pw_hash(salt, code, hash);

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
        ;
    if (i != 6 || code[i]) {
        res_error(res, 400, "bad_code_format", "메일로 받은 6자리 인증 코드를 입력하세요.");
        return 0;
    }

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
    pw_hash(salt, code, got);
    if (strcmp(got, want) != 0) {
        char msg[96];
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

static void handle_register(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    const char *student_no, *name, *phone, *password;
    char email[128];
    unsigned dept_id;
    char salt[PW_SALT_HEX_LEN + 1];
    char hash[PW_HASH_HEX_LEN + 1];
    Buf sql, display;
    long long dept_exists;
    unsigned tag_ids[16];
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
        unsigned uid = (unsigned)db_last_id();

        buf_init(&sql);
        db_sqlf(&sql, "DELETE FROM email_verifications WHERE email = %Q", email);   /* 코드는 한 번만 */
        db_exec_buf(&sql);
        buf_free(&sql);
        if (!profile_save(uid, tag_ids, ntags, json_str(in, "bio", "")))
            log_warn("가입한 회원 %s 의 관심 키워드를 저장하지 못했습니다.", student_no);
    }

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

    if (!uid || !pw_verify(salt, hash, password)) {
        res_error(res, 401, "bad_credentials", "학번 또는 비밀번호가 맞지 않습니다.");
        goto out;
    }

    /* 만료된 세션을 치우고 새 세션을 만든다. */
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

    res_set_cookie(res, SESSION_COOKIE, token, SESSION_HOURS * 3600L);

    /* 응답에 사용자 정보를 함께 담는다. */
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

static void handle_departments(Response *res)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    Buf b;
    char last_college[64] = "";
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
            buf_putc(&b, ',');
        }
        buf_putc(&b, '{');
        json_kv_int(&b, "id", atoll(row[2]));
        buf_putc(&b, ',');
        json_kv_str(&b, "name", row[3]);
        buf_putc(&b, '}');
    }
    if (!first_college)
        buf_puts(&b, "]}");
    buf_puts(&b, "]}");

    mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

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

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (!dot)
        return "application/octet-stream";
    if (str_ieq(dot, ".html")) return "text/html; charset=utf-8";
    if (str_ieq(dot, ".css"))  return "text/css; charset=utf-8";
    if (str_ieq(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (str_ieq(dot, ".json")) return "application/json; charset=utf-8";
    if (str_ieq(dot, ".svg"))  return "image/svg+xml";
    if (str_ieq(dot, ".png"))  return "image/png";
    if (str_ieq(dot, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

/* 경로 탈출(.. 또는 절대경로)을 막는다. */
static int path_is_safe(const char *p)
{
    if (strstr(p, ".."))
        return 0;
    if (strchr(p, ':') || strchr(p, '\\'))
        return 0;
    return 1;
}

static void serve_static(Request *req, Response *res)
{
    char full[sizeof g_webroot + HTTP_PATH_MAX + 2];
    const char *rel = req->path;
    FILE *f;
    long size;

    if (strcmp(rel, "/") == 0)
        rel = "/index.html";

    if (!path_is_safe(rel)) {
        res_error(res, 400, "bad_path", "잘못된 경로입니다.");
        return;
    }

    snprintf(full, sizeof full, "%s%s", g_webroot, rel);

    f = fopen(full, "rb");
    if (!f) {
        res_error(res, 404, "not_found", "페이지를 찾을 수 없습니다.");
        return;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        res_error(res, 500, "too_large", "파일이 너무 큽니다.");
        return;
    }

    buf_reset(&res->body);
    if (buf_reserve(&res->body, (size_t)size)) {
        size_t got = fread(res->body.data, 1, (size_t)size, f);
        res->body.len = got;
        res->body.data[got] = '\0';
    }
    fclose(f);

    res->status = 200;
    str_copy(res->content_type, sizeof res->content_type, mime_for(full));
    res_header(res, "Cache-Control", "no-cache");
}

/* ------------------------------------------------------------ 디스패처 */

void api_dispatch(Request *req, Response *res)
{
    /* "http://" + 호스트 + ":포트" + 경로 + "?" + 쿼리 가 모두 들어갈 크기 */
    char redirect[sizeof g_canonical_host + HTTP_PATH_MAX + sizeof(((Request *)0)->query) + 32];

    /* sku14.com -> www.sku14.com 으로 주소를 하나로 모은다. */
    if (strcmp(req->method, "GET") == 0 &&
        canonical_redirect_url(req, redirect, sizeof redirect)) {
        res_redirect(res, 301, redirect);
        return;
    }

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

    if (strcmp(req->method, "GET") != 0) {
        res_error(res, 405, "method_not_allowed", "허용되지 않는 메서드입니다.");
        return;
    }
    serve_static(req, res);
}

/* ------------------------------------------------------- 관리자 계정 생성 */

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
