/* 딥러닝 문장 임베딩 클라이언트.
 *
 * ml/embed_server.py (사전학습 트랜스포머) 를 127.0.0.1:SKU_EMBED_PORT (기본 8001) 에서
 * 부른다. 서비스가 없으면 0 을 돌려주고, 호출하는 쪽(ml.c)은 TF-IDF 만으로 동작한다. */
#ifndef SKU_EMBED_H
#define SKU_EMBED_H

#include <stddef.h>

/* texts[n] 을 임베딩한다. kind 는 "passage"(게시물) 또는 "query"(학과·관심사).
 * 성공하면 차원 수를 돌려주고 *out 에 n x dim 개의 float (L2 정규화됨, 호출자가 free).
 * model 에 모델 이름을 적는다 (NULL 이어도 된다). 실패하면 0. */
int embed_texts(const char *kind, const char *const *texts, int n, float **out,
                char *model, size_t modelsz);

/* 서비스가 떠 있는지. 30초 동안 결과를 기억한다. */
int embed_available(void);

#endif
