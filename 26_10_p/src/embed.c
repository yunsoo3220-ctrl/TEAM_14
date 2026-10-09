/* 딥러닝 문장 임베딩 클라이언트 (WinHTTP -> ml/embed_server.py). 설명은 embed.h 참고. */
#include "embed.h"
#include "common.h"
#include "json.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBED_TEXT_MAX 4000        /* 모델이 512 토큰까지만 보므로 그 이상은 보내지 않는다 */
#define HEALTH_TTL_MS  30000

static INTERNET_PORT embed_port(void)
{
    const char *p = getenv("SKU_EMBED_PORT");
    int n = p ? atoi(p) : 0;
    return (INTERNET_PORT)(n > 0 && n < 65536 ? n : 8001);
}

/* 127.0.0.1 에 요청을 보낸다. 응답을 받으면 1 (HTTP 상태와 무관). */
static int local_call(const wchar_t *method, const wchar_t *path, const char *body,
                      DWORD recv_timeout, Buf *out, DWORD *status)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD len = body ? (DWORD)strlen(body) : 0;
    DWORD statlen = sizeof *status;
    int ok = 0;

    *status = 0;
    ses = WinHttpOpen(L"SKU-Contest-Board/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        return 0;
    WinHttpSetTimeouts(ses, 2000, 2000, 30000, (int)recv_timeout);
    con = WinHttpConnect(ses, L"127.0.0.1", embed_port(), 0);
    if (con)
        req = WinHttpOpenRequest(con, method, path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req)
        goto done;
    if (!WinHttpSendRequest(req, body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
                            (DWORD)-1L, (LPVOID)body, len, len, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto done;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, status, &statlen, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD avail = 0, got = 0;
        char chunk[16384];

        if (!WinHttpQueryDataAvailable(req, &avail))
            goto done;
        if (avail == 0)
            break;
        if (avail > sizeof chunk)
            avail = sizeof chunk;
        if (!WinHttpReadData(req, chunk, avail, &got) || got == 0)
            break;
        buf_add(out, chunk, got);
    }
    ok = 1;

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

/* 서비스 상태가 바뀔 때만 로그를 남긴다. */
static volatile LONG g_last_ok = -1;

static void note_state(int ok, const char *why)
{
    if (InterlockedExchange(&g_last_ok, ok) == ok)
        return;
    if (ok)
        log_info("임베딩 서비스 연결됨 (127.0.0.1:%u)", (unsigned)embed_port());
    else
        log_warn("임베딩 서비스를 쓸 수 없어 TF-IDF 만으로 추천합니다 (%s). "
                 "ml\\embed_server.py 를 띄우세요.", why);
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* base64 를 풀어 out 에 쓴다. 쓴 바이트 수, 형식이 틀리면 -1. */
static long b64decode(const char *s, unsigned char *out, size_t outsz)
{
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;

    for (; *s && *s != '='; s++) {
        int v = b64val((unsigned char)*s);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outsz)
                return -1;
            out[n++] = (unsigned char)(acc >> bits);
        }
    }
    return (long)n;
}

/* UTF-8 글자 중간을 피해 maxbytes 이하 길이를 돌려준다. */
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

int embed_texts(const char *kind, const char *const *texts, int n, float **out,
                char *model, size_t modelsz)
{
    Buf req, resp;
    DWORD status = 0;
    Json *root = NULL;
    const char *data;
    float *vecs = NULL;
    long dim = 0, got;
    size_t need;
    int i;

    *out = NULL;
    if (model && modelsz)
        model[0] = '\0';
    if (n <= 0)
        return 0;

    buf_init(&req);
    buf_puts(&req, "{\"kind\":");
    json_write_str(&req, kind);
    buf_puts(&req, ",\"texts\":[");
    for (i = 0; i < n; i++) {
        const char *t = texts[i] ? texts[i] : "";
        size_t len = utf8_len_cut(t, EMBED_TEXT_MAX);
        char *cut = (char *)malloc(len + 1);

        if (!cut) {
            buf_free(&req);
            return 0;
        }
        memcpy(cut, t, len);
        cut[len] = '\0';
        if (i)
            buf_putc(&req, ',');
        json_write_str(&req, cut);
        free(cut);
    }
    buf_puts(&req, "]}");

    buf_init(&resp);
    /* 처음 한 번은 전체 글을 계산하느라 오래 걸릴 수 있다. 다음부터는 캐시에서 바로 온다. */
    if (!local_call(L"POST", L"/embed", req.data, 300000, &resp, &status)) {
        note_state(0, "연결 실패");
        goto fail;
    }
    if (status != 200) {
        char why[48];
        snprintf(why, sizeof why, "HTTP %lu", status);
        note_state(0, why);
        goto fail;
    }

    root = json_parse(resp.data ? resp.data : "");
    dim = root ? json_int(root, "dim", 0) : 0;
    data = root ? json_str(root, "data", NULL) : NULL;
    if (dim <= 0 || dim > 8192 || json_int(root, "n", -1) != n || !data) {
        note_state(0, "응답 형식 오류");
        goto fail;
    }
    need = (size_t)n * (size_t)dim * sizeof(float);
    vecs = (float *)malloc(need);
    if (!vecs)
        goto fail;
    /* 서비스는 little-endian float32 로 보낸다 (x86/x64 와 같다). */
    got = b64decode(data, (unsigned char *)vecs, need);
    if (got != (long)need) {
        note_state(0, "벡터 길이 불일치");
        goto fail;
    }
    if (model && modelsz)
        str_copy(model, modelsz, json_str(root, "model", ""));

    note_state(1, NULL);
    json_free(root);
    buf_free(&resp);
    buf_free(&req);
    *out = vecs;
    return (int)dim;

fail:
    free(vecs);
    json_free(root);
    buf_free(&resp);
    buf_free(&req);
    return 0;
}

int embed_available(void)
{
    static volatile LONG     cached = 0;
    static volatile ULONGLONG checked_at = 0;
    ULONGLONG now = GetTickCount64();
    Buf resp;
    DWORD status = 0;
    int ok;

    if (checked_at && now - checked_at < HEALTH_TTL_MS)
        return (int)cached;

    buf_init(&resp);
    ok = local_call(L"GET", L"/health", NULL, 3000, &resp, &status) && status == 200;
    buf_free(&resp);
    InterlockedExchange(&cached, ok);
    checked_at = now;
    return ok;
}
