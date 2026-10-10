/* Winsock2 기반 HTTP/1.1 서버와 요청/응답 도우미
 *
 * HTTP 요청의 모양 (텍스트 프로토콜):
 *     POST /api/auth/login?x=1 HTTP/1.1\r\n      ← 요청 라인: 메서드 경로 버전
 *     Host: www.sku14.com\r\n                     ← 헤더들 (이름: 값)
 *     Content-Type: application/json\r\n
 *     Content-Length: 27\r\n
 *     \r\n                                         ← 빈 줄 = 헤더 끝
 *     {"student_no":"2024..."}                     ← 본문 (Content-Length 바이트)
 * 응답도 같은 구조로 "HTTP/1.1 200 OK\r\n" + 헤더 + 빈 줄 + 본문이다.
 *
 * 스레드 모델: 연결 하나당 스레드 하나 (thread-per-connection).
 *   간단하고 이해하기 쉽지만 동시 접속이 수천 개가 되면 스레드가 너무 많아진다.
 *   과제 규모(동시 수십 명)에서는 충분하다.
 */
#include "http.h"
#include "db.h"

#include <winsock2.h>   /* Windows 소켓 API (반드시 windows.h 보다 먼저) */
#include <ws2tcpip.h>   /* inet_ntop */
#include <windows.h>    /* CreateThread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RECV_CHUNK      8192    /* recv 한 번에 읽는 최대 바이트 */
#define HEADER_MAX      16384   /* 헤더 최대 크기(16KB). 넘으면 연결을 끊는다 (메모리 고갈 공격 방지) */
#define SOCK_TIMEOUT_MS 15000   /* 소켓 송수신 시간 제한 15초 (느린 연결이 스레드를 붙잡는 것 방지) */

/* 요청을 처리할 콜백 (http_serve 에서 받아 저장). 모든 스레드가 읽기만 하므로 안전하다. */
static void (*g_dispatch)(Request *, Response *);

/* ------------------------------------------------------------------ 유틸 */

/* 상태 코드 → 이유 문구 (응답 첫 줄 "HTTP/1.1 404 Not Found" 의 뒷부분) */
static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";               /* 새 자원 생성 성공 */
    case 202: return "Accepted";              /* 접수됨 (백그라운드 처리 예정) */
    case 204: return "No Content";
    case 304: return "Not Modified";          /* 캐시된 것을 그대로 쓰라 */
    case 400: return "Bad Request";           /* 요청 형식 오류 */
    case 401: return "Unauthorized";          /* 로그인 필요 */
    case 403: return "Forbidden";             /* 권한 없음 */
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";              /* 중복 등 상태 충돌 */
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";           /* 외부 서비스(학교 홈페이지, AI) 오류 */
    default:  return "Unknown";
    }
}

/* 16진 문자 하나 → 0~15, 아니면 -1 */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 퍼센트 인코딩 해제. 읽는 포인터 s 와 쓰는 포인터 w 를 따로 두고 같은 버퍼에 덮어쓴다.
 * "%41" → 'A', "+" → ' ', 잘못된 "%zz" 는 그대로 둔다. */
void url_decode(char *s)
{
    char *w = s;   /* 쓰기 위치 (항상 s 보다 같거나 뒤처진다) */

    while (*s) {
        if (*s == '%' ) {
            int h = hexval(s[1]);                       /* 상위 4비트 */
            int l = (h >= 0) ? hexval(s[2]) : -1;       /* 하위 4비트 (s[1] 이 NUL 이면 s[2] 를 읽지 않음) */
            if (l >= 0) {
                *w++ = (char)(h * 16 + l);
                s += 3;
                continue;
            }
        }
        if (*s == '+') {         /* 폼 인코딩에서 + 는 공백 */
            *w++ = ' ';
            s++;
            continue;
        }
        *w++ = *s++;
    }
    *w = '\0';
}

