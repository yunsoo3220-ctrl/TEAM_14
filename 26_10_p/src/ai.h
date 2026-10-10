/* AI 게시물 분석 (Claude API, WinHTTP 로 직접 호출).
 *
 *   POST https://api.anthropic.com/v1/messages
 *
 * 게시물마다 한 번 분석해 요약·태그·주최·마감일과 학과별 관련도(0~100)를
 * post_ai / post_ai_departments 에 저장한다. 로그인한 학생에게 보여주는 추천은
 * 저장된 점수로만 계산하므로 화면을 볼 때마다 API 를 부르지 않는다.
 *
 * API 키는 환경변수 ANTHROPIC_API_KEY 로 받는다. 없으면 AI 기능만 꺼지고
 * 나머지 서버 기능은 그대로 동작한다.
 *
 * 왜 "미리 분석해 저장" 하는가?
 *   - API 호출은 느리고(수 초) 비용이 든다. 학생이 목록을 열 때마다 부르면
 *     화면이 느려지고 비용이 폭증한다.
 *   - 게시물 내용은 거의 바뀌지 않으므로 한 번 분석한 결과를 재사용해도 된다.
 */
#ifndef SKU_AI_H
#define SKU_AI_H

#include "common.h"

/* 분석에 사용할 Claude 모델 ID */
#define AI_MODEL "claude-opus-5-5"

/* 백그라운드 분석 작업의 진행 상황. 관리자 화면이 주기적으로 조회해 진행률을 보여 준다. */
typedef struct {
    int  enabled;          /* API 키가 있는지 */
    int  running;          /* 분석 작업이 돌고 있는지 */
    int  total;            /* 이번 작업에서 분석할 게시물 수 */
    int  done;             /* 성공 */
    int  failed;           /* 실패 */
    char last_error[256];  /* 마지막 실패 사유 */
    char finished_at[32];  /* 마지막 작업이 끝난 시각 */
} AiStatus;

/* ANTHROPIC_API_KEY 가 설정되어 있으면 1 */
int  ai_enabled(void);

/* 게시물 하나를 분석해 저장한다. 성공 시 1, 실패 시 0 과 err.
 *   post_id : 분석할 posts.id
 *   err     : 실패 사유를 받을 버퍼 (errsz 바이트) */
int  ai_analyze_post(unsigned post_id, char *err, size_t errsz);

/* 아직 분석하지 않은 게시물(force 면 전체)을 백그라운드 스레드에서 분석한다.
 * 시작했으면 1, 이미 돌고 있으면 0, AI 가 꺼져 있으면 -1.
 * 호출은 즉시 돌아오며, 진행 상황은 ai_status 로 확인한다. */
int  ai_start_background(int force);

/* 같은 작업을 현재 스레드에서 끝까지 돌린다 (CLI --ai-analyze 용). 실패 건수를 돌려준다. */
int  ai_run_sync(int force);

/* 현재 진행 상황을 out 에 복사한다. (스레드 안전: 내부 잠금 아래에서 복사) */
void ai_status(AiStatus *out);

#endif /* SKU_AI_H */
