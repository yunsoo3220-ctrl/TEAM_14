/* 공통 헤더 - 모든 모듈이 쓰는 타입과 문자열 버퍼
 *
 * 이 헤더는 서버의 거의 모든 .c 파일이 (직접 또는 http.h/db.h/json.h 를 통해)
 * 포함한다. 여기에는 다음 세 가지가 들어 있다.
 *   1) Buf  : 크기가 자동으로 늘어나는 문자열 버퍼 (구현은 str.c)
 *   2) str_*: C 문자열을 안전하게 다루기 위한 작은 도우미 함수들
 *   3) log_*: 콘솔/로그 파일에 시각과 함께 메시지를 남기는 함수들
 *
 * C 표준 라이브러리에는 "길이가 늘어나는 문자열" 이 없기 때문에, HTTP 응답 본문,
 * SQL 문, JSON 문자열처럼 길이를 미리 알 수 없는 텍스트는 모두 Buf 로 조립한다.
 */
#ifndef SKU_COMMON_H   /* 인클루드 가드: 같은 헤더가 두 번 포함되어도 */
#define SKU_COMMON_H   /* 선언이 중복되지 않도록 한 번만 처리하게 한다. */

#include <stddef.h>    /* size_t 정의 */
#include <stdarg.h>    /* va_list - buf_printf/log_* 같은 가변 인자 함수용 */

/* 가변 길이 문자열 버퍼. 응답 본문과 SQL 문을 조립할 때 쓴다.
 *
 * 불변 조건(invariant):
 *   - data 가 NULL 이 아니면 data[len] 은 항상 '\0' 이다.
 *     (그래서 data 를 그대로 C 문자열로 printf 등에 넘길 수 있다.)
 *   - len < cap (NUL 종료 문자 자리 1 바이트를 늘 남겨 둔다)
 *   - 메모리 확보에 실패해도 프로그램을 죽이지 않고, 이후 추가 요청을 조용히
 *     무시한다. (서버가 메모리 부족 하나로 통째로 멈추는 일을 막기 위함)
 *
 * 사용 순서: buf_init → buf_puts/buf_printf ... → (data 사용) → buf_free
 */
typedef struct {
    char  *data;   /* 힙에 할당된 실제 문자 배열 (처음에는 NULL 일 수 있음) */
    size_t len;    /* 현재 저장된 문자 수 (끝의 '\0' 제외) */
    size_t cap;    /* data 에 할당된 전체 바이트 수 (용량) */
} Buf;

/* 버퍼를 빈 상태(data=NULL, len=0, cap=0)로 초기화한다. 메모리는 아직 잡지 않는다. */
void  buf_init(Buf *b);
/* 버퍼가 가진 힙 메모리를 해제하고 다시 빈 상태로 만든다. 두 번 불러도 안전하다. */
void  buf_free(Buf *b);
/* 내용만 지우고(len=0) 할당된 메모리는 유지한다. 같은 버퍼를 재사용할 때 쓴다. */
void  buf_reset(Buf *b);
/* 앞으로 extra 바이트를 더 넣을 수 있도록 용량을 미리 늘린다.
 * 성공하면 1, 메모리 확보에 실패하면 0 을 돌려준다. */
int   buf_reserve(Buf *b, size_t extra);
/* s 에서 정확히 n 바이트를 버퍼 끝에 덧붙인다. (중간에 '\0' 이 있어도 복사) */
void  buf_add(Buf *b, const char *s, size_t n);
/* NUL 종료 문자열 s 전체를 버퍼 끝에 덧붙인다. */
void  buf_puts(Buf *b, const char *s);
/* 문자 하나를 버퍼 끝에 덧붙인다. */
void  buf_putc(Buf *b, char c);
/* printf 와 같은 형식 문자열로 서식화한 결과를 버퍼 끝에 덧붙인다. */
void  buf_printf(Buf *b, const char *fmt, ...);

/* 문자열 도우미 */
/* s 를 힙에 복사해 새 문자열을 돌려준다 (호출자가 free). s 가 NULL 이면 NULL. */
char *str_dup(const char *s);
/* dst(크기 dstsz) 에 src 를 복사한다. 넘치면 잘라내고 항상 '\0' 으로 끝낸다.
 * strncpy 와 달리 NUL 종료가 보장되므로 고정 크기 배열에 안전하게 쓸 수 있다. */
void  str_copy(char *dst, size_t dstsz, const char *src);
int   str_ieq(const char *a, const char *b);        /* 대소문자 무시 비교 (같으면 1) */
void  str_trim(char *s);                            /* 양끝 공백 제거 (제자리) */

/* 16진 난수 토큰 생성. out 은 n+1 바이트 이상이어야 한다.
 * OS 난수원을 쓰며, 실패하면 0 을 돌려준다 (반드시 확인할 것).
 *   - n 은 만들 16진 문자 개수이다. (예: n=48 이면 24 바이트 난수 → 48 글자)
 *   - 세션 토큰, 이메일 인증 토큰처럼 "추측되면 안 되는 값" 을 만드는 데 쓴다.
 *   - rand() 같은 의사 난수는 예측 가능하므로 보안 용도로 쓰면 안 된다. */
int   random_hex(char *out, size_t n);

/* 로그
 * 모두 printf 형식 문자열을 받으며, 앞에 시각과 등급([INFO]/[WARN]/[ERR])을
 * 붙여 한 줄로 출력한다. 줄바꿈은 함수가 알아서 붙이므로 fmt 에 넣지 않는다. */
void  log_info(const char *fmt, ...);   /* 정상 동작 기록 (서버 시작, 수집 결과 등) */
void  log_warn(const char *fmt, ...);   /* 이상하지만 계속 진행 가능한 상황 */
void  log_err(const char *fmt, ...);    /* 실패한 작업 (DB 오류, 네트워크 오류 등) */

#endif /* SKU_COMMON_H */