/* "a=1&b=2" 에서 name 의 값을 찾아 URL 디코딩해 out 에 쓴다. */
int req_query(const Request *req, const char *name, char *out, size_t outsz)
{
    const char *p = req->query;
    size_t nlen = strlen(name);

    out[0] = '\0';
    while (*p) {
        const char *amp = strchr(p, '&');                       /* 이번 "키=값" 조각의 끝 */
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);       /* 조각 길이 */

        /* 조각이 "name=" 으로 시작하는지 확인. p[nlen] == '=' 검사가 있어야
         * name="q" 일 때 "qq=1" 같은 다른 키를 잘못 잡지 않는다. */
        if (seg > nlen && p[nlen] == '=' && strncmp(p, name, nlen) == 0) {
            size_t vlen = seg - nlen - 1;                       /* 값 길이 */
            if (vlen >= outsz)
                vlen = outsz - 1;
            memcpy(out, p + nlen + 1, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return 1;
        }
        if (!amp)
            break;
        p = amp + 1;                                            /* 다음 조각으로 */
    }
    return 0;
}

/* "sid=abc; other=1" 에서 쿠키 값을 찾는다. */
int req_cookie(const Request *req, const char *name, char *out, size_t outsz)
{
    const char *p = req->cookie;
    size_t nlen = strlen(name);

    out[0] = '\0';
    while (*p) {
        while (*p == ' ' || *p == ';')     /* 구분자와 공백 건너뛰기 */
            p++;
        if (!*p)
            break;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;            /* 값 시작 */
            const char *end = strchr(v, ';');        /* 값 끝 */
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            if (vlen >= outsz)
                vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        p = strchr(p, ';');                          /* 다음 쿠키로 */
        if (!p)
            break;
    }
    return 0;
}

int req_is(const Request *req, const char *method, const char *path)
{
    return strcmp(req->method, method) == 0 && strcmp(req->path, path) == 0;
}

/* "/api/posts/12/comments" 같은 경로에서 숫자 id 를 꺼낸다. (사용법은 http.h 참고) */
int path_id(const char *path, const char *prefix, const char *suffix, unsigned *id)
{
    size_t plen = strlen(prefix);
    const char *p;
    unsigned long v;
    char *end;

    if (strncmp(path, prefix, plen) != 0)       /* 접두어가 다르면 아님 */
        return 0;
    p = path + plen;
    if (!isdigit((unsigned char)*p))            /* 바로 숫자가 와야 함 ("-1", " 1" 거부) */
        return 0;

    v = strtoul(p, &end, 10);                   /* end = 숫자가 끝난 위치 */
    if (v == 0 || v > 0xFFFFFFFFul)             /* id 는 1 이상, 32비트 범위 */
        return 0;

    if (suffix) {
        if (strcmp(end, suffix) != 0)           /* 숫자 뒤가 suffix 와 정확히 같아야 */
            return 0;
    } else if (*end != '\0') {                  /* suffix 가 없으면 숫자로 끝나야 */
        return 0;
    }

    *id = (unsigned)v;
    return 1;
}

/* ---------------------------------------------------------------- 응답 */

void res_init(Response *res)
{
    res->status = 200;
    str_copy(res->content_type, sizeof res->content_type, "text/plain; charset=utf-8");
    buf_init(&res->headers);
    buf_init(&res->body);
}

void res_free(Response *res)
{
    buf_free(&res->headers);
    buf_free(&res->body);
}

/* 헤더 한 줄 추가. 값은 그대로 들어가므로 신뢰할 수 있는 값만 넘길 것. */
void res_header(Response *res, const char *name, const char *value)
{
    buf_printf(&res->headers, "%s: %s\r\n", name, value);
}

