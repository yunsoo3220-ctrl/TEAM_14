/* 운영체제 차이를 감추는 얇은 층 (Windows / Linux / macOS)
 *
 * 서버 코드는 원래 Windows API(CRITICAL_SECTION, CreateThread, Interlocked*, Sleep ...)를
 * 직접 불렀다. 같은 소스를 리눅스·맥에서도 빌드하려고, 운영체제마다 다른 부분만 여기에 모았다.
 *   Windows : Win32 API 그대로
 *   그 밖    : POSIX (pthread, clock_gettime, /dev/urandom ...)
 *
 * 소켓 차이(Winsock vs BSD 소켓)는 http.c 한 곳에서만 쓰므로 그 파일 안에서 처리한다.
 */
#ifndef SKU_PLATFORM_H
#define SKU_PLATFORM_H

#include <stddef.h>
#include <stdint.h>   /* uint64_t */
#include <time.h>     /* time_t, struct tm */

#ifdef _WIN32
#  include <winsock2.h>   /* windows.h 보다 먼저 와야 옛 winsock.h 와 충돌하지 않는다 */
#  include <windows.h>
typedef CRITICAL_SECTION Mutex;
#else
#  include <pthread.h>
typedef pthread_mutex_t  Mutex;
#endif

/* 프로그램 시작 때 한 번 부른다.
 *   Windows : 콘솔 출력 코드페이지를 UTF-8 로 (한글 로그가 깨지지 않게)
 *   POSIX   : SIGPIPE 무시 (끊긴 소켓에 send 하면 프로세스가 통째로 죽는 것을 막는다) */
void platform_init(void);

/* 재진입 가능한 잠금 (같은 스레드가 여러 번 잠가도 교착되지 않음).
 * Windows 의 CRITICAL_SECTION 과 같은 성질이 필요해서 POSIX 에서는 RECURSIVE 뮤텍스를 쓴다. */
void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

/* fn(arg) 를 새 스레드에서 돌린다. 기다리지 않는(detached) 스레드다. 성공 1, 실패 0. */
int  thread_start(void (*fn)(void *), void *arg);

void     sleep_ms(unsigned ms);   /* 0 이면 남은 시간만 양보 */
uint64_t tick_ms(void);           /* 단조 증가 시계(ms). 시각 계산이 아니라 경과 시간 측정용 */

/* 스레드 안전한 localtime / gmtime (Windows: *_s, POSIX: *_r) */
void local_time(time_t t, struct tm *out);
void utc_time(time_t t, struct tm *out);

/* OS 난수원(Windows: rand_s, POSIX: /dev/urandom)으로 n 바이트를 채운다. 성공 1. */
int  os_random(void *buf, size_t n);

/* long 값에 대한 원자 연산 (gcc/clang 내장 함수 - MinGW, Linux gcc, macOS clang 모두 지원)
 *   atomic_cas(p, 새값, 비교값) : *p 가 비교값이면 새값으로 바꾼다. 바꾸기 전 값을 돌려준다.
 *                                (InterlockedCompareExchange 와 같은 인자 순서)
 *   atomic_swap(p, 새값)        : 새값을 넣고 이전 값을 돌려준다 (InterlockedExchange) */
#define atomic_cas(p, newv, cmp) __sync_val_compare_and_swap((p), (cmp), (newv))
#define atomic_swap(p, v)        __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

#endif /* SKU_PLATFORM_H */
