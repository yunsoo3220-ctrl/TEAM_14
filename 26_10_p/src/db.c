/* MySQL 연결과 SQL 조립/실행 */
#include "db.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MYSQL            *g_conn;
static CRITICAL_SECTION  g_lock;
static int               g_lock_ready;
static char              g_err[512];
static unsigned int      g_errno;

int db_init(const DbConfig *cfg)
{
    my_bool reconnect = 1;

    if (mysql_library_init(0, NULL, NULL)) {
        log_err("MySQL 라이브러리 초기화 실패");
        return 0;
    }

    InitializeCriticalSection(&g_lock);
    g_lock_ready = 1;

    g_conn = mysql_init(NULL);
    if (!g_conn) {
        log_err("mysql_init 실패");
        return 0;
    }

    /* 끊겼을 때 자동 재연결, 통신은 utf8mb4 로 고정 */
    mysql_options(g_conn, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(g_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(g_conn, cfg->host, cfg->user, cfg->pass,
                            cfg->name, cfg->port, NULL, 0)) {
        log_err("DB 접속 실패 (%s@%s:%u/%s): %s",
                cfg->user, cfg->host, cfg->port, cfg->name, mysql_error(g_conn));
        mysql_close(g_conn);
        g_conn = NULL;
        return 0;
    }

    mysql_set_character_set(g_conn, "utf8mb4");
    log_info("DB 접속 완료: %s@%s:%u/%s", cfg->user, cfg->host, cfg->port, cfg->name);
    return 1;
}

void db_close(void)
{
    if (g_conn) {
        mysql_close(g_conn);
        g_conn = NULL;
    }
    if (g_lock_ready) {
        DeleteCriticalSection(&g_lock);
        g_lock_ready = 0;
    }
    mysql_library_end();
}

void db_thread_begin(void)
{
    mysql_thread_init();
}

void db_thread_end(void)
{
    mysql_thread_end();
}

void db_lock(void)
{
    if (g_lock_ready)
        EnterCriticalSection(&g_lock);
}

void db_unlock(void)
{
    if (g_lock_ready)
        LeaveCriticalSection(&g_lock);
}

void db_quote(Buf *b, const char *s)
{
    size_t n;

    if (!s) {
        buf_puts(b, "NULL");
        return;
    }
    n = strlen(s);
    if (!buf_reserve(b, n * 2 + 3))
        return;

    b->data[b->len++] = '\'';
    db_lock();
    b->len += mysql_real_escape_string(g_conn, b->data + b->len, s, (unsigned long)n);
    db_unlock();
    b->data[b->len++] = '\'';
    b->data[b->len] = '\0';
}

void db_sqlf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    const char *p = fmt;

    va_start(ap, fmt);
    while (*p) {
        if (*p != '%') {
            buf_putc(b, *p++);
            continue;
        }
        p++;
        switch (*p) {
        case 'Q': db_quote(b, va_arg(ap, const char *));            break;
        case 's': buf_puts(b, va_arg(ap, const char *));            break;
        case 'd': buf_printf(b, "%d", va_arg(ap, int));             break;
        case 'u': buf_printf(b, "%u", va_arg(ap, unsigned int));    break;
        case 'L': buf_printf(b, "%lld", va_arg(ap, long long));     break;
        case '%': buf_putc(b, '%');                                 break;
        default:
            /* 알 수 없는 지정자는 그대로 남겨 실수를 눈에 띄게 한다. */
            buf_putc(b, '%');
            buf_putc(b, *p);
            break;
        }
        if (*p)
            p++;
    }
    va_end(ap);
}

int db_exec(const char *sql)
{
    int rc;

    db_lock();
    rc = mysql_query(g_conn, sql);
    if (rc) {
        g_errno = mysql_errno(g_conn);
        str_copy(g_err, sizeof g_err, mysql_error(g_conn));
        log_err("SQL 실패: %s", g_err);
        log_err("  문장: %.400s", sql);
    }
    db_unlock();
    return rc == 0;
}

MYSQL_RES *db_query(const char *sql)
{
    MYSQL_RES *res = NULL;

    db_lock();
    if (mysql_query(g_conn, sql)) {
        g_errno = mysql_errno(g_conn);
        str_copy(g_err, sizeof g_err, mysql_error(g_conn));
        log_err("SQL 실패: %s", g_err);
        log_err("  문장: %.400s", sql);
    } else {
        res = mysql_store_result(g_conn);
        if (!res && mysql_field_count(g_conn)) {
            str_copy(g_err, sizeof g_err, mysql_error(g_conn));
            log_err("결과셋 읽기 실패: %s", g_err);
        }
    }
    db_unlock();
    return res;
}

int db_exec_buf(Buf *b)
{
    return b->data ? db_exec(b->data) : 0;
}

MYSQL_RES *db_query_buf(Buf *b)
{
    return b->data ? db_query(b->data) : NULL;
}

unsigned long long db_last_id(void)
{
    unsigned long long id;

    db_lock();
    id = mysql_insert_id(g_conn);
    db_unlock();
    return id;
}

unsigned long long db_affected(void)
{
    unsigned long long n;

    db_lock();
    n = mysql_affected_rows(g_conn);
    db_unlock();
    return n;
}

const char *db_error(void)
{
    return g_err;
}

unsigned int db_errno(void)
{
    return g_errno;
}

long long db_scalar(const char *sql, long long def)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    long long v = def;

    res = db_query(sql);
    if (!res)
        return def;
    row = mysql_fetch_row(res);
    if (row && row[0])
        v = atoll(row[0]);
    mysql_free_result(res);
    return v;
}

int db_begin(void)
{
    db_lock();                     /* commit/rollback 까지 연결을 독점한다 */
    if (db_exec("START TRANSACTION"))
        return 1;
    db_unlock();
    return 0;
}

int db_commit(void)
{
    int ok = db_exec("COMMIT");
    db_unlock();
    return ok;
}

void db_rollback(void)
{
    db_exec("ROLLBACK");
    db_unlock();
}
