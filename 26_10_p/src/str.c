/* Buf: 응답/SQL 조립용 가변 문자열 버퍼와 잡다한 문자열 도우미
 *
 * 이 파일의 함수들은 서버 전체에서 가장 많이 호출된다. 그래서 다음 원칙을 지킨다.
 *   - NULL 이나 길이 0 같은 경계 입력에도 죽지 않는다.
 *   - 버퍼 넘침(buffer overflow)이 생기지 않도록 항상 크기를 확인하고 쓴다.
 *   - 메모리가 부족하면 로그만 남기고 조용히 실패한다(서버 전체를 멈추지 않음).
 */
#include "common.h"

/* rand_s() 를 쓰기 위한 선언. stdlib.h 보다 먼저 와야 한다.
 * (MSVC/MinGW 의 stdlib.h 는 이 매크로가 정의되어 있을 때만 rand_s 를 선언한다.) */
#define _CRT_RAND_S

#include <stdio.h>    /* vsnprintf, fprintf */
#include <stdlib.h>   /* malloc, realloc, free, rand_s */
#include <string.h>   /* memcpy, memmove, strlen */
#include <ctype.h>    /* tolower, isspace */
#include <time.h>     /* time, localtime_s, strftime - 로그 시각 */

/* 버퍼를 빈 상태로 만든다. 메모리를 할당하지 않으므로 실패할 일이 없다. */
void buf_init(Buf *b)
{
    b->data = NULL;   /* 아직 할당된 메모리 없음 */
    b->len = 0;       /* 저장된 글자 수 0 */
    b->cap = 0;       /* 용량 0 */
}

/* 버퍼 메모리를 반납한다. free(NULL) 은 아무 일도 하지 않으므로 빈 버퍼에 불러도 안전하다. */
void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);      /* 해제한 포인터를 다시 쓰지 않도록 NULL 로 되돌린다 (dangling 방지) */
}

/* 내용만 비운다. 할당된 메모리(cap)는 그대로 두어 다음 추가 때 realloc 을 줄인다. */
void buf_reset(Buf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';   /* 빈 문자열로 보이도록 첫 글자를 NUL 로 */
}

/* 앞으로 extra 바이트를 더 쓸 수 있도록 용량을 확보한다.
 *
 * 용량은 2배씩 늘린다(기하급수적 증가). 한 글자씩 1000번 추가해도 realloc 은
 * 몇 번(256→512→1024...)만 일어나므로, 전체 비용이 평균적으로 O(1) 이 된다.
 * 반환: 1 = 성공(이제 extra 바이트 + NUL 을 쓸 공간이 있음), 0 = 메모리 부족 */
int buf_reserve(Buf *b, size_t extra)
{
    size_t want = b->len + extra + 1;   /* 필요한 총 바이트 (+1 은 끝의 '\0' 자리) */
    size_t cap;
    char *p;

    if (want <= b->cap)                 /* 이미 충분하면 아무것도 하지 않는다 */
        return 1;

    cap = b->cap ? b->cap : 256;        /* 처음 할당이면 256 바이트부터 시작 */
    while (cap < want)                  /* 필요한 크기를 넘을 때까지 2배씩 키운다 */
        cap *= 2;

    /* realloc 은 기존 내용을 보존한 채 크기를 바꾼다. 실패하면 NULL 을 돌려주지만
     * 원래 메모리는 그대로 살아 있으므로, b->data 에 바로 대입하지 않고 p 로 받는다.
     * (b->data = realloc(...) 으로 쓰면 실패 시 원래 포인터를 잃어 메모리가 샌다.) */
    p = (char *)realloc(b->data, cap);
    if (!p) {
        log_err("메모리 부족: %zu 바이트 요청", cap);
        return 0;
    }
    b->data = p;
    b->cap = cap;
    return 1;
}

/* s 의 n 바이트를 버퍼 끝에 붙이고 NUL 종료를 유지한다. */
void buf_add(Buf *b, const char *s, size_t n)
{
    if (!n || !buf_reserve(b, n))       /* 붙일 게 없거나 공간 확보 실패면 그냥 돌아간다 */
        return;
    memcpy(b->data + b->len, s, n);     /* 현재 끝 위치부터 복사 */
    b->len += n;
    b->data[b->len] = '\0';             /* 불변 조건: data[len] == '\0' */
}

/* C 문자열 전체를 붙인다. NULL 은 무시한다(호출하는 쪽에서 매번 검사하지 않아도 되게). */
void buf_puts(Buf *b, const char *s)
{
    if (s)
        buf_add(b, s, strlen(s));
}

/* 문자 하나를 붙인다. 지역 변수 c 의 주소를 1바이트짜리 배열처럼 넘긴다. */
void buf_putc(Buf *b, char c)
{
    buf_add(b, &c, 1);
}

