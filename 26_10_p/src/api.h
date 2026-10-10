/* JSON API 핸들러들이 공유하는 선언
 *
 * 서버의 REST API(/api/...)는 기능별로 여러 .c 파일에 나뉘어 있다.
 *   api.c           : 전체 디스패처, 인증(회원가입/로그인/세션), 정적 파일 제공
 *   api_posts.c     : 공모전 게시물 목록/상세, 댓글
 *   api_admin.c     : 관리자 전용 기능 (게시물 등록/삭제, 학교 공지 수집·게시, 통계)
 *   api_ai.c        : Claude API 를 이용한 게시물 분석·추천
 *   api_ml.c        : 로컬 임베딩/추천 모델 기반 기능
 *   api_community.c : 관심 키워드·프로필, 잘 맞는 회원, 팀원 모집 글·댓글·좋아요
 *   api_study.c     : 그룹 스터디 (멤버·멘토/멘티)
 * 이 헤더는 그 파일들이 서로 공유해야 하는 타입과 함수만 모아 둔 것이다.
 */
#ifndef SKU_API_H
#define SKU_API_H

#include "http.h"   /* Request / Response 타입 */
#include "json.h"   /* Json 파서/빌더 */

#define SESSION_COOKIE   "sid"   /* 세션 토큰을 담는 쿠키 이름 */
#define SESSION_HOURS    12      /* 로그인 세션 유효 시간(시간 단위) */
#define SESSION_TOKEN_LEN 48     /* 세션 토큰 16진 문자 수 (=24바이트 난수, 192비트) */

/* 로그인한 사용자. id == 0 이면 비로그인.
 * 요청이 들어올 때마다 세션 쿠키 → sessions 테이블 → users 테이블 순으로 조회해
 * 이 구조체를 채운다. 핸들러는 이 정보로 권한 검사와 개인화를 한다. */
typedef struct {
    unsigned id;                     /* users.id (0 이면 로그인하지 않은 상태) */
    char     student_no[24];         /* 학번 (로그인 아이디로 쓰인다) */
    char     name[48];               /* 실명 */
    char     nickname[48];           /* 커뮤니티에 표시되는 별명 */
    unsigned department_id;          /* 0 = 소속 없음 (관리자) */
    char     department_name[64];    /* 학과 이름 (예: "컴퓨터공학과") */
    char     college_name[64];       /* 단과대학 이름 (예: "공과대학") */
    int      is_admin;               /* 관리자면 1, 일반 학생이면 0 */
} CurrentUser;

/* 요청의 세션 쿠키로 사용자를 채운다. 비로그인이면 0 을 돌려주고 u->id = 0.
 * 로그인 여부와 상관없이 볼 수 있는 화면(예: 게시물 목록)에서 쓴다. */
int  auth_current(const Request *req, CurrentUser *u);
/* 로그인 필수. 아니면 401 응답을 채우고 0 을 돌려준다.
 * 사용 예:  if (!auth_require(req, res, &u)) return 1;  // 응답은 이미 채워짐 */
int  auth_require(const Request *req, Response *res, CurrentUser *u);
/* 관리자 필수. 아니면 401/403 응답을 채우고 0 을 돌려준다.
 *   401 = 로그인 자체를 안 함, 403 = 로그인은 했지만 관리자가 아님 */
int  auth_require_admin(const Request *req, Response *res, CurrentUser *u);

/* 요청 본문을 JSON 객체로 파싱한다. 실패 시 400 을 채우고 NULL.
 * 반환된 Json 은 호출자가 json_free 로 해제해야 한다. */
Json *body_object(const Request *req, Response *res);

/* JSON 배열에서 양의 정수들을 모아 out 에 담는다. 담은 개수를 돌려준다.
 *   arr : JS_ARR 타입 Json (아니면 0 을 돌려줌)
 *   out : 결과를 담을 배열, max : out 의 최대 칸 수
 * 음수·0·소수·숫자 아닌 원소는 건너뛴다. (예: 학과 id 목록, 태그 id 목록) */
int  json_uint_array(const Json *arr, unsigned *out, int max);

/* 하위 모듈 라우터. 처리했으면 1.
 * api_dispatch 가 아래 함수들을 차례로 불러 보고, 1 을 돌려준 곳에서 멈춘다.
 * 각 라우터는 자기 담당 경로가 아니면 응답을 건드리지 않고 0 을 돌려준다. */
int  route_auth(Request *req, Response *res);       /* 가입·로그인·/api/me·학과 목록       (api.c) */
int  route_posts(Request *req, Response *res);      /* 게시물 조회·댓글                    (api_posts.c) */
int  route_admin(Request *req, Response *res);      /* 게시물 등록/삭제·공지 수집·통계     (api_admin.c) */
int  route_ai(Request *req, Response *res);         /* /api/recommendations, /api/ai/...   (api_ai.c) */
int  route_ml(Request *req, Response *res);         /* /api/related, /api/ml/...           (api_ml.c) */
int  route_community(Request *req, Response *res);  /* 키워드·프로필·팀원 모집 게시판      (api_community.c) */
int  route_study(Request *req, Response *res);      /* 그룹 스터디·멤버·멘토/멘티         (api_study.c) */

/* 관심 키워드 이름과 자기소개를 이어 붙인 글 (추천 개인화용). 내용이 있으면 1.
 * 추천 엔진은 이 글을 "사용자를 설명하는 문장" 으로 보고 게시물과 비교한다. */
int  profile_text(unsigned user_id, Buf *out);
/* 관심 키워드(최대 8개)와 자기소개를 저장한다. 성공 시 1.
 *   tag_ids/ntags : 선택한 관심 태그 id 배열과 그 개수
 *   bio           : 자기소개 문장 (NULL 이면 빈 문자열로 취급) */
int  profile_save(unsigned user_id, const unsigned *tag_ids, int ntags, const char *bio);

/* 전체 디스패처 (main 에서 http_serve 에 넘긴다)
 * 모든 HTTP 요청이 이 함수 하나로 들어온다. /api/ 로 시작하면 위 라우터로,
 * 그렇지 않으면 webroot 의 정적 파일(index.html, app.js 등)로 보낸다. */
void api_dispatch(Request *req, Response *res);

/* 관리자 계정을 만든다 (CLI --add-admin). 성공 시 1.
 * 비밀번호는 평문으로 저장하지 않고 솔트를 붙여 해시한 값만 DB 에 넣는다. */
int  api_create_admin(const char *student_no, const char *name, const char *password);

/* 정적 파일 루트 (기본 "www") */
void api_set_webroot(const char *dir);

/* 정규 호스트 이름 (기본 "www.sku14.com").
 * www 를 뗀 이름(sku14.com)으로 들어온 요청은 이 주소로 301 넘긴다.
 * 이렇게 주소를 하나로 통일하면 쿠키(세션)가 두 도메인으로 갈라지지 않는다. */
void api_set_canonical_host(const char *host, unsigned short port);

#endif /* SKU_API_H */
