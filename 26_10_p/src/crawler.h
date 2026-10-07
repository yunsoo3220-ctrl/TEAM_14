/* 서경대학교 홈페이지 공지 수집.
 *
 * 학교 홈페이지는 WordPress 로 되어 있어 공지사항이 REST API 로 열려 있다.
 *   GET https://www.skuniv.ac.kr/wp-json/wp/v2/notice
 *       ?search=<검색어>&after=<ISO8601>&per_page=50&_fields=id,date,link,title
 * HTTPS 를 직접 처리하지 않도록 Windows 내장 WinHTTP 를 쓴다. */
#ifndef SKU_CRAWLER_H
#define SKU_CRAWLER_H

typedef struct {
    int  fetched;          /* 받아온 공지 수 */
    int  inserted;         /* 새로 넣은 수 */
    int  skipped;          /* 이미 있어 건너뛴 수 */
    int  published;        /* 게시물로 올린 수 */
    char error[256];       /* 실패 시 사유 */
} CrawlResult;

/* 검색어는 '공모전', '해커톤' 두 개를 쓰고 window_days 일 이내 공지만 담는다.
 * 성공 시 1. notices 에 upsert 한 뒤, 아직 처리되지 않은(pending) 공지를
 * 모두 게시물로 올려 로그인 없이도 목록에 보이게 한다.
 * 대상 학과는 비워 두며, 관리자가 나중에 게시물에서 체크한다.
 * app_config['crawl.fetched_at'] 도 갱신한다. */
int crawl_notices(int window_days, CrawlResult *out);

/* HTML 엔티티(&amp; &#8216; 등)를 제자리에서 디코딩한다. 공지 제목 정리용. */
void html_entity_decode(char *s);

#endif
