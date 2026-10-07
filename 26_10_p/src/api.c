/* 디스패처, 정적 파일 제공, 인증(가입/로그인/세션), 학과 목록 */
#include "api.h"
#include "db.h"
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

static void handle_register(Request *req, Response *res)
{
    Json *in = body_object(req, res);
    const char *student_no, *name, *nickname, *password;
    unsigned dept_id;
    char salt[PW_SALT_HEX_LEN + 1];
    char hash[PW_HASH_HEX_LEN + 1];
    Buf sql;
    long long dept_exists;

    if (!in)
        return;

    student_no = json_str(in, "student_no", "");
    name       = json_str(in, "name", "");
    nickname   = json_str(in, "nickname", "");
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
    if (strlen(nickname) < 2 || strlen(nickname) > 40) {
        res_error(res, 400, "bad_nickname", "닉네임은 2자 이상 입력하세요.");
        goto out;
    }
    if (strlen(password) < 8) {
        res_error(res, 400, "weak_password", "비밀번호는 8자 이상이어야 합니다.");
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

    if (!random_hex(salt, PW_SALT_HEX_LEN)) {
        res_error(res, 500, "no_entropy", "보안 처리에 실패했습니다. 잠시 후 다시 시도하세요.");
        goto out;
    }
    pw_hash(salt, password, hash);

    buf_init(&sql);
    db_sqlf(&sql,
            "INSERT INTO users (student_no, name, nickname, department_id, role, pw_salt, pw_hash) "
            "VALUES (%Q, %Q, %Q, %u, 'student', %Q, %Q)",
            student_no, name, nickname, dept_id, salt, hash);

    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        if (db_errno() == DB_ERR_DUP_ENTRY)
            res_error(res, 409, "duplicate", "이미 등록된 학번이거나 닉네임입니다.");
        else
            /* 그 밖의 오류는 뭉뚱그리지 않는다. 자세한 내용은 서버 로그에 남는다. */
            res_error(res, 500, "db_error", "가입 처리 중 오류가 났습니다. 서버 로그를 확인하세요.");
        goto out;
    }
    buf_free(&sql);

    res_json(res, 201, "{\"ok\":true}");

out:
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
    if (!auth_current(req, &u)) {
        buf_puts(&b, "{\"user\":null}");
    } else {
        buf_puts(&b, "{\"user\":");
        write_user(&b, &u);
        buf_putc(&b, '}');
    }
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
