/* 기간이 지난 글 자동 정리.
 *
 * 지우는 것 (날짜는 "그날이 지나면" - 마감일 당일까지는 남는다):
 *   posts         마감일(deadline)이 지난 공모전.
 *                 마감일을 모르는 학교 공지(작성자 없음)는 게시일로부터 cleanup.no_deadline_days 일(기본 60일) 뒤.
 *                 댓글·대상 학과·AI 분석은 함께 지워지고(ON DELETE CASCADE), 연결된 팀원 모집글은 남는다.
 *   recruits      마감(closed)했거나 마감일이 지난 팀원 모집글 (댓글·태그·좋아요도 함께)
 *   study_groups  종료일(end_date)이 지난 스터디 (멤버 목록도 함께)
 *   notices       수집 기간보다 오래된 공지 기록 (게시물이 없는 것만)
 *
 * 지운 공모전의 공지 기록(notices)은 남겨 두므로 다음 수집 때 다시 올라오지 않는다.
 */
#ifndef SKU_CLEANUP_H
#define SKU_CLEANUP_H

/* 서버가 기대하는 열이 없으면 추가한다 (예전 DB 를 쓰던 경우). 서버 시작 시 한 번. */
void cleanup_ensure_schema(void);

/* 마감일이 비어 있는 학교 공지 게시물의 본문에서 마감일을 찾아 채운다.
 * 마감일 추출 기능이 생기기 전에 수집한 게시물용. 채운 건수를 돌려준다. */
int cleanup_backfill_deadlines(void);

/* 위 규칙대로 한 번 지운다. 지운 게시물·모집글·스터디 수의 합을 돌려준다 (실패 시 -1). */
int cleanup_expired(void);

/* 백그라운드 스레드에서 지금 한 번, 이후 1시간마다 cleanup_expired 를 돌린다. 웹 서버 모드에서 호출. */
void cleanup_start(void);

#endif /* SKU_CLEANUP_H */
