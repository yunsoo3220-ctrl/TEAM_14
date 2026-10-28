/* platform.h 구현. 함수마다 Windows 쪽과 POSIX 쪽을 #ifdef 로 나눠 둔다. */

/* rand_s() 선언을 켜는 매크로. stdlib.h 보다 먼저 와야 한다 (MinGW/MSVC 전용, 다른 곳에선 무시됨). */
#define _CRT_RAND_S
/* POSIX 쪽에서 clock_gettime, localtime_r, pthread_mutexattr_settype 등을 선언하게 한다.
 * (-std=c11 은 순수 C 표준 함수만 보여 주므로 이 매크로가 있어야 한다) */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include "platform.h"

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#  include <errno.h>
#  include <fcntl.h>
#  include <sched.h>
#  include <signal.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------ 시작 설정 */

void platform_init(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

/* ------------------------------------------------------------ 잠금 */

void mutex_init(Mutex *m)
{
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   /* 재진입 가능 */
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
#endif
}

void mutex_destroy(Mutex *m)
{
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}

void mutex_lock(Mutex *m)
{
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}

void mutex_unlock(Mutex *m)
{
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

/* ------------------------------------------------------------ 스레드 */

/* 스레드 함수 모양이 OS 마다 달라서(DWORD WINAPI f(LPVOID) / void *f(void *))
 * 공통 모양 void f(void *) 를 감싸는 시작 함수를 하나 둔다. */
typedef struct {
    void (*fn)(void *);
    void  *arg;
} ThreadStart;

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p)
#else
static void *thread_main(void *p)
#endif
{
    ThreadStart ts = *(ThreadStart *)p;
    free(p);
    ts.fn(ts.arg);
    return 0;
}

int thread_start(void (*fn)(void *), void *arg)
{
    ThreadStart *ts = (ThreadStart *)malloc(sizeof *ts);

    if (!ts)
        return 0;
    ts->fn = fn;
    ts->arg = arg;
#ifdef _WIN32
    {
        HANDLE th = CreateThread(NULL, 0, thread_main, ts, 0, NULL);
        if (!th) {
            free(ts);
            return 0;
        }
        CloseHandle(th);   /* 기다리지 않으므로 핸들은 바로 닫는다 (detach) */
    }
#else
    {
        pthread_t th;
        pthread_attr_t a;
        int rc;

        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        /* 스택 크기를 Windows 기본(1MB)보다 넉넉한 2MB 로 맞춘다.
         * macOS 의 보조 스레드 기본값은 512KB 라 큰 지역 배열이 있으면 모자랄 수 있다. */
        pthread_attr_setstacksize(&a, 2u * 1024 * 1024);
        rc = pthread_create(&th, &a, thread_main, ts);
        pthread_attr_destroy(&a);
        if (rc != 0) {
            free(ts);
            return 0;
        }
    }
#endif
    return 1;
}

/* ------------------------------------------------------------ 시간 */

void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    if (ms == 0) {
        sched_yield();
    } else {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)   /* 시그널로 깨면 남은 시간만큼 더 잔다 */
            ;
    }
#endif
}

uint64_t tick_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

void local_time(time_t t, struct tm *out)
{
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

void utc_time(time_t t, struct tm *out)
{
#ifdef _WIN32
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}

/* ------------------------------------------------------------ 난수 */

int os_random(void *buf, size_t n)
{
#ifdef _WIN32
    unsigned char *p = (unsigned char *)buf;

    while (n) {
        unsigned int v;
        size_t k = n < sizeof v ? n : sizeof v;

        if (rand_s(&v) != 0)
            return 0;
        memcpy(p, &v, k);
        p += k;
        n -= k;
    }
    return 1;
#else
    unsigned char *p = (unsigned char *)buf;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0)
        return 0;
    while (n) {
        ssize_t got = read(fd, p, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            close(fd);
            return 0;
        }
        p += got;
        n -= (size_t)got;
    }
    close(fd);
    return 1;
#endif
}
