/* 서경대학교 홈페이지 공지 수집.
 *
 * 학교 홈페이지는 WordPress 로 되어 있어 공지사항이 REST API 로 열려 있다.
 *   GET https://www.skuniv.ac.kr/wp-json/wp/v2/notice
 *       ?search=<검색어>&after=<ISO8601>&per_page=50&_fields=id,date,link,title
 * HTTPS 를 직접 처리하지 않도록 Windows 내장 WinHTTP 를 쓴다. */
#ifndef SKU_CRAWLER_H
#define SKU_CRAWLER_H

#include "common.h"

typedef struct {
    int  fetched;          /* 받아온 공지 수 */
    int  inserted;         /* 새로 넣은 수 */
    int  skipped;          /* 이미 있어 건너뛴 수 */
    int  published;        /* 게시물로 올린 수 */
    char error[256];       /* 실패 시 사유 */
} CrawlResult;

/* 검색어 여러 개(공모전·해커톤·경진대회·공모·대회·챌린지·아이디어·콘테스트)로 window_days 일
 * 이내 공지를 페이지를 넘겨 가며 모두 받고, 제목으로 참가할 수 있는 공모전·대회만 남긴다
 * (수상자 발표·장학금·서포터즈 등 제외).
 * 성공 시 1. notices 에 upsert 한 뒤, 아직 처리되지 않은(pending) 공지를
 * 모두 게시물로 올려 로그인 없이도 목록에 보이게 한다.
 * 대상 학과는 비워 두며, 관리자가 나중에 게시물에서 체크한다.
 * app_config['crawl.fetched_at'] 도 갱신한다. */
int crawl_notices(int window_days, CrawlResult *out);

/* HTML 엔티티(&amp; &#8216; 등)를 제자리에서 디코딩한다. 공지 제목 정리용. */
void html_entity_decode(char *s);

/* 공지(ext_id) 원문을 받아 게시판 본문용 텍스트로 바꿔 out 끝에 붙인다.
 *   GET /wp-json/wp/v2/notice/{ext_id}?_fields=content
 * 문단은 줄로, 목록은 "• ", 표는 " | " 로 잇고 이미지와 링크 주소는 뺀다.
 * 붙인 글이 있으면 1, 받지 못했거나 비었으면 0 (out 은 그대로). */
int notice_body_text(long ext_id, Buf *out);

#endif