/* 쿠키 설정.
 *   Path=/          사이트 전체 경로에서 쿠키를 보낸다
 *   HttpOnly        자바스크립트(document.cookie)로 읽을 수 없게 → XSS 로 세션을 훔치기 어려워짐
 *   SameSite=Lax    다른 사이트에서 보낸 POST 요청에는 쿠키를 붙이지 않음 → CSRF 방어
 *   Max-Age=초      유효 시간. 0 이면 브라우저가 즉시 지운다 (로그아웃) */
void res_set_cookie(Response *res, const char *name, const char *value, long max_age)
{
    buf_printf(&res->headers,
               "Set-Cookie: %s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%ld\r\n",
               name, value, max_age);
}

/* JSON 문자열을 복사해 본문으로 설정 */
void res_json(Response *res, int status, const char *json)
{
    res->status = status;
    str_copy(res->content_type, sizeof res->content_type, "application/json; charset=utf-8");
    buf_reset(&res->body);
    buf_puts(&res->body, json);
}

/* 이미 조립한 Buf 를 복사 없이 본문으로 넘긴다. 큰 응답에서 복사 비용을 아낀다.
 * 호출 후 body 는 빈 버퍼가 되므로 호출자가 buf_free 해도 안전하다. */
void res_json_buf(Response *res, int status, Buf *body)
{
    res->status = status;
    str_copy(res->content_type, sizeof res->content_type, "application/json; charset=utf-8");
    buf_free(&res->body);   /* 기존 본문이 있었다면 버린다 */
    res->body = *body;      /* 소유권 이전 (구조체 복사 = 포인터만 옮김) */
    buf_init(body);         /* 원래 버퍼는 비워 이중 해제를 막는다 */
}

/* 표준 오류 응답 {"error":{"code":"...","message":"..."}} */
void res_error(Response *res, int status, const char *code, const char *message)
{
    Buf b;

    buf_init(&b);
    buf_puts(&b, "{\"error\":{");
    buf_puts(&b, "\"code\":\"");
    buf_puts(&b, code);                  /* code 는 코드 안의 상수만 쓰므로 이스케이프하지 않는다 */
    buf_puts(&b, "\",\"message\":");
    /* 메시지는 JSON 문자열로 이스케이프해야 하므로 json.c 대신 간단히 처리
     * (" 와 \ 앞에 역슬래시, 줄바꿈은 \n 으로) */
    buf_putc(&b, '"');
    for (; *message; message++) {
        if (*message == '"' || *message == '\\')
            buf_putc(&b, '\\');
        if (*message == '\n')
            buf_puts(&b, "\\n");
        else
            buf_putc(&b, *message);
    }
    buf_puts(&b, "\"}}");
    res_json_buf(res, status, &b);
    buf_free(&b);                        /* 이미 비어 있으므로 아무 일도 안 함 (습관적 정리) */
}

void res_text(Response *res, int status, const char *text)
{
    res->status = status;
    str_copy(res->content_type, sizeof res->content_type, "text/plain; charset=utf-8");
    buf_reset(&res->body);
    buf_puts(&res->body, text);
}

/* 리다이렉트. 브라우저는 Location 헤더를 보고 바로 이동하지만,
 * 헤더를 따르지 않는 클라이언트를 위해 본문에도 링크를 넣는다. */
void res_redirect(Response *res, int status, const char *location)
{
    res->status = status;
    res_header(res, "Location", location);
    str_copy(res->content_type, sizeof res->content_type, "text/html; charset=utf-8");
    buf_reset(&res->body);
    buf_printf(&res->body,
               "<!doctype html><meta charset=\"utf-8\">"
               "<title>이동</title><p><a href=\"%s\">%s</a> 로 이동합니다.</p>",
               location, location);
}

/* "호스트:포트" 에서 포트를 뗀다. */
void host_name_only(const char *host_header, char *out, size_t outsz)
{
    const char *colon;

    str_copy(out, outsz, host_header);
    colon = strchr(out, ':');
    if (colon)
        out[colon - out] = '\0';
}

/* ------------------------------------------------------- 요청 읽기/처리 */

