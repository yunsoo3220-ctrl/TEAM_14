/* 딥러닝 문장 임베딩 클라이언트 (libcurl -> ml/embed_server.py). 설명은 embed.h 참고.
 *
 * 통신 규약 (embed_server.py 와 맞춰야 함):
 *   POST /embed   요청 {"kind":"passage"|"query", "texts":["...", ...]}
 *                 응답 {"model":"...", "dim":768, "n":개수, "data":"<base64>"}
 *                 data = n x dim 개의 little-endian float32 를 이어 붙여 base64 로 인코딩한 것
 *   GET  /health  살아 있으면 200
 * 벡터를 JSON 숫자 배열로 보내면 768차원 x 수백 개 = 수십만 개 숫자를 파싱해야 해서 느리다.
 * 그래서 바이너리를 base64 로 한 번에 보낸다.
 */
#include "embed.h"
#include "common.h"
#include "json.h"
#include "net.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBED_TEXT_MAX 4000        /* 모델이 512 토큰까지만 보므로 그 이상은 보내지 않는다 */
#define HEALTH_TTL_MS  30000       /* /health 결과를 기억하는 시간 (30초) */

/* 임베딩 서비스 포트: 환경변수 SKU_EMBED_PORT, 없거나 이상하면 8001 */
static unsigned embed_port(void)
{
    const char *p = getenv("SKU_EMBED_PORT");
    int n = p ? atoi(p) : 0;
    return (unsigned)(n > 0 && n < 65536 ? n : 8001);
}

/* 127.0.0.1 에 요청을 보낸다. 응답을 받으면 1 (HTTP 상태와 무관).
 *   method/path   : "POST", "/embed"
 *   body          : 보낼 JSON (NULL 이면 본문 없음)
 *   recv_timeout  : 응답을 기다릴 최대 시간(ms)
 *   out           : 응답 본문을 붙일 버퍼
 *   status        : HTTP 상태 코드를 받을 곳 */
static int local_call(const char *method, const char *path, const char *body,
                      long recv_timeout, Buf *out, long *status)
{
    char url[64];

    snprintf(url, sizeof url, "http://127.0.0.1:%u%s", embed_port(), path);
    /* 접속 2초 제한: 서비스가 꺼져 있으면 금방 실패해 서버가 오래 멈추지 않는다.
     * 로컬 호출이므로 프록시를 쓰지 않는다(회사/학교 프록시 설정이 있어도 우회). */
    return net_request(method, url, body ? "Content-Type: application/json\r\n" : NULL, body,
                       2000, 2000 + recv_timeout, 1, out, status, NULL, 0);
}

/* 서비스 상태가 바뀔 때만 로그를 남긴다.
 * -1 = 아직 모름, 0 = 안 됨, 1 = 됨. 요청마다 "안 됩니다" 로그가 쌓이는 것을 막는다. */
static volatile long g_last_ok = -1;

static void note_state(int ok, const char *why)
{
    /* atomic_swap 은 새 값을 넣고 "이전 값" 을 원자적으로 돌려준다.
     * 이전 값이 지금과 같으면 상태가 안 바뀐 것이므로 조용히 돌아간다. */
    if (atomic_swap(&g_last_ok, (long)ok) == ok)
        return;
    if (ok)
        log_info("임베딩 서비스 연결됨 (127.0.0.1:%u)", (unsigned)embed_port());
    else
        log_warn("임베딩 서비스를 쓸 수 없어 TF-IDF 만으로 추천합니다 (%s). "
                 "ml/embed_server.py 를 띄우세요.", why);
}

/* base64 문자 하나 → 0~63 값. base64 문자가 아니면 -1. */
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* base64 를 풀어 out 에 쓴다. 쓴 바이트 수, 형식이 틀리면 -1.
 * 원리: 글자 하나 = 6비트. acc 에 6비트씩 쌓다가 8비트 이상 모이면 1바이트를 꺼낸다.
 * '=' 패딩을 만나면 끝으로 본다. */
static long b64decode(const char *s, unsigned char *out, size_t outsz)
{
    size_t n = 0;        /* 지금까지 쓴 바이트 수 */
    unsigned acc = 0;    /* 비트 누산기 */
    int bits = 0;        /* acc 에 쌓인 유효 비트 수 */

    for (; *s && *s != '='; s++) {
        int v = b64val((unsigned char)*s);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outsz)              /* 예상보다 길면 버퍼 넘침 방지 */
                return -1;
            out[n++] = (unsigned char)(acc >> bits);   /* 위쪽 8비트를 꺼낸다 (unsigned char 로 자르기) */
        }
    }
    return (long)n;
}

/* UTF-8 글자 중간을 피해 maxbytes 이하 길이를 돌려준다.
 * UTF-8 에서 10xxxxxx (0x80~0xBF) 바이트는 "앞 글자에 이어지는 바이트" 이다.
 * 자를 위치가 이어지는 바이트라면 글자의 첫 바이트가 나올 때까지 앞으로 물러난다.
 * 이렇게 해야 한글이 반쪽만 남아 깨진 문자가 생기지 않는다. */
