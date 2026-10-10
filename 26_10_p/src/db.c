/* MySQL 연결과 SQL 조립/실행
 *
 * 전역 연결 하나(g_conn)를 모든 스레드가 공유하고, 임계구역(g_lock)으로 한 번에
 * 한 스레드만 쓰게 한다. MySQL C API 의 MYSQL 핸들은 스레드 안전하지 않아서,
 * 두 스레드가 동시에 mysql_query 를 부르면 프로토콜이 꼬여 결과가 뒤섞인다.
 */
#include "db.h"

#include <windows.h>   /* CRITICAL_SECTION */
#include <stdio.h>
#include <stdlib.h>    /* atoll */
#include <string.h>

static MYSQL            *g_conn;        /* 공유 DB 연결 (db_init 성공 후 유효) */
static CRITICAL_SECTION  g_lock;        /* g_conn 을 보호하는 재진입 가능 잠금 */
static int               g_lock_ready;  /* g_lock 이 초기화되었는지 (초기화 전 lock 호출 방지) */
static char              g_err[512];    /* 마지막 SQL 오류 메시지 */
static unsigned int      g_errno;       /* 마지막 SQL 오류 번호 */

/* DB 에 접속한다. 성공 1, 실패 0. */
int db_init(const DbConfig *cfg)
{
    my_bool reconnect = 1;   /* MYSQL_OPT_RECONNECT 에 넘길 값 (1 = 켬) */

    /* 클라이언트 라이브러리 전역 초기화. 여러 스레드를 쓰기 전에 메인 스레드에서 한 번 해야 한다. */
    if (mysql_library_init(0, NULL, NULL)) {
        log_err("MySQL 라이브러리 초기화 실패");
        return 0;
    }

    InitializeCriticalSection(&g_lock);
    g_lock_ready = 1;

    g_conn = mysql_init(NULL);   /* 연결 핸들 할당 (아직 접속은 안 함) */
    if (!g_conn) {
        log_err("mysql_init 실패");
        return 0;
    }

    /* 끊겼을 때 자동 재연결, 통신은 utf8mb4 로 고정
     * - MySQL 은 일정 시간(wait_timeout) 쓰지 않은 연결을 끊는다. 서버를 오래 띄워 두면
     *   밤새 끊길 수 있으므로 자동 재연결을 켠다.
     * - utf8mb4 는 이모지(4바이트 UTF-8)까지 담을 수 있는 진짜 UTF-8 이다.
     *   (MySQL 의 옛 "utf8" 은 3바이트까지만 지원해 이모지가 깨진다.) */
    mysql_options(g_conn, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(g_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    /* 실제 접속. 마지막 두 인자 NULL, 0 은 유닉스 소켓 경로 없음, 추가 플래그 없음. */
    if (!mysql_real_connect(g_conn, cfg->host, cfg->user, cfg->pass,
                            cfg->name, cfg->port, NULL, 0)) {
        /* 비밀번호는 로그에 남기지 않는다. */
        log_err("DB 접속 실패 (%s@%s:%u/%s): %s",
                cfg->user, cfg->host, cfg->port, cfg->name, mysql_error(g_conn));
        mysql_close(g_conn);
        g_conn = NULL;
        return 0;
    }

    /* 접속 후에도 한 번 더 지정해 mysql_real_escape_string 이 같은 문자셋 규칙을 쓰게 한다.
     * (이스케이프 함수는 연결의 문자셋을 보고 다바이트 문자를 처리한다.) */
    mysql_set_character_set(g_conn, "utf8mb4");
    log_info("DB 접속 완료: %s@%s:%u/%s", cfg->user, cfg->host, cfg->port, cfg->name);
    return 1;
}

/* 접속을 닫고 잠금과 라이브러리를 정리한다. */
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

/* 새 스레드에서 MySQL 함수를 쓰기 전에 부른다. (http.c 의 요청 스레드 시작부) */
void db_thread_begin(void)
{
    mysql_thread_init();
}

/* 스레드가 끝나기 직전에 부른다. 스레드별 내부 메모리를 해제한다. */
void db_thread_end(void)
{
    mysql_thread_end();
}

/* 공유 연결 잠금. 같은 스레드가 여러 번 잠가도 된다(재진입). */
void db_lock(void)
{
    if (g_lock_ready)
        EnterCriticalSection(&g_lock);
}

/* 잠금 해제. lock 한 횟수만큼 불러야 완전히 풀린다. */
void db_unlock(void)
{
    if (g_lock_ready)
        LeaveCriticalSection(&g_lock);
}

/* s 를 SQL 문자열 리터럴('...')로 이스케이프해 b 에 붙인다. NULL 이면 NULL 키워드.
 * 예: It's  →  'It\'s'
 * 이것이 SQL 인젝션 방어의 핵심이다. 사용자가 ' OR 1=1 -- 같은 값을 넣어도
 * 따옴표가 이스케이프되어 문자열 데이터로만 취급된다. */
void db_quote(Buf *b, const char *s)
{
    size_t n;

    if (!s) {
        buf_puts(b, "NULL");
        return;
    }
    n = strlen(s);
    /* 최악의 경우 모든 글자가 이스케이프되어 2배가 된다 (+ 따옴표 2개 + NUL 1개 = 3) */
    if (!buf_reserve(b, n * 2 + 3))
        return;

    b->data[b->len++] = '\'';   /* 여는 따옴표 */
    db_lock();                  /* 이스케이프 함수도 연결 핸들을 쓰므로 잠근다 */
    /* 버퍼 끝에 바로 이스케이프된 결과를 쓰고, 쓴 길이만큼 len 을 늘린다 */
    b->len += mysql_real_escape_string(g_conn, b->data + b->len, s, (unsigned long)n);
    db_unlock();
    b->data[b->len++] = '\'';   /* 닫는 따옴표 */
    b->data[b->len] = '\0';     /* Buf 불변 조건 유지 */
}

/* printf 비슷한 SQL 조립 함수. 지원 지정자는 db.h 참고.
 * 형식 문자열을 한 글자씩 읽으며, '%' 를 만나면 다음 글자로 어떤 인자를 꺼낼지 정한다. */
void db_sqlf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    const char *p = fmt;   /* 형식 문자열을 읽는 위치 */

    va_start(ap, fmt);
    while (*p) {
        if (*p != '%') {          /* 일반 글자는 그대로 복사 */
            buf_putc(b, *p++);
            continue;
        }
        p++;                      /* '%' 다음 글자(지정자)로 이동 */
        switch (*p) {
        case 'Q': db_quote(b, va_arg(ap, const char *));            break;  /* 안전한 문자열 */
        case 's': buf_puts(b, va_arg(ap, const char *));            break;  /* 날 문자열 (주의!) */
        case 'd': buf_printf(b, "%d", va_arg(ap, int));             break;
        case 'u': buf_printf(b, "%u", va_arg(ap, unsigned int));    break;
        case 'L': buf_printf(b, "%lld", va_arg(ap, long long));     break;
        case '%': buf_putc(b, '%');                                 break;  /* "%%" → "%" (LIKE 패턴 등) */
        default:
            /* 알 수 없는 지정자는 그대로 남겨 실수를 눈에 띄게 한다.
             * (이 경우 va_arg 를 꺼내지 않으므로 뒤 인자 순서가 어긋날 수 있다 - 쓰지 말 것) */
            buf_putc(b, '%');
            buf_putc(b, *p);
            break;
        }
        if (*p)                   /* 형식 문자열이 '%' 로 끝난 경우 NUL 을 넘어가지 않게 */
            p++;
    }
    va_end(ap);
}

/* 결과셋이 없는 문장(INSERT/UPDATE/DELETE 등)을 실행한다. 성공 1. */
int db_exec(const char *sql)
{
    int rc;

    db_lock();
    rc = mysql_query(g_conn, sql);   /* 0 이면 성공 */
    if (rc) {
        /* 오류 정보는 잠금 안에서 복사해 둔다. 잠금을 풀면 다른 스레드의 질의로 덮어써진다. */
        g_errno = mysql_errno(g_conn);
        str_copy(g_err, sizeof g_err, mysql_error(g_conn));
        log_err("SQL 실패: %s", g_err);
        log_err("  문장: %.400s", sql);   /* 너무 긴 문장은 앞 400자만 로그에 */
    }
    db_unlock();
    return rc == 0;
}

/* 결과셋이 있는 문장(SELECT)을 실행하고 결과 전체를 메모리로 받아 돌려준다.
 * 실패하면 NULL. 결과셋은 호출자가 mysql_free_result 로 해제해야 한다. */
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
        /* mysql_store_result 는 모든 행을 클라이언트 메모리로 한 번에 가져온다.
         * 그래서 잠금을 풀고 난 뒤에도 결과를 안전하게 읽을 수 있다.
         * (mysql_use_result 는 행을 하나씩 서버에서 당겨 오므로 다 읽을 때까지 연결을 점유한다.) */
        res = mysql_store_result(g_conn);
        /* 결과가 NULL 인데 열 개수가 0 이 아니면 "결과가 있어야 했는데 못 읽은" 진짜 오류.
         * 열 개수가 0 이면 애초에 결과셋이 없는 문장(UPDATE 등)이었다는 뜻이다. */
        if (!res && mysql_field_count(g_conn)) {
            str_copy(g_err, sizeof g_err, mysql_error(g_conn));
            log_err("결과셋 읽기 실패: %s", g_err);
        }
    }
    db_unlock();
    return res;
}