/* len 바이트를 모두 보낼 때까지 send 를 반복한다.
 * send 는 한 번에 일부만 보낼 수 있으므로(커널 버퍼가 차면) 반드시 반복해야 한다. */
static int send_all(SOCKET s, const char *data, size_t len)
{
    while (len) {
        int n = send(s, data, (int)(len > INT_MAX ? INT_MAX : len), 0);   /* send 는 int 길이만 받음 */
        if (n <= 0)              /* 오류 또는 연결 끊김 */
            return 0;
        data += n;
        len -= (size_t)n;
    }
    return 1;
}

/* Response 를 HTTP 텍스트로 직렬화해 보낸다. */
static void send_response(SOCKET s, Response *res, int keep_alive)
{
    Buf head;

    buf_init(&head);
    buf_printf(&head, "HTTP/1.1 %d %s\r\n", res->status, status_text(res->status));   /* 상태 줄 */
    buf_printf(&head, "Content-Type: %s\r\n", res->content_type);
    buf_printf(&head, "Content-Length: %zu\r\n", res->body.len);   /* Keep-Alive 에서 본문 끝을 알리는 데 필수 */
    buf_printf(&head, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close");
    /* 브라우저가 Content-Type 을 무시하고 내용을 추측(sniffing)하지 못하게 한다.
     * (예: 텍스트 파일을 스크립트로 해석해 실행하는 일을 막음) */
    buf_puts(&head, "X-Content-Type-Options: nosniff\r\n");
    if (res->headers.len)                                           /* 핸들러가 추가한 헤더들 */
        buf_add(&head, res->headers.data, res->headers.len);
    buf_puts(&head, "\r\n");                                        /* 헤더 끝 빈 줄 */

    send_all(s, head.data, head.len);
    if (res->body.len)
        send_all(s, res->body.data, res->body.len);
    buf_free(&head);
}

/* 헤더 한 줄에서 값 부분을 꺼낸다. "Host:  www.a.com " → "www.a.com" */
static void header_value(const char *line, char *out, size_t outsz)
{
    const char *colon = strchr(line, ':');

    if (!colon) {
        out[0] = '\0';
        return;
    }
    colon++;
    while (*colon == ' ' || *colon == '\t')   /* 콜론 뒤 공백 건너뛰기 */
        colon++;
    str_copy(out, outsz, colon);
    str_trim(out);                            /* 뒤쪽 공백/CR 제거 */
}

/* 요청 하나를 파싱한다. 0 이면 연결을 닫아야 한다.
 *   head/head_len : 요청 라인부터 빈 줄(\r\n\r\n)까지의 헤더 영역 */
static int parse_request(const char *head, size_t head_len, Request *req)
{
    char line[2048];                  /* 한 줄을 복사해 다룰 작업 버퍼 */
    const char *p = head;
    const char *end = head + head_len;
    const char *sp1, *sp2, *q;        /* 첫 번째/두 번째 공백 위치, '?' 위치 */
    size_t n;

    memset(req, 0, sizeof *req);      /* 모든 필드를 빈 문자열/0/NULL 로 */

    /* 요청 라인: "GET /path?query HTTP/1.1" */
    {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            return 0;
        n = (size_t)(nl - p);
        if (n && p[n - 1] == '\r')    /* CRLF 의 CR 제거 */
            n--;
        if (n >= sizeof line)
            return 0;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl + 1;                   /* 다음 줄(첫 헤더)로 */
    }

    sp1 = strchr(line, ' ');          /* 메서드 끝 */
    if (!sp1)
        return 0;
    sp2 = strchr(sp1 + 1, ' ');       /* 경로 끝 (뒤는 HTTP 버전) */
    if (!sp2)
        return 0;

    if ((size_t)(sp1 - line) >= sizeof req->method)   /* 메서드가 비정상적으로 길면 거부 */
        return 0;
    memcpy(req->method, line, (size_t)(sp1 - line));  /* memset 덕분에 NUL 종료 */

    /* 경로와 쿼리 분리 */
    n = (size_t)(sp2 - sp1 - 1);
    if (n >= sizeof req->path)
        return 0;
    memcpy(req->path, sp1 + 1, n);
    req->path[n] = '\0';

    q = strchr(req->path, '?');
    if (q) {
        str_copy(req->query, sizeof req->query, q + 1);   /* 쿼리는 디코딩하지 않고 원본 그대로 */
        req->path[q - req->path] = '\0';                   /* 경로에서 쿼리 부분을 잘라냄 */
    }
    url_decode(req->path);   /* 쿼리를 떼어 낸 뒤 디코딩해야 "%3F"(?)가 쿼리 구분자로 오인되지 않는다 */

    /* 헤더들: 필요한 4개만 골라 저장하고 나머지는 무시한다 */
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (len && p[len - 1] == '\r')
            len--;
        if (len == 0)                 /* 빈 줄 = 헤더 끝 */
            break;
        if (len < sizeof line) {      /* 너무 긴 헤더 줄은 조용히 무시 */
            memcpy(line, p, len);
            line[len] = '\0';

            /* 헤더 이름은 대소문자를 구분하지 않으므로 _strnicmp 로 비교 */
            if (_strnicmp(line, "Cookie:", 7) == 0)
                header_value(line, req->cookie, sizeof req->cookie);
            else if (_strnicmp(line, "Content-Type:", 13) == 0)
                header_value(line, req->content_type, sizeof req->content_type);
            else if (_strnicmp(line, "Host:", 5) == 0)
                header_value(line, req->host, sizeof req->host);
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return 1;
}

/* 헤더 영역에서 Content-Length 값을 찾는다. 없으면 0 (본문 없음).
 * 줄 맨 앞에서만 찾도록 '\n' 바로 뒤를 검사한다. (첫 줄은 요청 라인이라 건너뛰어도 됨) */
static long content_length_of(const char *head)
{
    const char *p = head;

    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (_strnicmp(p, "Content-Length:", 15) == 0) {
            char v[32];
            header_value(p, v, sizeof v);
            return atol(v);       /* 음수가 올 수도 있으므로 호출자가 검사한다 */
        }
    }
    return 0;
}

/* 클라이언트가 "Connection: close" 를 보냈으면 1 (이번 응답 후 연결을 닫는다) */
static int wants_close(const char *head)
{
    const char *p = head;

    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (_strnicmp(p, "Connection:", 11) == 0) {
            char v[64];
            header_value(p, v, sizeof v);
            return str_ieq(v, "close");
        }
    }
    return 0;
}

