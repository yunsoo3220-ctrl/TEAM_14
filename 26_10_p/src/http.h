/* 최소한의 HTTP/1.1 서버 (Winsock2).
 * 요청마다 스레드를 만들어 처리하고, 라우팅은 dispatch 콜백에 맡긴다. */
#ifndef SKU_HTTP_H
#define SKU_HTTP_H

#include "common.h"

#define HTTP_MAX_BODY  (512 * 1024)
#define HTTP_PATH_MAX  512

typedef struct {
    char   method[8];
    char   path[HTTP_PATH_MAX];      /* URL 디코딩된 경로 (쿼리 제외) */
    char   query[512];               /* 원본 쿼리 문자열 */
    char   host[128];                /* Host 헤더 (포트 포함 가능) */
    char   cookie[1024];
    char   content_type[128];
    char   client_ip[48];
    char  *body;                     /* 본문 (NUL 종료, 없으면 NULL) */
    size_t body_len;
} Request;

typedef struct {
    int  status;
    char content_type[80];
    Buf  headers;                    /* "Name: value\r\n" 누적 */
    Buf  body;
} Response;

/* 요청 도우미 */
int  req_query(const Request *req, const char *name, char *out, size_t outsz);
int  req_cookie(const Request *req, const char *name, char *out, size_t outsz);
int  req_is(const Request *req, const char *method, const char *path);

/* 경로가 prefix 로 시작하고 그 뒤가 양의 정수이면 1 과 그 값을 돌려준다.
 * 예: path_id("/api/posts/12", "/api/posts/", &id) -> 1, id = 12
 * suffix 가 NULL 이 아니면 숫자 뒤가 그 문자열과 정확히 일치해야 한다.
 * 예: path_id("/api/posts/12/comments", "/api/posts/", "/comments", &id) */
int  path_id(const char *path, const char *prefix, const char *suffix, unsigned *id);

/* 응답 도우미 */
void res_init(Response *res);
void res_free(Response *res);
void res_header(Response *res, const char *name, const char *value);
void res_set_cookie(Response *res, const char *name, const char *value, long max_age);
void res_json(Response *res, int status, const char *json);
void res_json_buf(Response *res, int status, Buf *body);     /* body 소유권 이전 */
void res_error(Response *res, int status, const char *code, const char *message);
void res_text(Response *res, int status, const char *text);
void res_redirect(Response *res, int status, const char *location);

/* Host 헤더에서 포트를 떼어 호스트 이름만 out 에 쓴다. */
void host_name_only(const char *host_header, char *out, size_t outsz);

/* 서버 실행 (블로킹). 0 이면 시작 실패.
 * bind_all 이 0 이면 127.0.0.1 에만, 1 이면 모든 인터페이스에 바인드한다. */
int  http_serve(unsigned short port, int bind_all,
                void (*dispatch)(Request *, Response *));

/* URL 디코딩 (제자리, + 는 공백으로) */
void url_decode(char *s);

#endif