/* Buf 에 조립된 SQL 실행. 메모리 부족으로 data 가 NULL 이면 실패로 처리한다. */
int db_exec_buf(Buf *b)
{
    return b->data ? db_exec(b->data) : 0;
}

MYSQL_RES *db_query_buf(Buf *b)
{
    return b->data ? db_query(b->data) : NULL;
}

/* 마지막 INSERT 의 AUTO_INCREMENT 값.
 * 주의: 다른 스레드가 사이에 INSERT 하면 그 값이 나온다. 정확히 필요하면
 * INSERT 와 이 호출을 db_lock/db_unlock 으로 함께 감싼다. */
unsigned long long db_last_id(void)
{
    unsigned long long id;

    db_lock();
    id = mysql_insert_id(g_conn);
    db_unlock();
    return id;
}

/* 마지막 변경 문장이 영향을 준 행 수 (위와 같은 주의사항) */
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

/* 첫 행 첫 열 값 하나를 정수로 읽는다. (COUNT, MAX, 존재 여부 확인 등)
 * 결과가 없거나 NULL 이면 def 를 돌려준다. */
long long db_scalar(const char *sql, long long def)
{
    MYSQL_RES *res;
    MYSQL_ROW row;     /* 행 = 문자열 포인터 배열 (row[0] 이 첫 열) */
    long long v = def;

    res = db_query(sql);
    if (!res)
        return def;
    row = mysql_fetch_row(res);
    if (row && row[0])           /* SQL NULL 값은 row[0] == NULL 로 온다 */
        v = atoll(row[0]);       /* MySQL C API 는 모든 값을 문자열로 준다 */
    mysql_free_result(res);
    return v;
}

/* 트랜잭션 시작. 성공하면 잠금을 쥔 채로 돌아간다. */
int db_begin(void)
{
    db_lock();                     /* commit/rollback 까지 연결을 독점한다 */
    if (db_exec("START TRANSACTION"))
        return 1;
    db_unlock();                   /* 시작에 실패했으면 잠금을 바로 돌려준다 */
    return 0;
}

/* 트랜잭션 확정 + db_begin 에서 잡은 잠금 해제 */
int db_commit(void)
{
    int ok = db_exec("COMMIT");
    db_unlock();
    return ok;
}

/* 트랜잭션 취소 + db_begin 에서 잡은 잠금 해제 */
void db_rollback(void)
{
    db_exec("ROLLBACK");
    db_unlock();
}
