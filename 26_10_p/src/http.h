/* 최소한의 HTTP/1.1 서버 (Winsock2).
 * 요청마다 스레드를 만들어 처리하고, 라우팅은 dispatch 콜백에 맡긴다.
 *
 * 동작 흐름:
 *   1) http_serve 가 포트를 열고 accept() 로 접속을 기다린다.
 *   2) 접속(TCP 연결)이 오면 새 스레드를 만들고, 그 스레드가 요청을 읽어 Request 를 채운다.
 *   3) dispatch(req, res) 콜백을 불러 Response 를 채우게 한다.
 *   4) Response 를 HTTP 형식으로 직렬화해 보낸다.
 *   5) Keep-Alive: 클라이언트가 "Connection: close" 를 보내지 않았으면 같은 연결에서
 *      다음 요청을 계속 받는다(2~4 반복). 연결이 끊기거나 15초 동안 조용하면 스레드가 끝난다.
 */
#ifndef SKU_HTTP_H
#define SKU_HTTP_H

#include "common.h"

#define HTTP_MAX_BODY  (512 * 1024)   /* 요청 본문 최대 크기(512KB). 넘으면 413 으로 거절 */
#define HTTP_PATH_MAX  512            /* 요청 경로 최대 길이 */

/* 파싱된 HTTP 요청 한 건 */
typedef struct {
    char   method[8];                /* "GET", "POST", "PUT", "DELETE" 등 */
    char   path[HTTP_PATH_MAX];      /* URL 디코딩된 경로 (쿼리 제외) */
    char   query[512];               /* 원본 쿼리 문자열 */
    char   host[128];                /* Host 헤더 (포트 포함 가능) */
    char   cookie[1024];             /* Cookie 헤더 원문 ("a=1; b=2" 형태) */
    char   content_type[128];        /* Content-Type 헤더 (예: application/json) */
    char   client_ip[48];            /* 접속한 클라이언트의 IP 주소 문자열 */
    char  *body;                     /* 본문 (NUL 종료, 없으면 NULL) */
    size_t body_len;                 /* 본문 바이트 수 */
} Request;

/* 핸들러가 채워 넣는 HTTP 응답 한 건 */
typedef struct {
    int  status;                     /* HTTP 상태 코드 (200, 404 등) */
    char content_type[80];           /* Content-Type 헤더 값 */
    Buf  headers;                    /* "Name: value\r\n" 누적 */
    Buf  body;                       /* 응답 본문 */
} Response;

/* 요청 도우미 */
/* 쿼리 문자열에서 name 의 값을 찾아 URL 디코딩 후 out 에 쓴다. 있으면 1, 없으면 0.
 * 예: query="page=2&q=AI" 에서 req_query(req,"q",...) → out="AI" */
int  req_query(const Request *req, const char *name, char *out, size_t outsz);
/* Cookie 헤더에서 name 쿠키 값을 찾아 out 에 쓴다. 있으면 1. */
int  req_cookie(const Request *req, const char *name, char *out, size_t outsz);
/* 메서드와 경로가 둘 다 정확히 일치하면 1. 예: req_is(req, "POST", "/api/auth/login") */
int  req_is(const Request *req, const char *method, const char *path);

/* 경로가 prefix 로 시작하고 그 뒤가 양의 정수이면 1 과 그 값을 돌려준다.
 * 예: path_id("/api/posts/12", "/api/posts/", &id) -> 1, id = 12
 * suffix 가 NULL 이 아니면 숫자 뒤가 그 문자열과 정확히 일치해야 한다.
 * 예: path_id("/api/posts/12/comments", "/api/posts/", "/comments", &id) */
int  path_id(const char *path, const char *prefix, const char *suffix, unsigned *id);

/* 응답 도우미 */
void res_init(Response *res);    /* 기본값(200, 빈 본문)으로 초기화 */
void res_free(Response *res);    /* headers/body 버퍼 해제 */
/* 임의의 응답 헤더 한 줄을 추가한다.
 * 주의: 값을 검사하지 않으므로 사용자 입력(줄바꿈 포함 가능)을 그대로 넣으면 안 된다. */
void res_header(Response *res, const char *name, const char *value);
/* Set-Cookie 헤더를 추가한다. max_age 초 뒤 만료 (0 이면 즉시 삭제).
 * HttpOnly, SameSite 속성을 붙여 자바스크립트 탈취와 CSRF 를 줄인다. */
void res_set_cookie(Response *res, const char *name, const char *value, long max_age);
/* 상태 코드와 JSON 문자열로 응답을 채운다. (json 은 복사된다) */
void res_json(Response *res, int status, const char *json);
void res_json_buf(Response *res, int status, Buf *body);     /* body 소유권 이전 */
/* {"error":{"code":..., "message":...}} 형태의 표준 오류 응답을 만든다.
 *   code    : 프론트엔드가 분기에 쓰는 기계용 코드 (예: "not_found", 이스케이프 없이 들어가므로 상수만)
 *   message : 사용자에게 보여 줄 한국어 설명 */
void res_error(Response *res, int status, const char *code, const char *message);
/* text/plain 응답 */
void res_text(Response *res, int status, const char *text);
/* Location 헤더를 넣은 리다이렉트 응답 (301 영구, 302 임시 등) */
void res_redirect(Response *res, int status, const char *location);

/* Host 헤더에서 포트를 떼어 호스트 이름만 out 에 쓴다.
 * 예: "www.sku14.com:8080" → "www.sku14.com"  (첫 ':' 에서 자르므로 IPv6 주소는 고려하지 않음) */
void host_name_only(const char *host_header, char *out, size_t outsz);

/* 서버 실행 (블로킹). 0 이면 시작 실패.
 * bind_all 이 0 이면 127.0.0.1 에만, 1 이면 모든 인터페이스에 바인드한다.
 *   127.0.0.1 전용: 이 PC 에서만 접속 가능 (개발/시연용으로 안전)
 *   모든 인터페이스: 같은 네트워크의 다른 기기에서도 접속 가능 */
int  http_serve(unsigned short port, int bind_all,
                void (*dispatch)(Request *, Response *));

/* URL 디코딩 (제자리, + 는 공백으로)
 * "%EC%95%88" 같은 퍼센트 인코딩을 원래 바이트로 되돌린다. 결과는 원본보다
 * 항상 짧거나 같으므로 같은 메모리에 덮어써도 안전하다. */
void url_decode(char *s);

#endif /* SKU_HTTP_H */