/* printf 형식으로 서식화해 붙인다. */
void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    /* 필요한 길이를 먼저 재고 한 번에 쓴다.
     * vsnprintf(NULL, 0, ...) 은 실제로 쓰지 않고 "필요한 글자 수" 만 돌려준다.
     * va_list 는 한 번 쓰면 소모되므로, 두 번째 호출 전에 va_start 를 다시 해야 한다. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0 || !buf_reserve(b, (size_t)n))   /* 빈 결과/형식 오류/메모리 부족 */
        return;

    va_start(ap, fmt);
    /* 남은 공간(cap - len)은 n+1 이상이 보장되므로 잘리지 않고 NUL 까지 써진다. */
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* 문자열을 힙에 복제한다. (POSIX strdup 과 같은 기능을 이식성 있게 직접 구현) */
char *str_dup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;          /* +1: 끝의 '\0' 까지 복사 */
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;                   /* 할당 실패 시 NULL - 호출자가 확인 */
}

/* 고정 크기 배열에 안전하게 복사한다. 넘치면 잘리지만 항상 NUL 로 끝난다. */
void str_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (!dstsz)                 /* 크기 0 인 대상에는 NUL 조차 쓸 수 없다 */
        return;
    if (!src) {                 /* NULL 원본은 빈 문자열로 취급 */
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstsz)             /* 들어갈 수 있는 최대 길이는 dstsz - 1 */
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    /* 참고: UTF-8 다바이트 문자 중간에서 잘릴 수 있다. 이 함수는 주로 길이가
     * 보장된 값(학번, 토큰 등)이나 로그용 메시지 복사에 쓰므로 그대로 둔다. */
}

/* ASCII 대소문자를 무시하고 두 문자열이 같은지 비교한다. 같으면 1.
 * HTTP 헤더 이름("Content-Type" vs "content-type")처럼 대소문자 구분이 없는 값에 쓴다. */
int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        /* tolower 에 음수(한글 바이트 등 0x80 이상의 char)를 넘기면 미정의 동작이므로
         * unsigned char 로 바꿔서 넘긴다. */
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;            /* 둘 다 동시에 끝났을 때만 같다 (길이도 같아야 함) */
}

/* 문자열 앞뒤의 공백(스페이스, 탭, 줄바꿈 등)을 제자리에서 지운다. */
void str_trim(char *s)
{
    char *p = s;
    size_t n;

    /* 1) 앞쪽 공백 건너뛰기 */
    while (*p && isspace((unsigned char)*p))
        p++;
    /* 앞에 공백이 있었다면 나머지를 맨 앞으로 당긴다.
     * 원본과 대상이 겹치므로 memcpy 가 아니라 memmove 를 써야 한다. */
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    /* 2) 뒤쪽 공백을 NUL 로 덮어 잘라낸다 */
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
    static const char hex[] = "0123456789abcdef";   /* 4비트 값 → 16진 문자 변환표 */
    size_t i = 0;                                   /* 지금까지 채운 글자 수 */

    while (i < n) {
        unsigned int v;     /* 32비트 암호학적 난수 */
        int k;

        if (rand_s(&v) != 0) {          /* 0 이 아니면 실패 (errno 값) */
            log_err("난수 생성 실패 (rand_s)");
            out[0] = '\0';              /* 반쯤 채운 값이 실수로 쓰이지 않도록 비운다 */
            return 0;
        }
        /* 32비트에서 16진 숫자 8개를 뽑아 쓴다.
         * 아래 4비트(v & 0xF)를 한 글자로 바꾸고, v 를 4비트 오른쪽으로 밀어 다음 4비트로. */
        for (k = 0; k < 8 && i < n; k++, i++) {
            out[i] = hex[v & 0xF];
            v >>= 4;
        }
    }
    out[n] = '\0';
    return 1;
}

/* 로그 공통 구현. "[2026-10-09 14:03:12] INFO 메시지" 형식으로 stderr 에 쓴다.
 * stderr 를 쓰는 이유: 버퍼링되지 않아 서버가 비정상 종료해도 마지막 로그가 남고,
 * start-server.ps1 이 stderr 를 logs/server.err.log 로 따로 모아 준다. */
static void vlog(const char *level, const char *fmt, va_list ap)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;

    /* localtime 은 내부 정적 버퍼를 써서 스레드에 안전하지 않다.
     * localtime_s 는 결과를 호출자가 준 tmv 에 써 주므로 여러 스레드가 동시에 써도 안전하다. */
    localtime_s(&tmv, &now);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "[%s] %-4s ", ts, level);   /* %-4s: 등급을 4칸 왼쪽 정렬해 줄을 맞춘다 */
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);                              /* 즉시 파일/콘솔로 내보낸다 */
}

/* 아래 세 함수는 등급 문자열만 다르고 나머지는 vlog 에 맡긴다.
 * 가변 인자(...)를 다른 함수로 넘기려면 va_list 로 바꿔서 넘겨야 한다. */
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
