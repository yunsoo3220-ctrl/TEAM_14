/* 딥러닝 문장 임베딩 클라이언트.
 *
 * ml/embed_server.py (사전학습 트랜스포머) 를 127.0.0.1:SKU_EMBED_PORT (기본 8001) 에서
 * 부른다. 서비스가 없으면 0 을 돌려주고, 호출하는 쪽(ml.c)은 TF-IDF 만으로 동작한다.
 *
 * "임베딩" 이란 문장을 수백 차원의 실수 벡터로 바꾼 것이다. 뜻이 비슷한 문장일수록
 * 벡터 방향이 비슷해지므로, 두 벡터의 내적(코사인 유사도)으로 의미의 가까움을 잴 수 있다.
 * 예: "영상 편집 공모전" 과 "유튜브 콘텐츠 대회" 는 겹치는 낱말이 없어도 가깝게 나온다.
 *
 * 무거운 신경망은 Python(PyTorch) 쪽에서 돌리고, C 서버는 HTTP 로 결과만 받는다.
 */
#ifndef SKU_EMBED_H
#define SKU_EMBED_H

#include <stddef.h>

/* texts[n] 을 임베딩한다. kind 는 "passage"(게시물) 또는 "query"(학과·관심사).
 * 성공하면 차원 수를 돌려주고 *out 에 n x dim 개의 float (L2 정규화됨, 호출자가 free).
 * model 에 모델 이름을 적는다 (NULL 이어도 된다). 실패하면 0.
 *   - passage/query 구분: E5 계열 모델은 문서와 검색어에 서로 다른 접두어를 붙여야
 *     성능이 좋아서, 서버 쪽에서 kind 에 맞는 접두어를 붙인다.
 *   - L2 정규화: 벡터 길이를 1 로 맞춰 두었기 때문에 내적이 곧 코사인 유사도가 된다.
 *   - 결과 배열 배치: i 번째 문장의 벡터는 (*out)[i*dim] ~ (*out)[i*dim + dim-1] */
int embed_texts(const char *kind, const char *const *texts, int n, float **out,
                char *model, size_t modelsz);

/* 서비스가 떠 있는지. 30초 동안 결과를 기억한다.
 * (매 요청마다 접속을 시도하면 서비스가 꺼져 있을 때 매번 연결 시간초과를 기다리게 되므로) */
int embed_available(void);

#endif /* SKU_EMBED_H */