/* 연결 하나에서 요청들을 차례로 처리한다 (Keep-Alive).
 * in 버퍼에는 소켓에서 받은 바이트가 쌓이며, 한 번의 recv 로 요청 두 개가 같이
 * 들어올 수도 있으므로 처리한 만큼만 앞에서 잘라내고 나머지는 다음 요청에 쓴다. */
static void handle_connection(SOCKET sock, const char *client_ip)
{
    Buf in;          /* 받은 바이트 누적 버퍼 */
    int alive = 1;   /* 연결을 계속 유지할지 */

    buf_init(&in);

    while (alive) {
        size_t head_len = 0;   /* 헤더 영역 길이 (빈 줄 포함) */
        char *split = NULL;    /* "\r\n\r\n" 위치 */
        long clen;             /* 본문 길이 */
        Request req;
        Response res;
        size_t consumed;       /* 이번 요청이 차지한 바이트 */

        /* 헤더가 끝날 때까지 읽는다 (이전 요청의 잔여 바이트부터 검사). */
        for (;;) {
            char chunk[RECV_CHUNK];
            int n;

            if (in.len) {
                in.data[in.len] = '\0';                  /* strstr 을 쓰기 위해 NUL 종료 보장 */
                split = strstr(in.data, "\r\n\r\n");
                if (split) {
                    head_len = (size_t)(split - in.data) + 4;
                    break;
                }
            }
            if (in.len > HEADER_MAX) {                   /* 헤더가 끝없이 오면 끊는다 */
                buf_free(&in);
                return;
            }
            n = recv(sock, chunk, sizeof chunk, 0);      /* 블로킹 (최대 SOCK_TIMEOUT_MS) */
            if (n <= 0) {                                /* 0 = 상대가 연결을 닫음, 음수 = 오류/시간초과 */
                buf_free(&in);
                return;
            }
            buf_add(&in, chunk, (size_t)n);
        }

        /* 본문 크기 검사: 너무 크면 메모리를 잡기 전에 거절한다 */
        clen = content_length_of(in.data);
        if (clen < 0 || clen > HTTP_MAX_BODY) {
            res_init(&res);
            res_error(&res, 413, "body_too_large", "요청 본문이 너무 큽니다.");
            send_response(sock, &res, 0);
            res_free(&res);
            buf_free(&in);
            return;
        }

        /* 본문을 모두 받을 때까지 더 읽는다. */
        while (in.len < head_len + (size_t)clen) {
            char chunk[RECV_CHUNK];
            int n = recv(sock, chunk, sizeof chunk, 0);
            if (n <= 0) {
                buf_free(&in);
                return;
            }
            buf_add(&in, chunk, (size_t)n);
        }

        if (!parse_request(in.data, head_len, &req)) {
            res_init(&res);
            res_error(&res, 400, "bad_request", "요청을 해석할 수 없습니다.");
            send_response(sock, &res, 0);
            res_free(&res);
            buf_free(&in);
            return;
        }

        str_copy(req.client_ip, sizeof req.client_ip, client_ip);
        alive = !wants_close(in.data);

        /* 본문을 별도 메모리로 복사한다 (in 버퍼는 다음 요청을 위해 재사용되므로) */
        if (clen > 0) {
            req.body = (char *)malloc((size_t)clen + 1);
            if (req.body) {
                memcpy(req.body, in.data + head_len, (size_t)clen);
                req.body[clen] = '\0';                  /* 문자열처럼 쓸 수 있게 NUL 종료 */
                req.body_len = (size_t)clen;
            }
        }

        /* 실제 처리: 라우터가 res 를 채운다 */
        res_init(&res);
        g_dispatch(&req, &res);
        send_response(sock, &res, alive);

        /* 접근 로그 (메서드, 경로, 상태, 본문 크기) */
        log_info("%s %s -> %d (%zu bytes)", req.method, req.path,
                 res.status, res.body.len);

        res_free(&res);
        free(req.body);

        /* 다음 요청을 위해 처리한 바이트를 버린다. */
        consumed = head_len + (size_t)clen;
        if (in.len > consumed) {            /* 다음 요청의 일부가 이미 들어와 있으면 앞으로 당긴다 */
            memmove(in.data, in.data + consumed, in.len - consumed);
            in.len -= consumed;
            in.data[in.len] = '\0';
        } else {
            buf_reset(&in);
        }
    }

    buf_free(&in);
}

