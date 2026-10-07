/* JSON API 핸들러들이 공유하는 선언 */
#ifndef SKU_API_H
#define SKU_API_H

#include "http.h"
#include "json.h"

#define SESSION_COOKIE   "sid"
#define SESSION_HOURS    12
#define SESSION_TOKEN_LEN 48

/* 로그인한 사용자. id == 0 이면 비로그인. */
typedef struct {
    unsigned id;
    char     student_no[24];
    char     name[48];
    char     nickname[48];
    unsigned department_id;          /* 0 = 소속 없음 (관리자) */
    char     department_name[64];
    char     college_name[64];
    int      is_admin;
} CurrentUser;

/* 요청의 세션 쿠키로 사용자를 채운다. 비로그인이면 0 을 돌려주고 u->id = 0. */
int  auth_current(const Request *req, CurrentUser *u);
/* 로그인 필수. 아니면 401 응답을 채우고 0 을 돌려준다. */
int  auth_require(const Request *req, Response *res, CurrentUser *u);
/* 관리자 필수. 아니면 401/403 응답을 채우고 0 을 돌려준다. */
int  auth_require_admin(const Request *req, Response *res, CurrentUser *u);

/* 요청 본문을 JSON 객체로 파싱한다. 실패 시 400 을 채우고 NULL. */
Json *body_object(const Request *req, Response *res);

/* JSON 배열에서 양의 정수들을 모아 out 에 담는다. 담은 개수를 돌려준다. */
int  json_uint_array(const Json *arr, unsigned *out, int max);

/* 하위 모듈 라우터. 처리했으면 1. */
int  route_auth(Request *req, Response *res);
int  route_posts(Request *req, Response *res);
int  route_admin(Request *req, Response *res);

/* 전체 디스패처 (main 에서 http_serve 에 넘긴다) */
void api_dispatch(Request *req, Response *res);

/* 관리자 계정을 만든다 (CLI --add-admin). 성공 시 1. */
int  api_create_admin(const char *student_no, const char *name, const char *password);

/* 정적 파일 루트 (기본 "www") */
void api_set_webroot(const char *dir);

/* 정규 호스트 이름 (기본 "www.sku14.com").
 * www 를 뗀 이름(sku14.com)으로 들어온 요청은 이 주소로 301 넘긴다. */
void api_set_canonical_host(const char *host, unsigned short port);

#endif