static size_t utf8_len_cut(const char *s, size_t maxbytes)
{
    size_t n = strlen(s);

    if (n <= maxbytes)
        return n;
    n = maxbytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

/* 여러 문장을 한 번에 임베딩한다. 설명은 embed.h 참고. */
int embed_texts(const char *kind, const char *const *texts, int n, float **out,
                char *model, size_t modelsz)
{
    Buf req, resp;          /* 요청 JSON, 응답 본문 */
    long status = 0;
    Json *root = NULL;      /* 파싱한 응답 */
    const char *data;       /* 응답의 base64 벡터 문자열 */
    float *vecs = NULL;     /* 디코딩한 벡터 (성공 시 호출자에게 넘김) */
    long dim = 0, got;
    size_t need;            /* 기대하는 바이트 수 = n x dim x 4 */
    int i;

    *out = NULL;
    if (model && modelsz)
        model[0] = '\0';
    if (n <= 0)
        return 0;

    /* 1) 요청 JSON 조립: {"kind":"passage","texts":["...","..."]} */
    buf_init(&req);
    buf_puts(&req, "{\"kind\":");
    json_write_str(&req, kind);
    buf_puts(&req, ",\"texts\":[");
    for (i = 0; i < n; i++) {
        const char *t = texts[i] ? texts[i] : "";
        size_t len = utf8_len_cut(t, EMBED_TEXT_MAX);   /* 너무 긴 글은 앞부분만 */
        char *cut = (char *)malloc(len + 1);            /* 잘라낸 사본 (json_write_str 는 NUL 종료 문자열을 받음) */

        if (!cut) {
            buf_free(&req);
            return 0;
        }
        memcpy(cut, t, len);
        cut[len] = '\0';
        if (i)
            buf_putc(&req, ',');
        json_write_str(&req, cut);   /* 따옴표·줄바꿈 등을 이스케이프해서 넣는다 */
        free(cut);
    }
    buf_puts(&req, "]}");

    buf_init(&resp);
    /* 2) 요청 전송
     * 처음 한 번은 전체 글을 계산하느라 오래 걸릴 수 있다. 다음부터는 캐시에서 바로 온다.
     * (그래서 받기 제한을 300초로 넉넉히 준다) */
    if (!local_call("POST", "/embed", req.data, 300000, &resp, &status)) {
        note_state(0, "연결 실패");
        goto fail;
    }
    if (status != 200) {
        char why[48];
        snprintf(why, sizeof why, "HTTP %lu", status);
        note_state(0, why);
        goto fail;
    }

    /* 3) 응답 검증: 차원이 합리적인 범위인지, 개수가 보낸 것과 같은지, data 가 있는지 */
    root = json_parse(resp.data ? resp.data : "");
    dim = root ? json_int(root, "dim", 0) : 0;
    data = root ? json_str(root, "data", NULL) : NULL;
    if (dim <= 0 || dim > 8192 || json_int(root, "n", -1) != n || !data) {
        note_state(0, "응답 형식 오류");
        goto fail;
    }
    /* 4) 벡터 디코딩 */
    need = (size_t)n * (size_t)dim * sizeof(float);
    vecs = (float *)malloc(need);
    if (!vecs)
        goto fail;
    /* 서비스는 little-endian float32 로 보낸다 (x86/x64 와 같다).
     * 그래서 디코딩한 바이트를 float 배열 메모리에 그대로 써 넣으면 바로 쓸 수 있다. */
    got = b64decode(data, (unsigned char *)vecs, need);
    if (got != (long)need) {   /* 바이트 수가 정확히 맞아야 한다 */
        note_state(0, "벡터 길이 불일치");
        goto fail;
    }
    if (model && modelsz)
        str_copy(model, modelsz, json_str(root, "model", ""));

    /* 5) 성공: 벡터 소유권을 호출자에게 넘기고 나머지는 정리 */
    note_state(1, NULL);
    json_free(root);
    buf_free(&resp);
    buf_free(&req);
    *out = vecs;
    return (int)dim;

fail:
    free(vecs);        /* free(NULL) 은 안전 */
    json_free(root);
    buf_free(&resp);
    buf_free(&req);
    return 0;
}

/* 임베딩 서비스가 떠 있는지 확인한다. 결과를 30초 동안 캐시한다. */
int embed_available(void)
{
    static volatile long     cached = 0;       /* 마지막 확인 결과 */
    static volatile uint64_t checked_at = 0;   /* 마지막 확인 시각 (단조 시계 ms) */
    uint64_t now = tick_ms();                  /* 64비트 ms - 오래 켜 둬도 되돌아가지 않음 */
    Buf resp;
    long status = 0;
    int ok;

    /* 캐시가 유효하면 바로 돌려준다 */
    if (checked_at && now - checked_at < HEALTH_TTL_MS)
        return (int)cached;

    /* /health 에 3초 제한으로 물어본다. 동시에 여러 스레드가 확인해도 결과만 덮어쓰므로 무해하다. */
    buf_init(&resp);
    ok = local_call("GET", "/health", NULL, 3000, &resp, &status) && status == 200;
    buf_free(&resp);
    atomic_swap(&cached, (long)ok);
    checked_at = now;
    return ok;
}