/* 연결 스레드에 넘기는 인자 (스레드 함수는 void* 하나만 받으므로 구조체로 묶는다) */
typedef struct {
    SOCKET sock;      /* 클라이언트 소켓 */
    char   ip[48];    /* 클라이언트 IP 문자열 */
} ConnArg;

/* 연결 하나를 맡는 스레드의 진입점 */
static DWORD WINAPI conn_thread(LPVOID arg)
{
    ConnArg *ca = (ConnArg *)arg;
    DWORD timeout = SOCK_TIMEOUT_MS;

    db_thread_begin();   /* 이 스레드에서 MySQL 을 쓰기 전 초기화 */
    /* 수신/송신 시간 제한. 이것이 없으면 아무것도 안 보내는 클라이언트가 스레드를 영원히 붙잡는다. */
    setsockopt(ca->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(ca->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);

    handle_connection(ca->sock, ca->ip);

    shutdown(ca->sock, SD_BOTH);   /* 송수신을 모두 끝냈음을 상대에게 알린다 */
    closesocket(ca->sock);
    db_thread_end();
    free(ca);                      /* http_serve 에서 calloc 한 인자를 여기서 해제 */
    return 0;
}

/* 서버 메인 루프: 소켓을 열고 접속마다 스레드를 띄운다. 정상적으로는 돌아오지 않는다. */
int http_serve(unsigned short port, int bind_all,
               void (*dispatch)(Request *, Response *))
{
    WSADATA wsa;
    SOCKET listener;              /* 접속을 기다리는 소켓 */
    struct sockaddr_in addr;      /* IPv4 주소 + 포트 */
    int reuse = 1;

    g_dispatch = dispatch;

    /* Winsock 2.2 초기화 (Windows 에서 소켓을 쓰기 전 필수) */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_err("WSAStartup 실패");
        return 0;
    }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   /* IPv4 TCP 소켓 */
    if (listener == INVALID_SOCKET) {
        log_err("소켓 생성 실패 (%d)", WSAGetLastError());
        WSACleanup();
        return 0;
    }
    /* 서버를 껐다 바로 켤 때 "주소가 이미 사용 중" 오류를 줄이기 위한 옵션 */
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof reuse);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    /* 기본은 루프백만. --bind-all 을 주면 같은 네트워크의 다른 기기에서도 들어올 수 있다.
     * htonl/htons: 호스트 바이트 순서 → 네트워크 바이트 순서(빅엔디언) 변환 */
    addr.sin_addr.s_addr = htonl(bind_all ? INADDR_ANY : INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        log_err("포트 %u 바인드 실패 (%d) - 이미 사용 중인지 확인하세요.",
                port, WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return 0;
    }
    /* 접속 대기 시작. SOMAXCONN = 운영체제가 허용하는 최대 대기열 길이 */
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        log_err("listen 실패 (%d)", WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return 0;
    }

    if (port == 80)
        log_info("서버 시작: http://%s/", bind_all ? "0.0.0.0" : "127.0.0.1");
    else
        log_info("서버 시작: http://%s:%u/", bind_all ? "0.0.0.0" : "127.0.0.1", port);

    /* 무한 루프: 접속 수락 → 스레드 생성 */
    for (;;) {
        struct sockaddr_in peer;               /* 접속한 상대 주소 */
        int peerlen = sizeof peer;
        SOCKET s = accept(listener, (struct sockaddr *)&peer, &peerlen);   /* 접속이 올 때까지 블로킹 */
        ConnArg *ca;
        HANDLE th;

        if (s == INVALID_SOCKET) {
            log_warn("accept 실패 (%d)", WSAGetLastError());
            continue;
        }

        /* 스레드 인자는 힙에 만든다. 스택 변수를 넘기면 다음 반복에서 덮어써진다. */
        ca = (ConnArg *)calloc(1, sizeof *ca);
        if (!ca) {
            closesocket(s);
            continue;
        }
        ca->sock = s;
        inet_ntop(AF_INET, &peer.sin_addr, ca->ip, sizeof ca->ip);   /* 이진 주소 → "1.2.3.4" */

        th = CreateThread(NULL, 0, conn_thread, ca, 0, NULL);
        if (!th) {
            log_warn("스레드 생성 실패");
            closesocket(s);
            free(ca);
            continue;
        }
        /* 스레드 핸들을 닫아도 스레드는 계속 돈다. 기다릴(join) 필요가 없으므로
         * 바로 닫아 핸들이 쌓이지 않게 한다(detach 와 같은 효과). */
        CloseHandle(th);
    }
}
