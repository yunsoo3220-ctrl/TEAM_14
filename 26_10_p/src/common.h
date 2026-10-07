/* 공통 헤더 - 모든 모듈이 쓰는 타입과 문자열 버퍼 */
#ifndef SKU_COMMON_H
#define SKU_COMMON_H

#include <stddef.h>
#include <stdarg.h>

/* 가변 길이 문자열 버퍼. 응답 본문과 SQL 문을 조립할 때 쓴다. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_reset(Buf *b);
int   buf_reserve(Buf *b, size_t extra);
void  buf_add(Buf *b, const char *s, size_t n);
void  buf_puts(Buf *b, const char *s);
void  buf_putc(Buf *b, char c);
void  buf_printf(Buf *b, const char *fmt, ...);

/* 문자열 도우미 */
char *str_dup(const char *s);
void  str_copy(char *dst, size_t dstsz, const char *src);
int   str_ieq(const char *a, const char *b);        /* 대소문자 무시 비교 */
void  str_trim(char *s);                            /* 양끝 공백 제거 (제자리) */

/* 16진 난수 토큰 생성. out 은 n+1 바이트 이상이어야 한다.
 * OS 난수원을 쓰며, 실패하면 0 을 돌려준다 (반드시 확인할 것). */
int   random_hex(char *out, size_t n);

/* 로그 */
void  log_info(const char *fmt, ...);
void  log_warn(const char *fmt, ...);
void  log_err(const char *fmt, ...);

#endif
