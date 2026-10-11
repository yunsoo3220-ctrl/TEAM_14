/* HTTP(S) 클라이언트 (libcurl). 설명은 net.h 참고. */
#include "net.h"
#include "platform.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#define USER_AGENT "SKU-Contest-Board/1.0"

void net_init(void)
{
    static volatile long inited;   /* 0 = 아직, 1 = 했음 */

    /* 처음 한 스레드만 0→1 로 바꾸는 데 성공해 초기화한다 (원자적 비교-교환) */
    if (atomic_cas(&inited, 1, 0) == 0)
        curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* curl 이 받은 본문 조각을 Buf 에 붙인다. 처리한 바이트 수를 돌려줘야 계속 받는다. */
static size_t write_cb(char *data, size_t size, size_t n, void *ud)
{
    buf_add((Buf *)ud, data, size * n);
    return size * n;
}

int net_request(const char *method, const char *url, const char *headers, const char *body,
                long connect_ms, long total_ms, int no_proxy,
                Buf *out, long *status, char *err, size_t errsz)
{
    struct curl_slist *hl = NULL;   /* 요청 헤더 목록 */
    CURL *c;
    CURLcode rc;

    *status = 0;
    if (err && errsz)
        err[0] = '\0';
    net_init();
    c = curl_easy_init();
    if (!c) {
        str_copy(err, errsz, "HTTP 모듈을 초기화하지 못했습니다.");
        return 0;
    }

    /* "A: b\r\nC: d\r\n" 를 한 줄씩 잘라 curl 헤더 목록으로 */
    if (headers) {
        const char *p = headers;
        while (*p) {
            const char *e = strstr(p, "\r\n");
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len) {
                char line[1024];
                if (len >= sizeof line)
                    len = sizeof line - 1;
                memcpy(line, p, len);
                line[len] = '\0';
                hl = curl_slist_append(hl, line);
            }
            if (!e)
                break;
            p = e + 2;
        }
    }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    }
    if (hl)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, total_ms);
    /* 여러 스레드에서 쓰므로 시간 제한에 시그널(SIGALRM)을 쓰지 않게 한다 */
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");   /* gzip 등 지원하는 압축은 모두 받는다 */
    if (no_proxy)
        curl_easy_setopt(c, CURLOPT_NOPROXY, "*");

    rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    } else if (err && errsz) {
        snprintf(err, errsz, "%s", curl_easy_strerror(rc));
    }

    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}
