/* 자체 추천 모델 - 외부 API 없이 서버 안에서 학습하고 추론한다.
 *
 * 1. 토큰화: 한국어는 형태소 분석기 없이 낱말 + 한글 두 글자 묶음(bigram)으로,
 *    영문·숫자는 소문자 낱말로 나눈다. 흔한 공지 상투어는 불용어로 뺀다.
 * 2. 문서 벡터: 게시물(제목 가중 2배 + 본문)과 학과 프로필을 TF-IDF 로 만든다.
 * 3. 학습(Rocchio): 학과 벡터 = 프로필 벡터 + 0.75 x (그 학과에 연결된 게시물 벡터 평균).
 *    연결 = 관리자가 대상 학과로 체크한 글(가중 1.0), 그 학과 학생이 댓글을 단 글(0.5).
 * 4. 추론: 게시물-학과 코사인 유사도. 점수(0~100)는 데이터에 맞춰 보정한다
 *    (게시물별 최고 유사도의 90% 지점이 80점이 되도록).
 * 5. 설명: 두 벡터에서 기여가 큰 낱말을 근거 키워드로 돌려준다.
 * 6. 딥러닝 임베딩 (embed.h): 임베딩 서비스가 떠 있으면 사전학습 트랜스포머로 만든 문장
 *    벡터에도 같은 Rocchio 를 적용하고, 유사도 = 0.7 x 임베딩 + 0.3 x TF-IDF 로 합친다.
 *    글자가 겹치지 않아도 뜻이 가까우면 찾는다. 서비스가 없으면 TF-IDF 만 쓴다.
 *
 * 7. 확률 (Platt scaling): P(학과에 맞음 | 유사도 s) = 1 / (1 + e^-(a s + b)).
 *    a, b 는 정답 쌍(관리자 체크·댓글·♥)을 양성, 그 글의 나머지 학과를 음성으로 두고
 *    로지스틱 회귀로 맞춘다. 정답이 적을 때 흔들리지 않게 데이터 분포에서 정한 사전값
 *    (평범한 쌍 5%, 상위 10% 쌍 70%) 쪽으로 당긴다 (MAP 추정). 정답이 쌓일수록 데이터가 이긴다.
 *
 * 게시물·라벨·프로필이 바뀌거나 임베딩 서비스가 새로 뜨면 다음 요청 때 자동으로 다시 학습한다.
 * ml_predict_departments 는 다시 학습하지 않고 지금 모델로 아직 저장하지 않은 글을 바로 매긴다. */
#ifndef SKU_ML_H
#define SKU_ML_H

#define ML_MAX_TERMS 4

typedef struct {
    unsigned id;                         /* 게시물 또는 학과 번호 */
    int      score;                      /* 0 ~ 100 */
    double   prob;                       /* 게시물-학과: 맞을 확률 0 ~ 1 (그 밖에는 0) */
    int      nterms;
    char     terms[ML_MAX_TERMS][40];    /* 근거 키워드 */
} MlHit;

typedef struct {
    int    posts;          /* 학습에 쓴 게시물 수 */
    int    departments;
    int    labels;         /* (학과, 게시물) 연결 수 */
    double tau;            /* 점수 보정값 */
    char   trained_at[32];
    int    dim;            /* 임베딩 차원 (0 = 임베딩 없이 TF-IDF 만) */
    char   embed_model[128];
    double pa, pb;         /* 확률 보정: P = 1 / (1 + e^-(pa * s + pb)) */
    int    positives;      /* 확률 보정에 쓴 양성 쌍 수 */
    int    negatives;      /* 음성 쌍 수 */
} MlInfo;

/* 학과와 관련 있는 게시물을 점수 순으로 담는다. min_score 미만은 뺀다.
 * 담은 개수, 모델을 만들 수 없으면 -1. info 는 NULL 이어도 된다. */
int ml_rank_posts(unsigned department_id, int min_score, MlHit *out, int max, MlInfo *info);

/* 게시물과 관련 있는 학과를 점수 순으로 담는다. */
int ml_rank_departments(unsigned post_id, int min_score, MlHit *out, int max, MlInfo *info);

/* 아직 저장하지 않은 글(제목·본문)이 학과마다 맞을 확률을 지금 모델로 계산한다 (재학습 없음).
 * 확률 높은 순으로 max 개까지 담고 개수를 돌려준다. 모델을 만들 수 없으면 -1. */
int ml_predict_departments(const char *title, const char *body, MlHit *out, int max, MlInfo *info);

/* 강제로 다시 학습한다. 성공 시 1. */
int ml_retrain(MlInfo *info);

/* 개인 맞춤: 학과 벡터에 내 관심사(키워드·자기소개 글) 벡터를 더해 순위를 매긴다.
 * profile_text 가 비면 ml_rank_posts 와 같다. */
int ml_rank_posts_personal(unsigned department_id, const char *profile_text, int min_score,
                           MlHit *out, int max, MlInfo *info);

/* 관심 키워드·자기소개가 비슷한 다른 학생을 점수 순으로. 근거는 겹친 키워드·낱말. */
int ml_match_users(unsigned user_id, int min_score, MlHit *out, int max);

/* 모집글마다 나와 맞는 정도를 매긴다 (out[i].id = recruit_ids[i]).
 * 내가 모델에 없으면(관리자 등) 0 을 돌려주고 점수는 모두 0. */
int ml_score_recruits(unsigned user_id, const unsigned *recruit_ids, int n, MlHit *out);

/* "terms":["영상","콘텐츠"] 를 b 에 쓴다 (api_ml.c). */
#include "common.h"
void ml_write_terms(Buf *b, const MlHit *h);

#endif
