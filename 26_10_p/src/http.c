/* Winsock2 기반 HTTP/1.1 서버와 요청/응답 도우미 */
#include "http.h"
#include "db.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RECV_CHUNK      8192
#define HEADER_MAX      16384
#define SOCK_TIMEOUT_MS 15000

static void (*g_dispatch)(Request *, Response *);

/* ------------------------------------------------------------------ 유틸 */

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default:  return "Unknown";
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(char *s)
{
    char *w = s;

    while (*s) {
        if (*s == '%' ) {
            int h = hexval(s[1]);
            int l = (h >= 0) ? hexval(s[2]) : -1;
            if (l >= 0) {
                *w++ = (char)(h * 16 + l);
                s += 3;
                continue;
            }
        }
        if (*s == '+') {
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
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);

        if (seg > nlen && p[nlen] == '=' && strncmp(p, name, nlen) == 0) {
            size_t vlen = seg - nlen - 1;
            if (vlen >= outsz)
                vlen = outsz - 1;
            memcpy(out, p + nlen + 1, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return 1;
        }
        if (!amp)
            break;
        p = amp + 1;
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
        while (*p == ' ' || *p == ';')
            p++;
        if (!*p)
            break;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *end = strchr(v, ';');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            if (vlen >= outsz)
                vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        p = strchr(p, ';');
        if (!p)
            break;
    }
    return 0;
}

int req_is(const Request *req, const char *method, const char *path)
{
    return strcmp(req->method, method) == 0 && strcmp(req->path, path) == 0;
}

int path_id(const char *path, const char *prefix, const char *suffix, unsigned *id)
{
    size_t plen = strlen(prefix);
    const char *p;
    unsigned long v;
    char *end;

    if (strncmp(path, prefix, plen) != 0)
        return 0;
    p = path + plen;
    if (!isdigit((unsigned char)*p))
        return 0;

    v = strtoul(p, &end, 10);
    if (v == 0 || v > 0xFFFFFFFFul)
        return 0;

    if (suffix) {
        if (strcmp(end, suffix) != 0)
            return 0;
    } else if (*end != '\0') {
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

void res_header(Response *res, const char *name, const char *value)
{
    buf_printf(&res->headers, "%s: %s\r\n", name, value);
}

void res_set_cookie(Response *res, const char *name, const char *value, long max_age)
{
    buf_printf(&res->headers,
               "Set-Cookie: %s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%ld\r\n",
               name, value, max_age);
}

void res_json(Response *res, int status, const char *json)
{
    res->status = status;
    str_copy(res->content_type, sizeof res->content_type, "application/json; charset=utf-8");
    buf_reset(&res->body);
    buf_puts(&res->body, json);
}

void res_json_buf(Response *res, int status, Buf *body)
{
    res->status = status;
    str_copy(res->content_type, sizeof res->content_type, "application/json; charset=utf-8");
    buf_free(&res->body);
    res->body = *body;      /* 소유권 이전 */
    buf_init(body);
}

void res_error(Response *res, int status, const char *code, const char *message)
{
    Buf b;

    buf_init(&b);
    buf_puts(&b, "{\"error\":{");
    buf_puts(&b, "\"code\":\"");
    buf_puts(&b, code);
    buf_puts(&b, "\",\"message\":");
    /* 메시지는 JSON 문자열로 이스케이프해야 하므로 json.c 대신 간단히 처리 */
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
    buf_free(&b);
}

void res_text(Response *res, int status, const char *text)
{
    res->status = status;
    str_copy(res->content_type, sizeof res->content_type, "text/plain; charset=utf-8");
    buf_reset(&res->body);
    buf_puts(&res->body, text);
}

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

void host_name_only(const char *host_header, char *out, size_t outsz)
{
    const char *colon;

    str_copy(out, outsz, host_header);
    colon = strchr(out, ':');
    if (colon)
        out[colon - out] = '\0';
}

/* ------------------------------------------------------- 요청 읽기/처리 */

static int send_all(SOCKET s, const char *data, size_t len)
{
    while (len) {
        int n = send(s, data, (int)(len > INT_MAX ? INT_MAX : len), 0);
        if (n <= 0)
            return 0;
        data += n;
        len -= (size_t)n;
    }
    return 1;
}

static void send_response(SOCKET s, Response *res, int keep_alive)
{
    Buf head;

    buf_init(&head);
    buf_printf(&head, "HTTP/1.1 %d %s\r\n", res->status, status_text(res->status));
    buf_printf(&head, "Content-Type: %s\r\n", res->content_type);
    buf_printf(&head, "Content-Length: %zu\r\n", res->body.len);
    buf_printf(&head, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close");
    buf_puts(&head, "X-Content-Type-Options: nosniff\r\n");
    if (res->headers.len)
        buf_add(&head, res->headers.data, res->headers.len);
    buf_puts(&head, "\r\n");

    send_all(s, head.data, head.len);
    if (res->body.len)
        send_all(s, res->body.data, res->body.len);
    buf_free(&head);
}

/* 헤더 한 줄에서 값 부분을 꺼낸다. */
static void header_value(const char *line, char *out, size_t outsz)
{
    const char *colon = strchr(line, ':');

    if (!colon) {
        out[0] = '\0';
        return;
    }
    colon++;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    str_copy(out, outsz, colon);
    str_trim(out);
}

/* 요청 하나를 파싱한다. 0 이면 연결을 닫아야 한다. */
static int parse_request(const char *head, size_t head_len, Request *req)
{
    char line[2048];
    const char *p = head;
    const char *end = head + head_len;
    const char *sp1, *sp2, *q;
    size_t n;

    memset(req, 0, sizeof *req);

    /* 요청 라인 */
    {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            return 0;
        n = (size_t)(nl - p);
        if (n && p[n - 1] == '\r')
            n--;
        if (n >= sizeof line)
            return 0;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl + 1;
    }

    sp1 = strchr(line, ' ');
    if (!sp1)
        return 0;
    sp2 = strchr(sp1 + 1, ' ');
    if (!sp2)
        return 0;

    if ((size_t)(sp1 - line) >= sizeof req->method)
        return 0;
    memcpy(req->method, line, (size_t)(sp1 - line));

    /* 경로와 쿼리 분리 */
    n = (size_t)(sp2 - sp1 - 1);
    if (n >= sizeof req->path)
        return 0;
    memcpy(req->path, sp1 + 1, n);
    req->path[n] = '\0';

    q = strchr(req->path, '?');
    if (q) {
        str_copy(req->query, sizeof req->query, q + 1);
        req->path[q - req->path] = '\0';
    }
    url_decode(req->path);

    /* 헤더들 */
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (len && p[len - 1] == '\r')
            len--;
        if (len == 0)
            break;
        if (len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';

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

static long content_length_of(const char *head)
{
    const char *p = head;

    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (_strnicmp(p, "Content-Length:", 15) == 0) {
            char v[32];
            header_value(p, v, sizeof v);
            return atol(v);
        }
    }
    return 0;
}

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

static void handle_connection(SOCKET sock, const char *client_ip)
{
    Buf in;
    int alive = 1;

    buf_init(&in);

    while (alive) {
        size_t head_len = 0;
        char *split = NULL;
        long clen;
        Request req;
        Response res;
        size_t consumed;

        /* 헤더가 끝날 때까지 읽는다 (이전 요청의 잔여 바이트부터 검사). */
        for (;;) {
            char chunk[RECV_CHUNK];
            int n;

            if (in.len) {
                in.data[in.len] = '\0';
                split = strstr(in.data, "\r\n\r\n");
                if (split) {
                    head_len = (size_t)(split - in.data) + 4;
                    break;
                }
            }
            if (in.len > HEADER_MAX) {
                buf_free(&in);
                return;
            }
            n = recv(sock, chunk, sizeof chunk, 0);
            if (n <= 0) {
                buf_free(&in);
                return;
            }
            buf_add(&in, chunk, (size_t)n);
        }

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

        if (clen > 0) {
            req.body = (char *)malloc((size_t)clen + 1);
            if (req.body) {
                memcpy(req.body, in.data + head_len, (size_t)clen);
                req.body[clen] = '\0';
                req.body_len = (size_t)clen;
            }
        }

        res_init(&res);
        g_dispatch(&req, &res);
        send_response(sock, &res, alive);

        log_info("%s %s -> %d (%zu bytes)", req.method, req.path,
                 res.status, res.body.len);

        res_free(&res);
        free(req.body);

        /* 다음 요청을 위해 처리한 바이트를 버린다. */
        consumed = head_len + (size_t)clen;
        if (in.len > consumed) {
            memmove(in.data, in.data + consumed, in.len - consumed);
            in.len -= consumed;
            in.data[in.len] = '\0';
        } else {
            buf_reset(&in);
        }
    }

    buf_free(&in);
}

typedef struct {
    SOCKET sock;
    char   ip[48];
} ConnArg;

static DWORD WINAPI conn_thread(LPVOID arg)
{
    ConnArg *ca = (ConnArg *)arg;
    DWORD timeout = SOCK_TIMEOUT_MS;

    db_thread_begin();
    setsockopt(ca->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(ca->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);

    handle_connection(ca->sock, ca->ip);

    shutdown(ca->sock, SD_BOTH);
    closesocket(ca->sock);
    db_thread_end();
    free(ca);
    return 0;
}

int http_serve(unsigned short port, int bind_all,
               void (*dispatch)(Request *, Response *))
{
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in addr;
    int reuse = 1;

    g_dispatch = dispatch;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_err("WSAStartup 실패");
        return 0;
    }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        log_err("소켓 생성 실패 (%d)", WSAGetLastError());
        WSACleanup();
        return 0;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof reuse);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    /* 기본은 루프백만. --bind-all 을 주면 같은 네트워크의 다른 기기에서도 들어올 수 있다. */
    addr.sin_addr.s_addr = htonl(bind_all ? INADDR_ANY : INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        log_err("포트 %u 바인드 실패 (%d) - 이미 사용 중인지 확인하세요.",
                port, WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return 0;
    }
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

    for (;;) {
        struct sockaddr_in peer;
        int peerlen = sizeof peer;
        SOCKET s = accept(listener, (struct sockaddr *)&peer, &peerlen);
        ConnArg *ca;
        HANDLE th;

        if (s == INVALID_SOCKET) {
            log_warn("accept 실패 (%d)", WSAGetLastError());
            continue;
        }

        ca = (ConnArg *)calloc(1, sizeof *ca);
        if (!ca) {
            closesocket(s);
            continue;
        }
        ca->sock = s;
        inet_ntop(AF_INET, &peer.sin_addr, ca->ip, sizeof ca->ip);

        th = CreateThread(NULL, 0, conn_thread, ca, 0, NULL);
        if (!th) {
            log_warn("스레드 생성 실패");
            closesocket(s);
            free(ca);
            continue;
        }
        CloseHandle(th);
    }
}
