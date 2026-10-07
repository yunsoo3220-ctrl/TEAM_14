/* Buf: 응답/SQL 조립용 가변 문자열 버퍼와 잡다한 문자열 도우미 */
#include "common.h"

/* rand_s() 를 쓰기 위한 선언. stdlib.h 보다 먼저 와야 한다. */
#define _CRT_RAND_S

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}

void buf_reset(Buf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

int buf_reserve(Buf *b, size_t extra)
{
    size_t want = b->len + extra + 1;
    size_t cap;
    char *p;

    if (want <= b->cap)
        return 1;

    cap = b->cap ? b->cap : 256;
    while (cap < want)
        cap *= 2;

    p = (char *)realloc(b->data, cap);
    if (!p) {
        log_err("메모리 부족: %zu 바이트 요청", cap);
        return 0;
    }
    b->data = p;
    b->cap = cap;
    return 1;
}

void buf_add(Buf *b, const char *s, size_t n)
{
    if (!n || !buf_reserve(b, n))
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s)
{
    if (s)
        buf_add(b, s, strlen(s));
}

void buf_putc(Buf *b, char c)
{
    buf_add(b, &c, 1);
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    /* 필요한 길이를 먼저 재고 한 번에 쓴다. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0 || !buf_reserve(b, (size_t)n))
        return;

    va_start(ap, fmt);
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *str_dup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

void str_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (!dstsz)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

void str_trim(char *s)
{
    char *p = s;
    size_t n;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* 세션 토큰과 비밀번호 솔트를 만든다.
 *
 * rand()/srand() 는 쓰지 않는다. Windows UCRT 에서 그 상태는 스레드마다 따로라서,
 * 요청마다 스레드를 만드는 이 서버에서는 모든 스레드가 같은 난수열을 내놓는다.
 * (실제로 서로 다른 사용자가 같은 솔트를 받고 세션 토큰이 충돌했다.)
 * rand_s() 는 OS 난수원(RtlGenRandom)을 쓰고 스레드 상태에 의존하지 않는다.
 *
 * 난수를 얻지 못하면 0 을 돌려준다. 호출자는 반드시 확인해야 하며,
 * 예측 가능한 값으로 대신 채우지 않는다. */
int random_hex(char *out, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i = 0;

    while (i < n) {
        unsigned int v;
        int k;

        if (rand_s(&v) != 0) {
            log_err("난수 생성 실패 (rand_s)");
            out[0] = '\0';
            return 0;
        }
        /* 32비트에서 16진 숫자 8개를 뽑아 쓴다. */
        for (k = 0; k < 8 && i < n; k++, i++) {
            out[i] = hex[v & 0xF];
            v >>= 4;
        }
    }
    out[n] = '\0';
    return 1;
}

static void vlog(const char *level, const char *fmt, va_list ap)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;

    localtime_s(&tmv, &now);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "[%s] %-4s ", ts, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("WARN", fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("ERR", fmt, ap);
    va_end(ap);
}
