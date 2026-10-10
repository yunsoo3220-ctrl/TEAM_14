/* 자체 추천 모델 (딥러닝 문장 임베딩 + TF-IDF + Rocchio). 설명은 ml.h 참고.
 *
 * 파일 구성:
 *   1) 밀집 벡터(임베딩) 도우미       dense_*, hybrid, embed_all
 *   2) 희소 벡터(TF-IDF) 자료구조     Vec, Term, vec_*
 *   3) 토큰화                          tokenize, flush_word (+ 불용어/조사/동의어 표)
 *   4) 모델 구조체와 확률 보정         Model, fit_calibration
 *   5) 학습                            train (게시물·학과 벡터, Rocchio, 점수 보정)
 *   6) 공개 함수                       ml_rank_*, ml_predict_*, ml_match_users, ml_score_recruits
 *
 * 모델(g_model)은 전역 하나이고, g_lock 으로 보호한다. 요청이 오면 "학습 데이터 서명" 을
 * 계산해 지난 학습 때와 다르면 다시 학습하고, 같으면 저장된 모델을 그대로 쓴다.
 */
#include "ml.h"
#include "common.h"
#include "db.h"
#include "embed.h"

#include <windows.h>
#include <math.h>      /* sqrt, log, exp, fabs */
#include <stdint.h>    /* uint32_t - 해시 키 */
#include <stdio.h>
#include <stdlib.h>    /* qsort, bsearch */
#include <string.h>
#include <time.h>

#define TITLE_WEIGHT   2.0f    /* 제목 낱말은 본문 낱말보다 2배 중요하게 */
#define BIGRAM_WEIGHT  0.6f    /* 두 글자 묶음은 온전한 낱말보다 덜 중요하게 */
#define ROCCHIO_ALPHA  1.0f    /* Rocchio: 원래 학과 프로필 벡터의 비중 */
#define ROCCHIO_BETA   0.75f   /* Rocchio: 정답 게시물 평균 벡터의 비중 */
#define CENTROID_TOP   40      /* 학습한 게시물 특징은 가중치 상위 이만큼만 쓴다 (잡음 억제) */
#define SHRINK_K       1.5f    /* 학습 데이터가 적을수록 반영을 줄인다: beta * w / (w + k) */
#define TERM_TEXT_MAX  40      /* 근거 키워드로 보여 줄 낱말의 최대 바이트 */
#define EMB_WEIGHT     0.7     /* 최종 유사도 = 0.7 x 임베딩 + 0.3 x TF-IDF (임베딩이 있을 때) */

/* ============================================================ 밀집 벡터 (임베딩) */

/* 밀집 벡터를 길이 1 로 정규화한다 (L2 정규화). 영벡터는 그대로 둔다.
 * 길이가 1 이면 두 벡터의 내적이 곧 코사인 유사도가 된다. */
static void dense_normalize(float *v, int dim)
{
    double s = 0;   /* 제곱합 (정밀도를 위해 double 로 누적) */
    int i;

    for (i = 0; i < dim; i++)
        s += (double)v[i] * v[i];
    if (s <= 0)
        return;
    s = sqrt(s);
    for (i = 0; i < dim; i++)
        v[i] = (float)(v[i] / s);
}

/* 두 밀집 벡터의 내적 = Σ a[i]·b[i] */
static double dense_dot(const float *a, const float *b, int dim)
{
    double s = 0;
    int i;

    for (i = 0; i < dim; i++)
        s += (double)a[i] * b[i];
    return s;
}

/* 트랜스포머 임베딩의 코사인은 관계없는 글끼리도 0.7~0.8 쯤 나온다.
 * 전체 평균(mu) 아래는 0, 위는 0~1 로 펴서 TF-IDF 코사인과 같은 척도로 맞춘다.
 *   e = (emb - mu) / (1 - mu)   (emb > mu 일 때), 아니면 0
 *   결과 = 0.3 x tfidf + 0.7 x e
 * 임베딩이 없으면(dim <= 0) TF-IDF 값을 그대로 쓴다. */
static double hybrid(double tfidf, double emb, double mu, int dim)
{
    double e;

    if (dim <= 0)
        return tfidf;
    e = emb > mu && mu < 1 ? (emb - mu) / (1 - mu) : 0;
    return (1 - EMB_WEIGHT) * tfidf + EMB_WEIGHT * e;
}

/* 글 n 개를 임베딩한다. 서비스가 없으면 NULL, *dim = 0. */
static float *embed_all(const char *kind, char **texts, int n, int *dim, char *model, size_t modelsz)
{
    float *v = NULL;

    *dim = n > 0 ? embed_texts(kind, (const char *const *)texts, n, &v, model, modelsz) : 0;
    if (*dim <= 0) {
        *dim = 0;
        free(v);
        return NULL;
    }
    return v;
}

/* 문자열 포인터 배열과 그 안의 문자열들을 모두 해제한다 */
static void free_texts(char **texts, int n)
{
    int i;

    if (!texts)
        return;
    for (i = 0; i < n; i++)
        free(texts[i]);
    free(texts);
}

/* a + "\n" + b + "\n" + c 를 새로 만든다 (NULL 은 빈 문자열).
 * Buf 의 data 를 그대로 돌려주므로 호출자가 free 한다 (buf_free 대신). */
static char *join3(const char *a, const char *b, const char *c)
{
    Buf t;

    buf_init(&t);
    buf_puts(&t, a ? a : "");
    buf_putc(&t, '\n');
    buf_puts(&t, b ? b : "");
    buf_putc(&t, '\n');
    buf_puts(&t, c ? c : "");
    return t.data;
}

/* ============================================================ 희소 벡터 */

/* 희소 벡터의 원소 하나 = (낱말 해시, 가중치) + 설명용 원문
 * 수만 가지 낱말 중 문서에 실제로 나온 것만 저장하므로 "희소(sparse)" 벡터다. */
typedef struct {
    uint32_t key;                  /* 낱말의 32비트 해시 (비교·정렬 기준) */
    float    w;                    /* 가중치 (처음엔 출현 횟수, 나중엔 TF-IDF) */
    char     kind;                 /* 'w' 낱말, 'b' 두 글자 묶음 */
    char     text[TERM_TEXT_MAX];  /* 원래 낱말 (근거 키워드 표시용) */
} Term;

/* 동적 배열로 된 희소 벡터. 연산 전에는 key 순으로 정렬되어 있어야 한다 (vec_compact). */
typedef struct {
    Term *t;
    int   n, cap;   /* 원소 수, 할당 용량 */
} Vec;

static void vec_free(Vec *v)
{
    free(v->t);
    v->t = NULL;
    v->n = v->cap = 0;
}

/* 원소 하나를 끝에 추가 (정렬은 나중에 vec_compact 로). 메모리 부족이면 0. */
static int vec_push(Vec *v, uint32_t key, float w, char kind, const char *text)
{
    if (v->n == v->cap) {                    /* 꽉 찼으면 2배로 늘린다 */
        int cap = v->cap ? v->cap * 2 : 64;
        Term *t = (Term *)realloc(v->t, (size_t)cap * sizeof *t);
        if (!t)
            return 0;
        v->t = t;
        v->cap = cap;
    }
    v->t[v->n].key = key;
    v->t[v->n].w = w;
    v->t[v->n].kind = kind;
    str_copy(v->t[v->n].text, sizeof v->t[v->n].text, text);
    v->n++;
    return 1;
}

/* qsort 비교: key 오름차순. (x > y) 는 참이면 1, 거짓이면 0 → 결과는 -1/0/1 */
static int term_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Term *)a)->key, y = ((const Term *)b)->key;
    return x < y ? -1 : x > y;
}

/* 키 순으로 정렬하고 같은 키를 합친다.
 * 예: [(영상,1),(AI,1),(영상,1)] → [(AI,1),(영상,2)]  (= 낱말별 출현 횟수 세기) */
static void vec_compact(Vec *v)
{
    int i, k = 0;   /* k: 합쳐진 결과의 마지막 위치 */

    if (v->n == 0)
        return;
    qsort(v->t, (size_t)v->n, sizeof *v->t, term_cmp);
    for (i = 1; i < v->n; i++) {
        if (v->t[i].key == v->t[k].key)
            v->t[k].w += v->t[i].w;      /* 같은 낱말: 가중치를 더한다 */
        else
            v->t[++k] = v->t[i];         /* 새 낱말: 다음 칸으로 */
    }
    v->n = k + 1;
}

/* 희소 벡터를 길이 1 로 정규화 */
static void vec_normalize(Vec *v)
{
    double s = 0;
    int i;

    for (i = 0; i < v->n; i++)
        s += (double)v->t[i].w * v->t[i].w;
    if (s <= 0)
        return;
    s = sqrt(s);
    for (i = 0; i < v->n; i++)
        v->t[i].w = (float)(v->t[i].w / s);
}

/* out += scale * v  (둘 다 정렬된 상태를 유지한다)
 * v 의 원소를 모두 out 끝에 붙인 뒤 vec_compact 로 정렬·합치기. */
static void vec_add_scaled(Vec *out, const Vec *v, float scale)
{
    int i;

    for (i = 0; i < v->n; i++)
        vec_push(out, v->t[i].key, v->t[i].w * scale, v->t[i].kind, v->t[i].text);
    vec_compact(out);
}

/* qsort 비교: 가중치 내림차순 */
static int weight_desc(const void *a, const void *b)
{
    float x = ((const Term *)a)->w, y = ((const Term *)b)->w;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* 가중치가 큰 k 개만 남긴다 (키 순 정렬은 유지).
 * 가중치순 정렬 → 앞 k 개만 남김 → 다시 키순 정렬 */
static void vec_keep_top(Vec *v, int k)
{
    if (v->n <= k)
        return;
    qsort(v->t, (size_t)v->n, sizeof *v->t, weight_desc);
    v->n = k;
    qsort(v->t, (size_t)v->n, sizeof *v->t, term_cmp);
}

/* 두 희소 벡터의 내적. 둘 다 key 순으로 정렬되어 있으므로
 * 병합 정렬의 합치기 단계처럼 두 포인터를 앞으로만 움직이며 같은 key 를 찾는다.
 * 시간 복잡도 O(n + m) - 모든 쌍을 비교하는 O(n x m) 보다 훨씬 빠르다. */
static double vec_dot(const Vec *a, const Vec *b)
{
    int i = 0, j = 0;
    double s = 0;

    while (i < a->n && j < b->n) {
        if (a->t[i].key == b->t[j].key)
            s += (double)a->t[i++].w * b->t[j++].w;   /* 공통 낱말: 곱해서 더함 */
        else if (a->t[i].key < b->t[j].key)
            i++;                                      /* 작은 쪽을 앞으로 */
        else
            j++;
    }
    return s;
}

/* ============================================================ 토큰화 */

/* FNV-1a 32비트 해시. prefix("w:" 또는 "b:")와 낱말을 이어서 해시한다.
 * prefix 를 섞으면 낱말 "공모" 와 두 글자 묶음 "공모" 가 서로 다른 키가 된다.
 * FNV-1a: 바이트마다 XOR 한 뒤 소수 16777619 를 곱한다 - 빠르고 분포가 고르다. */
static uint32_t fnv1a(const char *prefix, const char *s)
{
    uint32_t h = 2166136261u;   /* FNV 오프셋 기저값 (표준 상수) */

    for (; *prefix; prefix++) { h ^= (unsigned char)*prefix; h *= 16777619u; }
    for (; *s; s++)           { h ^= (unsigned char)*s;      h *= 16777619u; }
    return h;
}

/* 공지에 거의 늘 나오는 낱말. 학과를 가르는 데 도움이 되지 않는다.
 * (불용어 stop word: "안내", "신청" 처럼 어느 글에나 있어서 구분력이 없는 낱말) */
static const char *STOP_WORDS[] = {
    "공모전", "안내", "모집", "참가", "참여", "학생", "학생들", "대학", "대학교", "서경대학교",
    "학년도", "개최", "신청", "접수", "기간", "붙임", "주최", "주관", "관심", "바랍니다",
    "있는", "위해", "대한", "통해", "관련", "내용", "일정", "방법", "대상", "제출",
    "홈페이지", "문의", "공고", "공고문", "포스터", "참고", "아래", "해당", "이상", "이내",
    "또는", "그리고", "있습니다", "합니다", "드립니다", "바랍니다", "가능", "누구나", "전국민",
    "개요", "시상", "상금", "수상", "대상", "최우수상", "우수상", "장려상", "명의", "상장",
    "양식", "참조", "아래와", "같이", "부문", "규모", "시상규모", "참가대상", "참가자격", "우리",
    "공모명", "공모기간", "공모대상", "공모주제", "공모형식", "접수기간", "접수방법", "선정발표",
    "다운로드", "작성", "작성하여", "이메일", "메일", "제출처", "확인", "기타", "사항", "세부",
    "공모전에", "공모전을", "공모전이", "팀을", "팀원", "만들어", "봤습니다", "했습니다", "입니다",
    "학년입니다", "같이", "나갈", "찾고", "있어요", "있고", "적이", "합니다", "해요", "구해요",
    "함께", "하실", "분", "환영합니다", "환영해요", "찾습니다", "모집합니다",
    "the", "and", "of", "to", "in", "for", "kr", "www", "http", "https", "com", "co", "or", NULL
};
/* 두 글자 묶음용 불용어 ("니다", "습니" 같은 어미 조각 등) */
static const char *STOP_BIGRAMS[] = {
    "공모", "모전", "안내", "모집", "참가", "참여", "학생", "대학", "학교", "신청", "접수",
    "기간", "붙임", "주최", "주관", "관심", "바랍", "랍니", "니다", "합니", "습니", "있는",
    "위해", "하는", "대한", "년도", "학년", "개최", "내용", "일정", "방법", "대상", "제출",
    "까지", "하여", "으로", "에서", "하고", "해주", "주시", "시길", "드립", "립니", "됩니",
    "입니", "있습", "홈페", "페이", "이지", "문의", "공고", "서경", "경대", "교내", "해당",
    "이상", "이내", "통해", "관련", "있으", "으며", "하시", "시기", "기바", "참고", "아래",
    "포스", "스터", "개요", "시상", "상금", "수상", "우수", "장려", "명의", "상장", "에게",
    "부터", "이며", "하며", "되는", "되어", "하기", "위한", "따라", "양식", "참조", "부문",
    "규모", "같이", "아래", "래와", "작성", "다운", "운로", "로드", "확인", "사항", "세부", NULL
};
/* 낱말 끝에 붙은 조사 한 글자는 떼어 낸 형태도 함께 쓴다.
 * 예: "영상을" → "영상을" 과 "영상" 둘 다 토큰으로 */
static const char *JOSA[] = {
    "을", "를", "이", "가", "은", "는", "의", "에", "로", "과", "와", "도", "만", NULL
};

/* 같은 뜻으로 보는 낱말. 왼쪽이 나오면 오른쪽 낱말도 함께 센다 (0.8 배).
 * 예: "유튜브" 가 나오면 "영상" 도 0.8 만큼 센다 → "영상" 학과 프로필과 매칭된다. */
static const char *SYNONYMS[][2] = {
    { "유튜브", "영상" }, { "숏폼", "영상" }, { "쇼츠", "영상" }, { "동영상", "영상" },
    { "영상제작", "영상" }, { "뮤직비디오", "영상" }, { "촬영", "영상" }, { "릴스", "영상" },
    { "코딩", "개발" }, { "프로그래밍", "개발" }, { "앱개발", "개발" }, { "웹개발", "개발" },
    { "소프트웨어", "개발" }, { "인공지능", "ai" }, { "생성형", "ai" }, { "챗봇", "ai" },
    { "빅데이터", "데이터" }, { "데이터분석", "데이터" }, { "통계", "데이터" },
    { "웹툰", "만화" }, { "컷만화", "만화" }, { "일러스트", "디자인" }, { "브랜딩", "디자인" },
    { "시각디자인", "디자인" }, { "그래픽", "디자인" }, { "스타트업", "창업" }, { "사업화", "창업" },
    { "홍보", "마케팅" }, { "광고", "마케팅" }, { "캠페인", "마케팅" }, { "핀테크", "금융" },
    { "투자", "금융" }, { "에세이", "글쓰기" }, { "수기", "글쓰기" }, { "체험수기", "글쓰기" },
    { "논문", "글쓰기" }, { "시나리오", "글쓰기" }, { "탄소중립", "환경" }, { "친환경", "환경" },
    { "기후", "환경" }, { "아이디어", "기획" }, { "정책제안", "공공정책" }, { "정책", "공공정책" },
    { "청소년", "교육" }, { "예방교육", "교육" }, { "건축", "도시건축" }, { "도시", "도시건축" },
    { "메타버스", "게임" }, { "e스포츠", "게임" }, { "화장품", "뷰티" }, { "메이크업", "뷰티" },
    { "헤어", "뷰티" }, { "체육", "스포츠" }, { "운동", "스포츠" }, { "바이오", "과학" },
    { "과학기술", "과학" }, { "외국어", "글로벌" }, { "국제", "글로벌" }, { "해외", "글로벌" },
    { "밴드", "음악" }, { "작곡", "음악" }, { "연극", "공연" }, { "뮤지컬", "공연" },
    { NULL, NULL }
};

/* 동의어 표에서 word 의 대표 낱말을 찾는다. 없으면 NULL. */
static const char *synonym_of(const char *word)
{
    int i;

    for (i = 0; SYNONYMS[i][0]; i++)
        if (strcmp(word, SYNONYMS[i][0]) == 0)
            return SYNONYMS[i][1];
    return NULL;
}

/* s 가 NULL 로 끝나는 목록 list 에 정확히 있으면 1 */
static int in_list(const char *s, const char **list)
{
    for (; *list; list++)
        if (strcmp(s, *list) == 0)
            return 1;
    return 0;
}

/* UTF-8 한 글자를 읽어 코드포인트를 돌려준다. len 에 바이트 수.
 * 첫 바이트의 앞쪽 비트로 길이를 정하고, 각 바이트의 데이터 비트를 이어 붙인다.
 * 뒤 바이트가 문자열 끝(0)이면 깨진 글자로 보고 U+FFFD(대체 문자)를 돌려준다. */
static unsigned utf8_next(const unsigned char *s, int *len)
{
    if (s[0] < 0x80)                                 { *len = 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1])              { *len = 2; return ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3F); }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2])      { *len = 3; return ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3F); }
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *len = 4;
        return ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3F);
    }
    *len = 1;
    return 0xFFFD;
}

/* 글자 종류: 같은 종류가 이어지는 구간을 한 낱말로 본다 */
#define CLS_NONE   0   /* 공백·문장부호·기타 (낱말 구분자) */
#define CLS_HANGUL 1   /* 한글 완성형 음절 */
#define CLS_LATIN  2   /* 영문·숫자 */

static int char_class(unsigned cp)
{
    if (cp >= 0xAC00 && cp <= 0xD7A3)   /* '가' ~ '힣' (한글 음절 11,172자) */
        return CLS_HANGUL;
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9'))
        return CLS_LATIN;
    return CLS_NONE;
}

/* 토큰 하나를 벡터에 넣는다. kind 는 prefix 의 첫 글자('w' 또는 'b') */
static void add_token(Vec *v, const char *prefix, const char *text, float w)
{
    vec_push(v, fnv1a(prefix, text), w, prefix[0], text);
}

/* 한 낱말(같은 종류 글자가 이어진 구간)을 토큰으로 만든다.
 * start/len 은 원문 안의 바이트 범위, nchars 는 글자 수. */
static void flush_word(Vec *v, const char *start, int len, int nchars, int cls, float w)
{
    char word[64];

    if (len <= 0 || len >= (int)sizeof word)   /* 너무 긴 낱말(주소 등)은 버린다 */
        return;
    memcpy(word, start, (size_t)len);
    word[len] = '\0';

    if (cls == CLS_LATIN) {
        /* 영문·숫자: 소문자로 바꾸고, 2글자 이상이며 숫자만으로 된 게 아닐 때만 쓴다
         * ("2026", "10" 같은 숫자는 학과 구분에 의미가 없다) */
        int i, digits = 1;

        for (i = 0; word[i]; i++) {
            if (word[i] >= 'A' && word[i] <= 'Z')
                word[i] = (char)(word[i] - 'A' + 'a');
            if (word[i] < '0' || word[i] > '9')
                digits = 0;
        }
        if (nchars >= 2 && !digits && !in_list(word, STOP_WORDS)) {
            const char *syn = synonym_of(word);
            add_token(v, "w:", word, w);
            if (syn)
                add_token(v, "w:", syn, w * 0.8f);
        }
        return;
    }

    /* 한글: 낱말 자체 + 조사 뗀 형태 + 같은 뜻 낱말 + 두 글자 묶음 */
    if (nchars >= 2 && nchars <= 12) {
        const char *syn = NULL;

        if (!in_list(word, STOP_WORDS)) {
            add_token(v, "w:", word, w);
            syn = synonym_of(word);
        }
        /* 마지막 글자(한글 = 3바이트)가 조사면 뗀 형태(어간)도 넣는다 */
        if (nchars >= 3 && in_list(word + len - 3, JOSA)) {
            char stem[64];
            memcpy(stem, word, (size_t)(len - 3));
            stem[len - 3] = '\0';
            if (!in_list(stem, STOP_WORDS)) {
                add_token(v, "w:", stem, w);
                if (!syn)
                    syn = synonym_of(stem);
            }
        }
        if (syn)
            add_token(v, "w:", syn, w * 0.8f);
    }
    if (nchars >= 2) {
        int i;
        /* 한글 음절은 UTF-8 로 모두 3바이트다.
         * 3바이트씩 밀면서 6바이트(=두 글자)를 잘라 두 글자 묶음을 만든다.
         * 예: "인공지능" → "인공", "공지", "지능" */
        for (i = 0; i + 6 <= len; i += 3) {
            char pair[8];
            memcpy(pair, word + i, 6);
            pair[6] = '\0';
            if (!in_list(pair, STOP_BIGRAMS))
                add_token(v, "b:", pair, w * BIGRAM_WEIGHT);
        }
    }
}

/* 글 전체를 토큰화해 v 에 넣는다. w 는 이 글의 토큰 가중치(제목이면 TITLE_WEIGHT).
 * 글자를 하나씩 읽으며 글자 종류가 바뀌는 지점에서 앞 구간을 낱말로 내보낸다.
 * 문자열 끝(*s == 0)도 종류가 CLS_NONE 으로 바뀌는 지점으로 처리해 마지막 낱말을 내보낸다. */
static void tokenize(Vec *v, const char *text, float w)
{
    const unsigned char *s = (const unsigned char *)(text ? text : "");
    const char *word_start = NULL;     /* 현재 낱말의 시작 위치 */
    int word_cls = CLS_NONE, nchars = 0;

    for (;;) {
        int len = 1;
        unsigned cp = *s ? utf8_next(s, &len) : 0;
        int cls = *s ? char_class(cp) : CLS_NONE;

        if (cls != word_cls) {
            if (word_cls != CLS_NONE)  /* 이전 구간이 낱말이었으면 내보낸다 */
                flush_word(v, word_start, (int)((const char *)s - word_start), nchars, word_cls, w);
            word_cls = cls;
            word_start = (const char *)s;
            nchars = 0;
        }
        if (!*s)
            break;
        nchars++;
        s += len;
    }
}

/* ============================================================ 모델 */

/* 문서 하나 (게시물 또는 학과) */
typedef struct {
    unsigned id;   /* posts.id 또는 departments.id */
    Vec      v;    /* TF-IDF 벡터 */
} Doc;

/* 문서 빈도 표 (키 순 정렬)
 * df = 그 낱말이 나온 문서 수 (IDF 계산에 필요) */
typedef struct { uint32_t key; int df; } Df;

/* 학습된 모델 전체 */
typedef struct {
    int      ready;          /* 학습이 끝나 쓸 수 있으면 1 */
    char     signature[160]; /* 학습할 때의 데이터 서명 (바뀌면 재학습) */
    Doc     *posts;          /* 게시물 벡터들 */
    int      nposts;
    Doc     *depts;          /* 학과 벡터들 (Rocchio 반영 후) */
    int      ndepts;
    float   *cos;          /* [nposts * ndepts] */
    Df      *df;           /* 개인 맞춤 질의를 같은 기준으로 벡터화하려고 남겨 둔다 */
    int      ndf, ndocs;     /* df 표 크기, IDF 계산에 쓴 전체 문서 수 */
    double   tau;            /* 점수 보정 척도 (ml.h 의 4번 참고) */
    int      labels;         /* 학습에 쓴 (게시물, 학과) 정답 연결 수 */
    char     trained_at[32];
    int      dim;          /* 임베딩 차원. 0 이면 임베딩 없이 TF-IDF 만 */
    float   *pemb;         /* [nposts * dim] 게시물 임베딩 */
    float   *demb;         /* [ndepts * dim] 학과 임베딩 (Rocchio 반영) */
    double   mu;           /* 게시물-학과 임베딩 코사인 평균 (hybrid 참고) */
    char     emb_model[128];
    double   pa, pb;       /* 확률 보정 P = sigmoid(pa * s + pb) */
    int      npos, nneg;   /* 보정에 쓴 양성·음성 쌍 */
} Model;

/* 정답 연결 하나: 게시물 인덱스 pi, 학과 인덱스 di, 가중치 w (체크 1.0 / 댓글·♥ 0.5) */
typedef struct { int pi, di; float w; } Label;

static int float_desc(const void *a, const void *b);   /* 아래에서 정의 (전방 선언) */

/* 시그모이드(로지스틱) 함수: 실수 전체를 (0, 1) 로 눌러 확률처럼 만든다.
 * x = 0 → 0.5, x 가 클수록 1 에, 작을수록 0 에 가까워진다. */
static double sigmoid(double x)
{
    return 1.0 / (1.0 + exp(-x));
}

/* 확률 보정: 유사도 s 를 "이 학과에 맞을 확률" 로 바꾸는 로지스틱 함수의 a, b 를 맞춘다.
 *   양성 = 정답 쌍 (가중치 = 체크 1.0, 댓글·♥ 0.5)
 *   음성 = 정답이 하나라도 있는 글의 나머지 학과 (정답이 없는 글은 모르므로 뺀다)
 * 정답이 적을 때를 위해 사전값 (모든 쌍의 중앙값 -> 5%, 상위 10% 지점 -> 70%) 을 정규분포 사전으로
 * 두고 MAP 를 뉴턴법으로 푼다. 정답이 없으면 사전값 그대로다.
 *
 * 수학 정리:
 *   목표 = 가중 로그 손실 Σ w·[ -y·log p - (1-y)·log(1-p) ]  +  (a-a0)²/(2·sa²) + (b-b0)²/(2·sb²)
 *          (p = sigmoid(a·s + b), 뒤 두 항이 정규분포 사전 = L2 정칙화)
 *   기울기  ga = Σ w(p-y)s + (a-a0)/sa²,   gb = Σ w(p-y) + (b-b0)/sb²
 *   헤시안  haa = Σ w p(1-p) s² + 1/sa²,  hab = Σ w p(1-p) s,  hbb = Σ w p(1-p) + 1/sb²
 *   뉴턴 갱신 [a,b] -= H⁻¹ · g   (2x2 역행렬을 직접 계산) */
static void fit_calibration(Model *m, const Label *lab, int nlab)
{
    size_t npairs = (size_t)m->nposts * (size_t)m->ndepts, k;   /* 전체 (게시물, 학과) 쌍 수 */
    float *yw = NULL, *sorted = NULL;   /* yw: 쌍별 정답 가중치 (0 = 음성) */
    char *has = NULL;                   /* has[i]: 게시물 i 에 정답이 하나라도 있는지 */
    /* s_hi: 상위 10% 지점의 유사도. 점수 보정(train)에서 90% 지점이 tau·ln5 가 되도록 맞췄으므로 그 값을 쓴다 */
    double s_med = 0, s_hi = m->tau * log(5.0);
    /* L0, L1: 목표 확률 5%, 70% 의 로짓(log-odds) 값. sigmoid(L) = 확률 */
    double L0 = log(0.05 / 0.95), L1 = log(0.7 / 0.3), a0, b0, sa, sb = 1.5, a, b;
    int i, j, it;

    m->npos = m->nneg = 0;
    if (npairs == 0)
        return;
    /* 1) 모든 쌍 유사도의 중앙값 구하기 */
    sorted = (float *)malloc(npairs * sizeof *sorted);
    if (sorted) {
        memcpy(sorted, m->cos, npairs * sizeof *sorted);
        qsort(sorted, npairs, sizeof *sorted, float_desc);
        s_med = sorted[npairs / 2];
        free(sorted);
    }
    /* 2) 사전값: 두 점 (s_med → 5%), (s_hi → 70%) 을 지나는 직선 a0·s + b0 = 로짓
     *    두 점이 너무 가까우면(0.02 미만) 기울기가 폭주하지 않게 최소 간격을 둔다. */
    a0 = (L1 - L0) / (s_hi - s_med > 0.02 ? s_hi - s_med : 0.02);
    b0 = L1 - a0 * s_hi;
    sa = a0 / 2;    /* 사전 분포의 표준편차 (기울기는 사전값의 절반 정도까지 움직일 수 있게) */
    a = a0;         /* 사전값에서 출발 */
    b = b0;

    /* 3) 정답 표 만들기 */
    yw = (float *)calloc(npairs, sizeof *yw);
    has = (char *)calloc((size_t)m->nposts, 1);
    if (yw && has) {
        for (i = 0; i < nlab; i++) {
            yw[(size_t)lab[i].pi * m->ndepts + lab[i].di] = lab[i].w;   /* 2차원 [pi][di] 를 1차원으로 */
            has[lab[i].pi] = 1;
        }
        for (i = 0; i < m->nposts; i++)
            if (has[i])
                for (j = 0; j < m->ndepts; j++) {
                    if (yw[(size_t)i * m->ndepts + j] > 0)
                        m->npos++;
                    else
                        m->nneg++;
                }

        /* 4) 뉴턴법 (최대 50회, 양성이 없으면 사전값 그대로) */
        for (it = 0; it < 50 && m->npos > 0; it++) {
            /* 사전 항의 기울기와 헤시안부터 시작 */
            double ga = (a - a0) / (sa * sa), gb = (b - b0) / (sb * sb);
            double haa = 1 / (sa * sa), hbb = 1 / (sb * sb), hab = 0, det, da, db;

            /* 데이터 항 누적 */
            for (i = 0; i < m->nposts; i++) {
                if (!has[i])
                    continue;
                for (j = 0; j < m->ndepts; j++) {
                    k = (size_t)i * m->ndepts + j;
                    {
                        /* y: 양성이면 1, w: 양성은 정답 가중치, 음성은 1 */
                        double s = m->cos[k], y = yw[k] > 0, w = yw[k] > 0 ? yw[k] : 1.0;
                        double p = sigmoid(a * s + b), q = w * p * (1 - p);
                        ga += w * (p - y) * s;
                        gb += w * (p - y);
                        haa += q * s * s;
                        hab += q * s;
                        hbb += q;
                    }
                }
            }
            /* 2x2 헤시안 [[haa,hab],[hab,hbb]] 의 역행렬로 갱신량 계산 */
            det = haa * hbb - hab * hab;
            if (det <= 0)            /* 수치 문제로 역행렬이 없으면 중단 */
                break;
            da = (hbb * ga - hab * gb) / det;
            db = (haa * gb - hab * ga) / det;
            a -= da;
            b -= db;
            if (fabs(da) < 1e-7 && fabs(db) < 1e-7)   /* 수렴 */
                break;
        }
    }
    free(yw);
    free(has);
    m->pa = a;
    m->pb = b;
}

static volatile LONG    g_init;    /* 0 미초기화, 1 초기화 중, 2 완료 (ai.c 와 같은 방식) */
static CRITICAL_SECTION g_lock;    /* g_model 보호 */
static Model            g_model;   /* 현재 모델 */

/* g_lock 을 한 번만 초기화 (스레드 안전) */
static void ensure_init(void)
{
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        memset(&g_model, 0, sizeof g_model);
        InterlockedExchange(&g_init, 2);
    }
    while (g_init != 2)
        Sleep(0);
}

/* 모델이 가진 모든 메모리를 해제하고 0 으로 초기화 */
static void model_free(Model *m)
{
    int i;

    for (i = 0; i < m->nposts; i++)
        vec_free(&m->posts[i].v);
    for (i = 0; i < m->ndepts; i++)
        vec_free(&m->depts[i].v);
    free(m->posts);
    free(m->depts);
    free(m->cos);
    free(m->df);
    free(m->pemb);
    free(m->demb);
    memset(m, 0, sizeof *m);
}

/* 학습 데이터가 바뀌었는지 가늠하는 문자열. 다르면 다시 학습한다.
 * 게시물 수/최대 id/제목·본문 길이의 CRC 합/대상 학과 연결 수/댓글 수/♥ 조합/학과 프로필 CRC 를
 * "/" 로 이어 붙인 문자열이다. 이 중 하나라도 바뀌면 서명이 달라진다.
 * (데이터 전체를 비교하지 않고 싼 질의 하나로 변화를 감지하는 방법. 완벽하진 않지만 실용적) */
static void current_signature(char *out, size_t outsz)
{
    MYSQL_RES *qr = db_query(
        "SELECT CONCAT((SELECT COUNT(*) FROM posts), '/', "
        "              (SELECT IFNULL(MAX(id), 0) FROM posts), '/', "
        "              (SELECT IFNULL(SUM(CRC32(CONCAT(title, LENGTH(body)))), 0) FROM posts), '/', "
        "              (SELECT COUNT(*) FROM post_departments), '/', "
        "              (SELECT COUNT(*) FROM comments), '/', "
        "              (SELECT IFNULL(SUM(recruit_id * 131 + user_id), 0) FROM recruit_likes), '/', "
        "              (SELECT IFNULL(SUM(CRC32(keywords)), 0) FROM dept_profiles))");
    MYSQL_ROW row;

    out[0] = '\0';
    if (!qr)
        return;
    row = mysql_fetch_row(qr);
    if (row && row[0])
        str_copy(out, outsz, row[0]);
    mysql_free_result(qr);
}

/* TF 를 1 + ln(tf) 로 줄이고 IDF 를 곱한 뒤 정규화한다. */
/* qsort/bsearch 비교: Df 키 오름차순 */
static int df_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Df *)a)->key, y = ((const Df *)b)->key;
    return x < y ? -1 : x > y;
}

/* 출현 횟수 벡터 v 를 TF-IDF 벡터로 바꾼다.
 *   TF  = 1 + ln(횟수)   (sublinear TF: 10번 나와도 10배가 아니라 약 3.3배로 - 반복의 영향 완화)
 *   IDF = ln((N+1)/(df+1)) + 1   (스무딩: 처음 보는 낱말(df=0)도 0 으로 나누지 않게, +1 로 하한)
 * df 표는 정렬되어 있으므로 bsearch(이진 탐색, O(log n))로 찾는다. */
static void apply_tfidf(Vec *v, const Df *df, int ndf, int ndocs)
{
    int i;

    for (i = 0; i < v->n; i++) {
        Df probe, *hit;
        double idf, tf = v->t[i].w;

        probe.key = v->t[i].key;
        hit = (Df *)bsearch(&probe, df, (size_t)ndf, sizeof *df, df_cmp);
        idf = log((ndocs + 1.0) / ((hit ? hit->df : 0) + 1.0)) + 1.0;
        v->t[i].w = (float)((tf > 0 ? 1.0 + log(tf) : 0.0) * idf);
    }
    vec_normalize(v);
}

/* qsort 비교: float 내림차순 */
static int float_desc(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* 모델을 처음부터 학습한다. 성공 1.
 * 단계:
 *   1) 게시물·학과 텍스트를 읽어 토큰화 (출현 횟수 벡터)
 *   2) (가능하면) 딥러닝 임베딩 계산
 *   3) 문서 빈도(df) 표 → TF-IDF 로 변환
 *   4) 정답 연결(관리자 체크, 댓글, ♥)을 읽어 Rocchio 로 학과 벡터 보정
 *   5) 모든 (게시물, 학과) 유사도 표(cos) 계산
 *   6) 점수 척도(tau)와 확률 보정(pa, pb) 맞추기 */
static int train(Model *m)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    my_ulonglong rows;
    Df *df = NULL;
    int ndf = 0, i, j, total_terms = 0;
    Vec *centroid = NULL;          /* 학과별 "연결된 게시물 벡터의 합" */
    /* cweight: 학과별 연결 가중치 합, ecent: 임베딩 공간의 centroid [ndepts * dim],
     * ecos: 임베딩 코사인 표 [nposts * ndepts] */
    float *cweight = NULL, *ecent = NULL, *ecos = NULL;
    char **ptexts = NULL, **dtexts = NULL;   /* 임베딩에 보낼 원문들 */
    Label *lab = NULL;             /* 정답 연결 목록 (확률 보정용) */
    int nlab = 0, caplab = 0;
    double t0 = (double)GetTickCount64();   /* 학습 시간 측정용 */

    memset(m, 0, sizeof *m);
    current_signature(m->signature, sizeof m->signature);

    /* 게시물 */
    qr = db_query("SELECT id, title, body FROM posts ORDER BY id");
    if (!qr)
        return 0;
    rows = mysql_num_rows(qr);
    m->posts = (Doc *)calloc((size_t)(rows ? rows : 1), sizeof *m->posts);
    ptexts = (char **)calloc((size_t)(rows ? rows : 1), sizeof *ptexts);
    while (m->posts && ptexts && (row = mysql_fetch_row(qr)) != NULL) {
        Doc *d = &m->posts[m->nposts];
        ptexts[m->nposts++] = join3(row[1], row[2], NULL);   /* 제목 + 본문 (임베딩용) */
        d->id = (unsigned)strtoul(row[0], NULL, 10);
        tokenize(&d->v, row[1], TITLE_WEIGHT);   /* 제목은 2배 가중 */
        tokenize(&d->v, row[2], 1.0f);
        vec_compact(&d->v);                      /* 같은 낱말 합치기 → 출현 횟수 */
    }
    mysql_free_result(qr);

    /* 학과 (프로필이 없으면 학과 이름만으로 시작한다)
     * dept_profiles.keywords = 학과를 설명하는 키워드 글 (sql/05_dept_profiles.sql) */
    qr = db_query("SELECT d.id, d.name, IFNULL(p.keywords, '') FROM departments d "
                  "LEFT JOIN dept_profiles p ON p.department_id = d.id ORDER BY d.id");
    if (!qr)
        goto fail;
    rows = mysql_num_rows(qr);
    m->depts = (Doc *)calloc((size_t)(rows ? rows : 1), sizeof *m->depts);
    dtexts = (char **)calloc((size_t)(rows ? rows : 1), sizeof *dtexts);
    while (m->depts && dtexts && (row = mysql_fetch_row(qr)) != NULL) {
        Doc *d = &m->depts[m->ndepts];
        dtexts[m->ndepts++] = join3(row[1], row[2], NULL);
        d->id = (unsigned)strtoul(row[0], NULL, 10);
        tokenize(&d->v, row[1], 1.0f);
        tokenize(&d->v, row[2], 1.0f);
        vec_compact(&d->v);
    }
    mysql_free_result(qr);
    if (!m->posts || !m->depts || !ptexts || !dtexts || m->ndepts == 0)
        goto fail;

    /* 딥러닝 임베딩: 게시물은 passage, 학과는 query 로 넣는다 (e5 모델 규칙).
     * 서비스가 없거나 두 결과의 차원이 다르면 임베딩 없이 간다. */
    {
        int pdim = 0, ddim = 0;

        m->pemb = embed_all("passage", ptexts, m->nposts, &pdim, m->emb_model, sizeof m->emb_model);
        if (m->pemb)
            m->demb = embed_all("query", dtexts, m->ndepts, &ddim, NULL, 0);
        if (!m->pemb || !m->demb || pdim != ddim) {
            free(m->pemb);
            free(m->demb);
            m->pemb = m->demb = NULL;
            m->emb_model[0] = '\0';
        } else {
            m->dim = pdim;
        }
    }

    /* 문서 빈도 (게시물 + 학과 프로필을 한 말뭉치로 본다)
     * 방법: 모든 문서의 (낱말, 1) 을 한 배열에 모아 정렬한 뒤 같은 낱말끼리 개수를 센다.
     * 각 문서 벡터는 이미 compact 되어 낱말이 문서당 한 번만 있으므로, 센 개수 = 낱말이 나온 문서 수. */
    for (i = 0; i < m->nposts; i++) total_terms += m->posts[i].v.n;
    for (i = 0; i < m->ndepts; i++) total_terms += m->depts[i].v.n;
    df = (Df *)malloc((size_t)(total_terms ? total_terms : 1) * sizeof *df);
    if (!df)
        goto fail;
    /* 쉼표 연산자: "df[ndf].key = ..." 와 "df[ndf++].df = 1" 을 한 문장으로 차례로 실행 */
    for (i = 0; i < m->nposts; i++)
        for (j = 0; j < m->posts[i].v.n; j++)
            df[ndf].key = m->posts[i].v.t[j].key, df[ndf++].df = 1;
    for (i = 0; i < m->ndepts; i++)
        for (j = 0; j < m->depts[i].v.n; j++)
            df[ndf].key = m->depts[i].v.t[j].key, df[ndf++].df = 1;
    qsort(df, (size_t)ndf, sizeof *df, df_cmp);
    {
        int k = 0;
        for (i = 1; i < ndf; i++) {
            if (df[i].key == df[k].key)
                df[k].df++;
            else
                df[++k] = df[i];
        }
        ndf = ndf ? k + 1 : 0;
    }

    /* 출현 횟수 → TF-IDF (전체 문서 수 = 게시물 + 학과) */
    for (i = 0; i < m->nposts; i++)
        apply_tfidf(&m->posts[i].v, df, ndf, m->nposts + m->ndepts);
    for (i = 0; i < m->ndepts; i++)
        apply_tfidf(&m->depts[i].v, df, ndf, m->nposts + m->ndepts);

    /* Rocchio: 학과에 연결된 게시물 벡터의 평균을 더한다. */
    centroid = (Vec *)calloc((size_t)m->ndepts, sizeof *centroid);
    cweight = (float *)calloc((size_t)m->ndepts, sizeof *cweight);
    if (!centroid || !cweight)
        goto fail;
    if (m->dim) {
        ecent = (float *)calloc((size_t)m->ndepts * (size_t)m->dim, sizeof *ecent);
        if (!ecent)
            goto fail;
    }

    /* 정답 연결 세 종류를 UNION ALL 로 모은 뒤, 같은 (게시물, 학과) 쌍은 가장 큰 가중치만 쓴다.
     *   관리자 체크 1.0 / 그 학과 학생의 댓글 0.5 / 그 학과 학생의 ♥(모집글 → 연결된 공모전) 0.5 */
    qr = db_query(
        "SELECT post_id, department_id, MAX(w) FROM ("
        "  SELECT post_id, department_id, 1.0 AS w FROM post_departments "
        "  UNION ALL "
        "  SELECT c.post_id, u.department_id, 0.5 FROM comments c "
        "  JOIN users u ON u.id = c.author_id "
        "  WHERE u.role = 'student' AND u.department_id IS NOT NULL "
        "  UNION ALL "
        /* 공모전에 연결된 모집글을 마음에 들어한 학생 -> 그 공모전은 그 학생 학과에도 관심거리 */
        "  SELECT r.post_id, u.department_id, 0.5 FROM recruit_likes l "
        "  JOIN recruits r ON r.id = l.recruit_id JOIN users u ON u.id = l.user_id "
        "  WHERE r.post_id IS NOT NULL AND u.role = 'student' AND u.department_id IS NOT NULL"
        ") x GROUP BY post_id, department_id");
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            unsigned pid = (unsigned)strtoul(row[0], NULL, 10);
            unsigned did = (unsigned)strtoul(row[1], NULL, 10);
            float w = (float)atof(row[2]);
            int pi = -1, di = -1;   /* id → 배열 인덱스 */

            for (i = 0; i < m->nposts; i++) if (m->posts[i].id == pid) { pi = i; break; }
            for (i = 0; i < m->ndepts; i++) if (m->depts[i].id == did) { di = i; break; }
            if (pi < 0 || di < 0)
                continue;
            /* 학과 di 의 centroid 에 게시물 벡터를 가중치 w 로 더한다 */
            vec_add_scaled(&centroid[di], &m->posts[pi].v, w);
            if (ecent)
                for (j = 0; j < m->dim; j++)
                    ecent[(size_t)di * m->dim + j] += w * m->pemb[(size_t)pi * m->dim + j];
            cweight[di] += w;
            m->labels++;
            /* 정답 목록에 추가 (필요하면 배열을 2배로 늘린다. 실패하면 그냥 빠뜨린다) */
            if (nlab == caplab) {
                int cap = caplab ? caplab * 2 : 64;
                Label *t = (Label *)realloc(lab, (size_t)cap * sizeof *t);
                if (t) {
                    lab = t;
                    caplab = cap;
                }
            }
            if (nlab < caplab) {
                lab[nlab].pi = pi;
                lab[nlab].di = di;
                lab[nlab++].w = w;
            }
        }
        mysql_free_result(qr);
    }
    /* 학과 벡터 = ALPHA x 프로필 + beta x 정규화된 centroid
     * beta = BETA x (가중치합 / (가중치합 + K)) : 연결이 적은 학과는 beta 가 작아져
     * 우연히 연결된 글 한두 개가 학과 벡터를 크게 흔들지 않는다 (축소 추정, shrinkage). */
    for (i = 0; i < m->ndepts; i++) {
        Vec dv = { 0 };   /* 새 학과 벡터 */

        vec_add_scaled(&dv, &m->depts[i].v, ROCCHIO_ALPHA);
        if (cweight[i] > 0) {
            float beta = ROCCHIO_BETA * cweight[i] / (cweight[i] + SHRINK_K);

            vec_keep_top(&centroid[i], CENTROID_TOP);   /* 상위 40개 특징만 (잡음 낱말 제거) */
            vec_normalize(&centroid[i]);                /* 합 → 방향만 (평균과 같은 방향) */
            vec_add_scaled(&dv, &centroid[i], beta);

            /* 임베딩 공간에서도 같은 식: 학과 = 프로필 + beta x 연결된 게시물 평균 */
            if (ecent) {
                float *dc = &ecent[(size_t)i * m->dim], *de = &m->demb[(size_t)i * m->dim];
                dense_normalize(dc, m->dim);
                for (j = 0; j < m->dim; j++)
                    de[j] += beta * dc[j];
            }
        }
        if (m->dim)
            dense_normalize(&m->demb[(size_t)i * m->dim], m->dim);
        vec_normalize(&dv);
        vec_free(&m->depts[i].v);   /* 옛 프로필 벡터를 새 벡터로 교체 */
        m->depts[i].v = dv;
        vec_free(&centroid[i]);
    }

    /* 유사도 표: 임베딩 코사인의 평균을 먼저 구한 뒤 TF-IDF 와 섞는다.
     * cos[i * ndepts + j] = 게시물 i 와 학과 j 의 최종 유사도 (2차원 표를 1차원 배열에 저장) */
    m->cos = (float *)calloc((size_t)(m->nposts ? m->nposts : 1) * (size_t)m->ndepts, sizeof *m->cos);
    if (!m->cos)
        goto fail;
    if (m->dim && m->nposts > 0) {
        double sum = 0;

        ecos = (float *)malloc((size_t)m->nposts * (size_t)m->ndepts * sizeof *ecos);
        if (!ecos)
            goto fail;
        for (i = 0; i < m->nposts; i++)
            for (j = 0; j < m->ndepts; j++) {
                float c = (float)dense_dot(&m->pemb[(size_t)i * m->dim], &m->demb[(size_t)j * m->dim], m->dim);
                ecos[i * m->ndepts + j] = c;
                sum += c;
            }
        m->mu = sum / ((double)m->nposts * m->ndepts);   /* 임베딩 코사인의 전체 평균 */
    }
    for (i = 0; i < m->nposts; i++)
        for (j = 0; j < m->ndepts; j++)
            m->cos[i * m->ndepts + j] = (float)hybrid(vec_dot(&m->posts[i].v, &m->depts[j].v),
                                                      ecos ? ecos[i * m->ndepts + j] : 0,
                                                      m->mu, m->dim);

    /* 점수 보정: 게시물마다 가장 잘 맞는 학과의 유사도를 모아, 그 상위 10% 지점이 80점이 되게 한다.
     * score = 100 * (1 - exp(-cos / tau)),  80 = 100 * (1 - exp(-p90 / tau))  ->  tau = p90 / ln 5
     * (1 - e^(-x) 꼴이라 유사도가 커질수록 100 에 가까워지되 넘지 않는다) */
    {
        size_t np = m->nposts > 0 ? (size_t)m->nposts : 1;
        float *best = (float *)malloc(np * sizeof *best);   /* 게시물별 최고 유사도 */
        double p90 = 0.2;                                    /* 게시물이 없을 때의 기본값 */

        if (best && m->nposts > 0) {
            for (i = 0; i < m->nposts; i++) {
                best[i] = 0;
                for (j = 0; j < m->ndepts; j++)
                    if (m->cos[i * m->ndepts + j] > best[i])
                        best[i] = m->cos[i * m->ndepts + j];
            }
            qsort(best, (size_t)m->nposts, sizeof *best, float_desc);
            p90 = best[m->nposts / 10];   /* 내림차순 정렬의 10% 위치 = 상위 10% 지점 */
        }
        free(best);
        if (p90 < 0.05)                   /* 너무 작으면 모든 점수가 100 에 몰리므로 하한 */
            p90 = 0.05;
        m->tau = p90 / log(5.0);
    }
    fit_calibration(m, lab, nlab);

    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_s(&tmv, &now);
        strftime(m->trained_at, sizeof m->trained_at, "%Y-%m-%d %H:%M:%S", &tmv);
    }
    /* 성공: df 표는 개인 맞춤·예측에서 쓰려고 모델에 남기고, 임시 자료는 해제 */
    m->ready = 1;
    m->df = df;
    m->ndf = ndf;
    m->ndocs = m->nposts + m->ndepts;
    free(centroid);
    free(cweight);
    free(ecent);
    free(ecos);
    free(lab);
    free_texts(ptexts, m->nposts);
    free_texts(dtexts, m->ndepts);
    log_info("확률 보정: 양성 %d, 음성 %d -> P = sigmoid(%.2f s %+.2f)", m->npos, m->nneg, m->pa, m->pb);
    if (m->dim)
        log_info("자체 모델 학습 (임베딩 %s, %d차원 + TF-IDF): 게시물 %d, 학과 %d, 연결 %d, "
                 "mu %.3f, tau %.3f (%.0f ms)", m->emb_model, m->dim, m->nposts, m->ndepts,
                 m->labels, m->mu, m->tau, (double)GetTickCount64() - t0);
    else
        log_info("자체 모델 학습 (TF-IDF): 게시물 %d, 학과 %d, 연결 %d, tau %.3f (%.0f ms)",
                 m->nposts, m->ndepts, m->labels, m->tau, (double)GetTickCount64() - t0);
    return 1;

fail:
    /* 실패: 만들던 것을 모두 해제하고 모델을 빈 상태로 */
    if (centroid)
        for (i = 0; i < m->ndepts; i++)
            vec_free(&centroid[i]);
    free(centroid);
    free(cweight);
    free(ecent);
    free(ecos);
    free(lab);
    free_texts(ptexts, m->nposts);
    free_texts(dtexts, m->ndepts);
    free(df);
    model_free(m);
    log_err("자체 모델을 학습할 수 없습니다.");
    return 0;
}

/* 필요하면 다시 학습한다. g_lock 을 잡은 상태에서 부른다.
 *   force = 1 : 무조건 재학습
 *   force = 0 : 데이터 서명이 바뀌었거나, 임베딩 없이 학습했는데 지금은 임베딩 서비스가 떠 있으면 재학습 */
static int ensure_model(int force)
{
    char sig[160];

    if (!force && g_model.ready) {
        current_signature(sig, sizeof sig);
        /* 임베딩 없이 학습한 뒤 서비스가 뜨면 다시 학습한다. */
        if (strcmp(sig, g_model.signature) == 0 && (g_model.dim || !embed_available()))
            return 1;
    }
    model_free(&g_model);
    return train(&g_model);
}

/* 현재 모델 상태를 MlInfo 로 복사 (info 가 NULL 이면 아무것도 안 함) */
static void fill_info(MlInfo *info)
{
    if (!info)
        return;
    info->posts = g_model.nposts;
    info->departments = g_model.ndepts;
    info->labels = g_model.labels;
    info->tau = g_model.tau;
    str_copy(info->trained_at, sizeof info->trained_at, g_model.trained_at);
    info->dim = g_model.dim;
    str_copy(info->embed_model, sizeof info->embed_model, g_model.emb_model);
    info->pa = g_model.pa;
    info->pb = g_model.pb;
    info->positives = g_model.npos;
    info->negatives = g_model.nneg;
}

/* 유사도 → 0~100 점수 (train 의 "점수 보정" 식). +0.5 후 정수 변환 = 반올림 */
static int to_score(double cos)
{
    double s;

    if (cos <= 0 || g_model.tau <= 0)
        return 0;
    s = 100.0 * (1.0 - exp(-cos / g_model.tau));
    return (int)(s + 0.5);
}

/* 유사도 → 확률 (Platt scaling) */
static double to_prob(double s)
{
    return sigmoid(g_model.pa * s + g_model.pb);
}

/* a 가 b 에 조사 한 글자만 붙은 형태인지 (프로젝트를 / 프로젝트)
 * 양방향으로 확인한다. 한글 조사 한 글자 = 3바이트. */
static int josa_variant(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);

    if (la == lb + 3 && strncmp(a, b, lb) == 0 && in_list(a + lb, JOSA))
        return 1;
    if (lb == la + 3 && strncmp(a, b, la) == 0 && in_list(b + la, JOSA))
        return 1;
    return 0;
}

/* 두 벡터에서 기여(가중치 곱)가 큰 낱말을 근거로 고른다. */
typedef struct { float c; const char *text; int bigram; } Contrib;   /* 기여도, 낱말, 두 글자 묶음인지 */

/* qsort 비교: 기여도 내림차순 */
static int contrib_desc(const void *a, const void *b)
{
    float x = ((const Contrib *)a)->c, y = ((const Contrib *)b)->c;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* 게시물 벡터와 학과 벡터의 공통 낱말 중 내적에 크게 기여한 것을 hit->terms 에 담는다.
 * ("왜 이 학과에 추천됐는가?" 를 사람이 이해할 수 있게 보여 주는 설명 기능) */
static void explain(const Vec *post, const Vec *dept, MlHit *hit)
{
    Contrib *cs = (Contrib *)malloc((size_t)(post->n ? post->n : 1) * sizeof *cs);
    int i = 0, j = 0, n = 0, k;

    hit->nterms = 0;
    if (!cs)
        return;
    /* vec_dot 과 같은 병합 방식으로 공통 낱말과 그 기여도(가중치 곱)를 모은다 */
    while (i < post->n && j < dept->n) {
        if (post->t[i].key == dept->t[j].key) {
            cs[n].c = post->t[i].w * dept->t[j].w;
            cs[n].text = post->t[i].text;
            cs[n].bigram = (post->t[i].kind == 'b');
            n++;
            i++; j++;
        } else if (post->t[i].key < dept->t[j].key) {
            i++;
        } else {
            j++;
        }
    }
    qsort(cs, (size_t)n, sizeof *cs, contrib_desc);

    /* 근거는 온전한 낱말로 보여준다. 겹친 낱말이 하나도 없을 때만 두 글자 묶음을 쓴다.
     * pass 0 = 낱말만, pass 1 = (낱말이 하나도 없었을 때) 두 글자 묶음 */
    {
        int pass;
        for (pass = 0; pass < 2 && hit->nterms == 0; pass++) {
            for (k = 0; k < n && hit->nterms < ML_MAX_TERMS; k++) {
                const char *show = cs[k].text;
                int dup = 0, t;

                if (cs[k].bigram != pass)
                    continue;
                /* 두 글자 묶음은 그 글자가 든 게시물 쪽 낱말로 바꿔 보여준다 (디자 -> 시각디자인). */
                if (cs[k].bigram) {
                    for (t = 0; t < post->n; t++)
                        if (post->t[t].kind == 'w' && strstr(post->t[t].text, cs[k].text)) {
                            show = post->t[t].text;
                            break;
                        }
                }
                /* 이미 고른 근거와 사실상 같은 말이면 건너뛴다 */
                for (t = 0; t < hit->nterms; t++)
                    if (_stricmp(hit->terms[t], show) == 0 ||  /* ai 와 AI 는 같은 말 */
                        josa_variant(hit->terms[t], show))
                        dup = 1;
                if (dup)
                    continue;
                str_copy(hit->terms[hit->nterms], sizeof hit->terms[0], show);
                hit->nterms++;
            }
        }
    }
    free(cs);
}

/* qsort 비교: 점수 내림차순, 같으면 id 큰 것(최신) 먼저 */
static int hit_desc(const void *a, const void *b)
{
    const MlHit *x = (const MlHit *)a, *y = (const MlHit *)b;
    if (x->score != y->score)
        return y->score - x->score;
    return x->id < y->id ? 1 : -1;     /* 같은 점수면 최신 글(큰 번호) 먼저 */
}

/* 학과 하나에 대해 모든 게시물의 점수를 매겨 순위를 낸다 (ml.h 참고).
 * 공개 함수들은 모두 같은 틀이다: 잠금 → (필요시) 학습 → 계산 → 잠금 해제.
 * 모델 전체를 잠그므로 학습 중에 다른 요청이 반쯤 바뀐 모델을 읽는 일이 없다. */
int ml_rank_posts(unsigned department_id, int min_score, MlHit *out, int max, MlInfo *info)
{
    int di = -1, i, n = 0;   /* di: 학과 인덱스 */
    MlHit *all;              /* 기준을 넘은 모든 후보 */

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!ensure_model(0)) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    fill_info(info);

    for (i = 0; i < g_model.ndepts; i++)
        if (g_model.depts[i].id == department_id)
            di = i;
    all = (MlHit *)calloc((size_t)(g_model.nposts ? g_model.nposts : 1), sizeof *all);
    if (di < 0 || !all) {
        free(all);
        LeaveCriticalSection(&g_lock);
        return di < 0 ? 0 : -1;   /* 없는 학과면 결과 0건 (오류 아님), 메모리 부족이면 -1 */
    }

    /* 유사도 표의 di 열을 훑는다 */
    for (i = 0; i < g_model.nposts; i++) {
        int s = to_score(g_model.cos[i * g_model.ndepts + di]);
        if (s >= min_score) {
            all[n].id = (unsigned)i;            /* 일단 색인을 담고 아래에서 바꾼다 */
            all[n].score = s;
            all[n].prob = to_prob(g_model.cos[i * g_model.ndepts + di]);
            n++;
        }
    }
    qsort(all, (size_t)n, sizeof *all, hit_desc);
    if (n > max)
        n = max;
    /* 상위 max 개만: 인덱스 → 실제 게시물 id, 근거 키워드 채우기 (설명은 비싸므로 최종 결과에만) */
    for (i = 0; i < n; i++) {
        int pi = (int)all[i].id;
        out[i] = all[i];
        out[i].id = g_model.posts[pi].id;
        explain(&g_model.posts[pi].v, &g_model.depts[di].v, &out[i]);
    }
    free(all);
    LeaveCriticalSection(&g_lock);
    return n;
}

/* 게시물 하나에 대해 모든 학과의 순위 (ml_rank_posts 와 방향만 반대: 유사도 표의 pi 행을 훑는다) */
int ml_rank_departments(unsigned post_id, int min_score, MlHit *out, int max, MlInfo *info)
{
    int pi = -1, i, n = 0;
    MlHit *all;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!ensure_model(0)) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    fill_info(info);

    for (i = 0; i < g_model.nposts; i++)
        if (g_model.posts[i].id == post_id)
            pi = i;
    all = (MlHit *)calloc((size_t)g_model.ndepts, sizeof *all);
    if (pi < 0 || !all) {
        free(all);
        LeaveCriticalSection(&g_lock);
        return pi < 0 ? 0 : -1;
    }

    for (i = 0; i < g_model.ndepts; i++) {
        int s = to_score(g_model.cos[pi * g_model.ndepts + i]);
        if (s >= min_score) {
            all[n].id = (unsigned)i;
            all[n].score = s;
            all[n].prob = to_prob(g_model.cos[pi * g_model.ndepts + i]);
            n++;
        }
    }
    qsort(all, (size_t)n, sizeof *all, hit_desc);
    if (n > max)
        n = max;
    for (i = 0; i < n; i++) {
        int di = (int)all[i].id;
        out[i] = all[i];
        out[i].id = g_model.depts[di].id;
        explain(&g_model.posts[pi].v, &g_model.depts[di].v, &out[i]);
    }
    free(all);
    LeaveCriticalSection(&g_lock);
    return n;
}

/* qsort 비교: 확률 내림차순 */
static int prob_desc(const void *a, const void *b)
{
    double x = ((const MlHit *)a)->prob, y = ((const MlHit *)b)->prob;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* 저장 전 글의 학과별 확률 (ml.h 참고). 모델에 없는 새 글이므로 벡터를 그 자리에서 만든다. */
int ml_predict_departments(const char *title, const char *body, MlHit *out, int max, MlInfo *info)
{
    Vec v = { 0 };       /* 새 글의 TF-IDF 벡터 */
    float *emb = NULL;   /* 새 글의 임베딩 */
    MlHit *all;
    char *text;
    int i, n;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!ensure_model(0)) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    fill_info(info);

    /* 학습 때와 똑같이 벡터로 만든다: 제목 2배 + 본문, 학습 말뭉치의 IDF */
    tokenize(&v, title, TITLE_WEIGHT);
    tokenize(&v, body, 1.0f);
    vec_compact(&v);
    apply_tfidf(&v, g_model.df, g_model.ndf, g_model.ndocs);

    if (g_model.dim) {
        text = join3(title, body, NULL);
        /* 차원이 모델과 다르면(서비스 모델이 바뀐 경우 등) 임베딩을 쓰지 않는다 */
        if (text && embed_texts("passage", (const char *const *)&text, 1, &emb, NULL, 0) != g_model.dim) {
            free(emb);
            emb = NULL;
            log_warn("예측: 임베딩을 구하지 못해 TF-IDF 쪽만으로 매깁니다.");
        }
        free(text);
    }

    all = (MlHit *)calloc((size_t)g_model.ndepts, sizeof *all);
    if (!all) {
        free(emb);
        vec_free(&v);
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    for (i = 0; i < g_model.ndepts; i++) {
        /* 임베딩이 없으면 평균(mu)으로 두어 그 몫은 0 이 된다. */
        double e = emb ? dense_dot(emb, &g_model.demb[(size_t)i * g_model.dim], g_model.dim) : g_model.mu;
        double s = hybrid(vec_dot(&v, &g_model.depts[i].v), e, g_model.mu, g_model.dim);

        all[i].id = (unsigned)i;
        all[i].score = to_score(s);
        all[i].prob = to_prob(s);
    }
    /* 최소 점수 거르기 없이 모든 학과를 확률순으로 */
    qsort(all, (size_t)g_model.ndepts, sizeof *all, prob_desc);
    n = g_model.ndepts < max ? g_model.ndepts : max;
    for (i = 0; i < n; i++) {
        int di = (int)all[i].id;
        out[i] = all[i];
        out[i].id = g_model.depts[di].id;
        explain(&v, &g_model.depts[di].v, &out[i]);
    }
    free(all);
    free(emb);
    vec_free(&v);
    LeaveCriticalSection(&g_lock);
    return n;
}

/* 강제 재학습 */
int ml_retrain(MlInfo *info)
{
    int ok;

    ensure_init();
    EnterCriticalSection(&g_lock);
    ok = ensure_model(1);
    fill_info(info);
    LeaveCriticalSection(&g_lock);
    return ok;
}

/* ============================================================ 개인 맞춤 추천 */

#define PERSONAL_WEIGHT 0.7f       /* 학과 벡터에 내 관심사 벡터를 이만큼 더한다 */

/* 개인 맞춤 게시물 순위 (ml.h 참고).
 * 질의 벡터 q = normalize(학과 벡터 + 0.7 x 내 관심사 벡터) 를 만들어 모든 게시물과 비교한다.
 * 게시물-학과 유사도 표(cos)를 쓰지 않고 그 자리에서 다시 계산한다(질의가 사람마다 다르므로). */
int ml_rank_posts_personal(unsigned department_id, const char *profile_text, int min_score,
                           MlHit *out, int max, MlInfo *info)
{
    int di = -1, i, n = 0;
    Vec q = { 0 }, u = { 0 };   /* q: 최종 질의, u: 내 관심사 */
    float *qemb = NULL;         /* 임베딩 공간의 질의 */
    MlHit *all;

    if (!profile_text || !profile_text[0])
        return ml_rank_posts(department_id, min_score, out, max, info);

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!ensure_model(0)) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    fill_info(info);
    for (i = 0; i < g_model.ndepts; i++)
        if (g_model.depts[i].id == department_id)
            di = i;
    if (di < 0) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    /* 질의 = 학과 벡터 + 0.7 x 내 관심사(키워드·자기소개) 벡터
     * 관심사 글도 학습 말뭉치의 df 표로 TF-IDF 를 매겨야 게시물 벡터와 같은 척도가 된다. */
    tokenize(&u, profile_text, 1.0f);
    vec_compact(&u);
    apply_tfidf(&u, g_model.df, g_model.ndf, g_model.ndocs);
    vec_add_scaled(&q, &g_model.depts[di].v, 1.0f);
    vec_add_scaled(&q, &u, PERSONAL_WEIGHT);
    vec_normalize(&q);
    vec_free(&u);

    /* 임베딩 질의도 같은 식. 관심사 임베딩을 못 구하면 학과 임베딩만 쓴다. */
    if (g_model.dim) {
        float *uemb = NULL;
        int k, udim = embed_texts("query", &profile_text, 1, &uemb, NULL, 0);

        qemb = (float *)malloc((size_t)g_model.dim * sizeof *qemb);
        if (qemb) {
            memcpy(qemb, &g_model.demb[(size_t)di * g_model.dim], (size_t)g_model.dim * sizeof *qemb);
            if (udim == g_model.dim)
                for (k = 0; k < udim; k++)
                    qemb[k] += PERSONAL_WEIGHT * uemb[k];
            dense_normalize(qemb, g_model.dim);
        }
        free(uemb);
    }

    all = (MlHit *)calloc((size_t)(g_model.nposts ? g_model.nposts : 1), sizeof *all);
    if (!all || (g_model.dim && !qemb)) {
        free(all);
        free(qemb);
        vec_free(&q);
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    for (i = 0; i < g_model.nposts; i++) {
        double e = qemb ? dense_dot(&g_model.pemb[(size_t)i * g_model.dim], qemb, g_model.dim) : 0;
        double sim = hybrid(vec_dot(&g_model.posts[i].v, &q), e, g_model.mu, g_model.dim);
        int s = to_score(sim);
        if (s >= min_score) {
            all[n].id = (unsigned)i;
            all[n].score = s;
            all[n].prob = to_prob(sim);
            n++;
        }
    }
    qsort(all, (size_t)n, sizeof *all, hit_desc);
    if (n > max)
        n = max;
    for (i = 0; i < n; i++) {
        int pi = (int)all[i].id;
        out[i] = all[i];
        out[i].id = g_model.posts[pi].id;
        explain(&g_model.posts[pi].v, &q, &out[i]);   /* 근거도 개인 질의 기준 */
    }
    free(all);
    free(qemb);
    vec_free(&q);
    LeaveCriticalSection(&g_lock);
    return n;
}

/* ============================================================ 회원 · 모집글 매칭
 *
 * 회원 문서 = 고른 키워드(가중 3, 키워드 자체를 뜻하는 't:' 토큰 + 그 낱말) + 자기소개 + 학과 이름(0.7).
 * 모집글 문서 = 찾는 키워드(가중 3) + 제목(2) + 본문.
 * 두 종류를 한 말뭉치로 TF-IDF 를 매기고 코사인 유사도로 비교한다.
 * 많은 사람이 고른 키워드(예: 경험쌓기)는 IDF 가 낮아 저절로 덜 중요해진다.
 *
 * 마음에 들어요 (recruit_likes):
 *   - 좋아한 모집글의 제목·키워드를 회원 문서에 0.5 배로 넣는다 (무엇에 끌리는지가 곧 관심사).
 *   - 회원끼리: 같은 모집글을 좋아한 정도(Jaccard) x 0.15, 상대 모집글을 좋아했으면 +0.10 을
 *     유사도에 더한다 (협업 필터링). 근거에는 "내 모집글에 ♥" 같은 말로 보여 준다.
 *
 * 이 회원 모델(g_people)은 게시물 모델(g_model)과 별개로 학습하지만 같은 g_lock 을 쓴다. */

#define TAG_WEIGHT       3.0f   /* 고른 키워드는 자기소개 낱말보다 훨씬 확실한 신호 */
#define LIKE_TEXT_WEIGHT 0.5f   /* ♥ 누른 모집글 글의 가중치 */
#define CO_LIKE_BONUS    0.15   /* 같은 모집글 ♥ 가산점의 최대치 */
#define AUTHOR_BONUS     0.10   /* 서로의 모집글에 ♥ 했을 때 가산점 */

/* 회원 매칭 모델 */
typedef struct {
    int    ready;
    char   signature[240];
    Doc   *users;          /* 학생 회원 문서들 */
    int    nusers;
    Doc   *recs;           /* 모집글 문서들 */
    int    nrecs;
    double tau;            /* 점수 보정 척도 */
    int    dim;            /* 임베딩 차원 (0 = TF-IDF 만) */
    float *uemb, *remb;    /* 회원, 모집글 임베딩 */
    double mu_uu, mu_ur;   /* 회원-회원, 회원-모집글 임베딩 코사인 평균 */
    int   *like_off;       /* [nusers + 1] 회원 i 가 좋아한 모집글 = like_rec[like_off[i] .. like_off[i+1]) */
    int   *like_rec;       /* 모집글 색인, 회원마다 오름차순 */
    int   *rec_author;     /* [nrecs] 작성자의 회원 색인 (회원 모델에 없으면 -1) */
} PeopleModel;

static PeopleModel g_people;

static int find_doc(const Doc *docs, int n, unsigned id);   /* 아래에서 정의 */

/* 회원 모델의 모든 메모리 해제 */
static void people_free(PeopleModel *m)
{
    int i;

    for (i = 0; i < m->nusers; i++) vec_free(&m->users[i].v);
    for (i = 0; i < m->nrecs; i++)  vec_free(&m->recs[i].v);
    free(m->users);
    free(m->recs);
    free(m->uemb);
    free(m->remb);
    free(m->like_off);
    free(m->like_rec);
    free(m->rec_author);
    memset(m, 0, sizeof *m);
}

/* qsort 비교: int 오름차순 */
static int int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

/* 좋아요 관계를 읽는다. 실패해도 좋아요 없이 동작한다.
 * 결과는 CSR(압축 희소 행) 형식으로 저장한다:
 *   like_off[i] ~ like_off[i+1] 구간의 like_rec 값들 = 회원 i 가 좋아한 모집글 인덱스
 * 회원마다 동적 배열을 따로 두지 않고 큰 배열 하나에 이어 붙여 메모리와 할당 횟수를 줄인다. */
static void load_likes(PeopleModel *m)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int i, n = 0, *cnt;   /* cnt[u]: 회원 u 의 ♥ 개수 */

    m->rec_author = (int *)malloc((size_t)(m->nrecs ? m->nrecs : 1) * sizeof *m->rec_author);
    m->like_off = (int *)calloc((size_t)m->nusers + 1, sizeof *m->like_off);
    if (!m->rec_author || !m->like_off)
        return;
    for (i = 0; i < m->nrecs; i++)
        m->rec_author[i] = -1;

    /* 모집글 → 작성자 회원 인덱스 */
    qr = db_query("SELECT id, author_id FROM recruits");
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            int r = find_doc(m->recs, m->nrecs, (unsigned)strtoul(row[0], NULL, 10));
            if (r >= 0)
                m->rec_author[r] = find_doc(m->users, m->nusers, (unsigned)strtoul(row[1], NULL, 10));
        }
        mysql_free_result(qr);
    }

    qr = db_query("SELECT user_id, recruit_id FROM recruit_likes");
    if (!qr)
        return;
    m->like_rec = (int *)malloc((size_t)(mysql_num_rows(qr) + 1) * sizeof *m->like_rec);
    cnt = (int *)calloc((size_t)m->nusers + 1, sizeof *cnt);
    if (m->like_rec && cnt) {
        /* 1) (회원, 모집글) 쌍을 임시 배열에 모으며 회원별 개수를 센다 */
        int *pair_u = (int *)malloc((size_t)(mysql_num_rows(qr) + 1) * sizeof *pair_u);
        int *pair_r = (int *)malloc((size_t)(mysql_num_rows(qr) + 1) * sizeof *pair_r);

        while (pair_u && pair_r && (row = mysql_fetch_row(qr)) != NULL) {
            int u = find_doc(m->users, m->nusers, (unsigned)strtoul(row[0], NULL, 10));
            int r = find_doc(m->recs, m->nrecs, (unsigned)strtoul(row[1], NULL, 10));
            if (u < 0 || r < 0)          /* 관리자가 누른 것 등 모델에 없는 쌍 */
                continue;
            pair_u[n] = u;
            pair_r[n] = r;
            cnt[u]++;
            n++;
        }
        /* 회원별로 묶는다 (계수 정렬)
         * 2) 누적합으로 각 회원 구간의 시작 위치를 정한다 */
        for (i = 0; i < m->nusers; i++)
            m->like_off[i + 1] = m->like_off[i] + cnt[i];
        /* 3) cnt 를 "구간 안에서 다음에 쓸 칸" 으로 재사용해 쌍을 제자리에 배치 */
        memset(cnt, 0, (size_t)m->nusers * sizeof *cnt);
        for (i = 0; i < n; i++)
            m->like_rec[m->like_off[pair_u[i]] + cnt[pair_u[i]]++] = pair_r[i];
        /* 4) 회원마다 모집글 인덱스를 정렬 (social_bonus 의 병합 비교를 위해) */
        for (i = 0; i < m->nusers; i++)
            qsort(&m->like_rec[m->like_off[i]], (size_t)cnt[i], sizeof(int), int_cmp);
        free(pair_u);
        free(pair_r);
    }
    free(cnt);
    mysql_free_result(qr);
}

/* 회원 u 가 좋아한 모집글 목록과 개수 */
static int likes_of(const PeopleModel *m, int u, const int **list)
{
    if (!m->like_off || !m->like_rec) {
        *list = NULL;
        return 0;
    }
    *list = &m->like_rec[m->like_off[u]];
    return m->like_off[u + 1] - m->like_off[u];
}

/* 관계 종류 비트 플래그 (여러 개가 동시에 참일 수 있어 | 로 합친다) */
#define SOCIAL_CO        1     /* 같은 모집글을 좋아함 */
#define SOCIAL_LIKED_ME  2     /* j 가 i 의 모집글을 좋아함 */
#define SOCIAL_I_LIKED   4     /* i 가 j 의 모집글을 좋아함 */

/* 회원 i, j 사이 좋아요 관계로 생기는 가산점. *flags 에 관계를 적는다.
 * Jaccard 유사도 = |A ∩ B| / |A ∪ B| = 공통 / (na + nb - 공통)  (0~1) */
static double social_bonus(const PeopleModel *m, int i, int j, int *flags)
{
    const int *a, *b;
    int na = likes_of(m, i, &a), nb = likes_of(m, j, &b);
    int x = 0, y = 0, common = 0, k;
    double bonus = 0;

    *flags = 0;
    /* 정렬된 두 목록의 교집합 크기 (병합 방식) */
    while (x < na && y < nb) {
        if (a[x] == b[y]) { common++; x++; y++; }
        else if (a[x] < b[y]) x++;
        else y++;
    }
    if (common) {
        bonus += CO_LIKE_BONUS * common / (double)(na + nb - common);
        *flags |= SOCIAL_CO;
    }
    /* 서로의 모집글에 ♥ 했는지 */
    if (m->rec_author) {
        for (k = 0; k < nb; k++)
            if (m->rec_author[b[k]] == i) { *flags |= SOCIAL_LIKED_ME; break; }
        for (k = 0; k < na; k++)
            if (m->rec_author[a[k]] == j) { *flags |= SOCIAL_I_LIKED; break; }
    }
    if (*flags & (SOCIAL_LIKED_ME | SOCIAL_I_LIKED))
        bonus += AUTHOR_BONUS;
    return bonus;
}

/* 회원 i - 회원 j, 회원 i - 모집글 r 의 하이브리드 유사도 (회원끼리는 좋아요 가산점 포함) */
static double people_cos_uu(const PeopleModel *m, int i, int j)
{
    double e = m->dim ? dense_dot(&m->uemb[(size_t)i * m->dim], &m->uemb[(size_t)j * m->dim], m->dim) : 0;
    int flags;
    return hybrid(vec_dot(&m->users[i].v, &m->users[j].v), e, m->mu_uu, m->dim) +
           social_bonus(m, i, j, &flags);
}

/* 근거 키워드 앞에 좋아요 관계를 끼워 넣는다.
 * 칸이 꽉 찼으면 마지막 키워드를 버리고, 나머지를 한 칸씩 뒤로 밀어 맨 앞에 넣는다. */
static void explain_social(const PeopleModel *m, int me, int other, MlHit *hit)
{
    const char *label = NULL;
    int flags, k;

    social_bonus(m, me, other, &flags);
    if (flags & SOCIAL_LIKED_ME)     label = "내 모집글에 ♥";
    else if (flags & SOCIAL_I_LIKED) label = "내가 ♥한 모집글 작성자";
    else if (flags & SOCIAL_CO)      label = "같은 모집글에 ♥";
    if (!label)
        return;
    if (hit->nterms == ML_MAX_TERMS)
        hit->nterms--;
    for (k = hit->nterms; k > 0; k--)
        memcpy(hit->terms[k], hit->terms[k - 1], sizeof hit->terms[0]);
    str_copy(hit->terms[0], sizeof hit->terms[0], label);
    hit->nterms++;
}

/* 회원 i - 모집글 r 유사도 (가산점 없음) */
static double people_cos_ur(const PeopleModel *m, int i, int r)
{
    double e = m->dim ? dense_dot(&m->uemb[(size_t)i * m->dim], &m->remb[(size_t)r * m->dim], m->dim) : 0;
    return hybrid(vec_dot(&m->users[i].v, &m->recs[r].v), e, m->mu_ur, m->dim);
}

/* 회원 모델용 데이터 서명 (current_signature 와 같은 아이디어) */
static void people_signature(char *out, size_t outsz)
{
    MYSQL_RES *qr = db_query(
        "SELECT CONCAT((SELECT COUNT(*) FROM users), '/', "
        "              (SELECT IFNULL(SUM(CRC32(CONCAT(bio, IFNULL(department_id, 0)))), 0) FROM users), '/', "
        "              (SELECT COUNT(*) FROM user_interests), '/', "
        "              (SELECT IFNULL(SUM(user_id * 131 + tag_id), 0) FROM user_interests), '/', "
        "              (SELECT COUNT(*) FROM recruits), '/', "
        "              (SELECT IFNULL(SUM(CRC32(CONCAT(title, LENGTH(body)))), 0) FROM recruits), '/', "
        "              (SELECT IFNULL(SUM(recruit_id * 131 + tag_id), 0) FROM recruit_tags), '/', "
        "              (SELECT COUNT(*) FROM recruit_likes), '/', "
        "              (SELECT IFNULL(SUM(recruit_id * 131 + user_id), 0) FROM recruit_likes))");
    MYSQL_ROW row;

    out[0] = '\0';
    if (!qr)
        return;
    row = mysql_fetch_row(qr);
    if (row && row[0])
        str_copy(out, outsz, row[0]);
    mysql_free_result(qr);
}

/* "영상,기획,팀장" 처럼 쉼표로 이은 키워드를 토큰으로 넣는다.
 * 키워드마다 두 가지를 넣는다:
 *   't:키워드' 토큰 (가중 3) - "이 키워드를 골랐다" 는 사실 자체
 *   키워드를 토큰화한 낱말들 (가중 1) - 자기소개·본문의 같은 낱말과도 맞물리게 */
static void add_tags(Vec *v, const char *csv)
{
    char buf[1024], *p, *next;

    str_copy(buf, sizeof buf, csv ? csv : "");
    for (p = buf; p && *p; p = next) {
        next = strchr(p, ',');
        if (next)
            *next++ = '\0';
        if (*p) {
            add_token(v, "t:", p, TAG_WEIGHT);
            tokenize(v, p, 1.0f);
        }
    }
}

/* *texts 에 임베딩할 글을 함께 담는다 (호출자가 free_texts).
 * sql 의 결과 열 순서:
 *   회원(is_user=1):  id, 학과 이름, 자기소개, 키워드 CSV, 좋아한 모집글 글
 *   모집글(is_user=0): id, 제목, 본문, 키워드 CSV */
static Doc *load_docs(const char *sql, int is_user, int *count, char ***texts)
{
    MYSQL_RES *qr = db_query(sql);
    MYSQL_ROW row;
    my_ulonglong rows;
    Doc *docs;

    *count = 0;
    *texts = NULL;
    if (!qr)
        return NULL;
    rows = mysql_num_rows(qr);
    docs = (Doc *)calloc((size_t)(rows ? rows : 1), sizeof *docs);
    *texts = (char **)calloc((size_t)(rows ? rows : 1), sizeof **texts);
    if (!*texts) {
        free(docs);
        docs = NULL;
    }
    while (docs && (row = mysql_fetch_row(qr)) != NULL) {
        Doc *d = &docs[*count];
        /* 회원: 학과 / 키워드 / 자기소개 (+ 좋아한 모집글), 모집글: 제목 / 키워드 / 본문 */
        char *text = join3(row[1], row[3], row[2]);
        int liked = is_user && mysql_num_fields(qr) > 4 && row[4] && row[4][0];

        if (liked && text) {
            Buf t;
            buf_init(&t);
            buf_puts(&t, text);
            buf_puts(&t, "\n관심 있게 본 모집글: ");
            buf_puts(&t, row[4]);
            free(text);
            text = t.data;
        }
        (*texts)[(*count)++] = text;
        d->id = (unsigned)strtoul(row[0], NULL, 10);
        if (is_user) {            /* id, 학과, 자기소개, 키워드, 좋아한 모집글 */
            tokenize(&d->v, row[1], 0.7f);
            tokenize(&d->v, row[2], 1.0f);
            add_tags(&d->v, row[3]);
            if (liked)
                tokenize(&d->v, row[4], LIKE_TEXT_WEIGHT);
        } else {                  /* id, 제목, 본문, 키워드 */
            tokenize(&d->v, row[1], TITLE_WEIGHT);
            tokenize(&d->v, row[2], 1.0f);
            add_tags(&d->v, row[3]);
        }
        vec_compact(&d->v);
    }
    mysql_free_result(qr);
    return docs;
}

/* 회원 매칭 모델 학습. 흐름은 train 과 비슷하다:
 * 문서 읽기 → ♥ 관계 → 임베딩 → TF-IDF → 점수 척도(tau) */
static int people_train(PeopleModel *m)
{
    Df *df = NULL;
    int ndf = 0, i, j, total = 0;
    char **utexts = NULL, **rtexts = NULL;

    memset(m, 0, sizeof *m);
    people_signature(m->signature, sizeof m->signature);

    /* 학생 회원만 (관리자 제외). 키워드는 LEFT JOIN + GROUP_CONCAT 으로 한 줄에 모은다 */
    m->users = load_docs(
        "SELECT u.id, IFNULL(d.name, ''), u.bio, "
        "       IFNULL(GROUP_CONCAT(t.name ORDER BY t.sort SEPARATOR ','), ''), "
        /* 좋아한 모집글의 제목과 찾는 키워드 */
        "       IFNULL((SELECT GROUP_CONCAT(CONCAT(r.title, ' ', "
        "                 IFNULL((SELECT GROUP_CONCAT(t2.name SEPARATOR ' ') FROM recruit_tags rt2 "
        "                         JOIN interest_tags t2 ON t2.id = rt2.tag_id "
        "                         WHERE rt2.recruit_id = r.id), '')) SEPARATOR ' / ') "
        "               FROM recruit_likes l JOIN recruits r ON r.id = l.recruit_id "
        "               WHERE l.user_id = u.id), '') "
        "FROM users u LEFT JOIN departments d ON d.id = u.department_id "
        "LEFT JOIN user_interests ui ON ui.user_id = u.id "
        "LEFT JOIN interest_tags t ON t.id = ui.tag_id "
        "WHERE u.role = 'student' GROUP BY u.id ORDER BY u.id", 1, &m->nusers, &utexts);
    m->recs = load_docs(
        "SELECT r.id, r.title, r.body, "
        "       IFNULL(GROUP_CONCAT(t.name ORDER BY t.sort SEPARATOR ','), '') "
        "FROM recruits r LEFT JOIN recruit_tags rt ON rt.recruit_id = r.id "
        "LEFT JOIN interest_tags t ON t.id = rt.tag_id "
        "GROUP BY r.id ORDER BY r.id", 0, &m->nrecs, &rtexts);
    if (!m->users || !m->recs) {
        free_texts(utexts, m->nusers);
        free_texts(rtexts, m->nrecs);
        people_free(m);
        return 0;
    }
    load_likes(m);

    /* 딥러닝 임베딩. 회원끼리는 대칭 비교라 둘 다 query 로 넣는다.
     * (passage/query 구분은 "짧은 질문 ↔ 긴 문서" 처럼 비대칭일 때 쓰는 것이다) */
    {
        int udim = 0, rdim = 0;

        m->uemb = embed_all("query", utexts, m->nusers, &udim, NULL, 0);
        if (m->uemb && m->nrecs > 0)
            m->remb = embed_all("query", rtexts, m->nrecs, &rdim, NULL, 0);
        else
            rdim = udim;          /* 모집글이 없으면 회원 임베딩만으로 충분하다 */
        if (!m->uemb || udim != rdim || (m->nrecs > 0 && !m->remb)) {
            free(m->uemb);
            free(m->remb);
            m->uemb = m->remb = NULL;
        } else {
            m->dim = udim;
        }
    }
    free_texts(utexts, m->nusers);
    free_texts(rtexts, m->nrecs);
    /* 임베딩 코사인 평균 (hybrid 의 기준점): 회원-회원(자기 자신 제외), 회원-모집글 */
    if (m->dim) {
        double s = 0;
        long long cnt = 0;

        for (i = 0; i < m->nusers; i++)
            for (j = 0; j < m->nusers; j++)
                if (i != j) {
                    s += dense_dot(&m->uemb[(size_t)i * m->dim], &m->uemb[(size_t)j * m->dim], m->dim);
                    cnt++;
                }
        m->mu_uu = cnt ? s / cnt : 0;
        s = 0;
        cnt = 0;
        for (i = 0; i < m->nusers; i++)
            for (j = 0; j < m->nrecs; j++) {
                s += dense_dot(&m->uemb[(size_t)i * m->dim], &m->remb[(size_t)j * m->dim], m->dim);
                cnt++;
            }
        m->mu_ur = cnt ? s / cnt : m->mu_uu;
    }

    /* 문서 빈도와 TF-IDF (train 과 같은 방법, 회원 + 모집글을 한 말뭉치로) */
    for (i = 0; i < m->nusers; i++) total += m->users[i].v.n;
    for (i = 0; i < m->nrecs; i++)  total += m->recs[i].v.n;
    df = (Df *)malloc((size_t)(total ? total : 1) * sizeof *df);
    if (!df) {
        people_free(m);
        return 0;
    }
    for (i = 0; i < m->nusers; i++)
        for (j = 0; j < m->users[i].v.n; j++)
            df[ndf].key = m->users[i].v.t[j].key, df[ndf++].df = 1;
    for (i = 0; i < m->nrecs; i++)
        for (j = 0; j < m->recs[i].v.n; j++)
            df[ndf].key = m->recs[i].v.t[j].key, df[ndf++].df = 1;
    qsort(df, (size_t)ndf, sizeof *df, df_cmp);
    {
        int k = 0;
        for (i = 1; i < ndf; i++) {
            if (df[i].key == df[k].key)
                df[k].df++;
            else
                df[++k] = df[i];
        }
        ndf = ndf ? k + 1 : 0;
    }
    for (i = 0; i < m->nusers; i++)
        apply_tfidf(&m->users[i].v, df, ndf, m->nusers + m->nrecs);
    for (i = 0; i < m->nrecs; i++)
        apply_tfidf(&m->recs[i].v, df, ndf, m->nusers + m->nrecs);
    free(df);   /* 회원 모델은 새 질의를 벡터화할 일이 없어 df 표를 남기지 않는다 */

    /* 점수 보정: 회원마다 가장 비슷한 다른 회원의 유사도를 모아, 그 중앙값을 70점으로.
     * (같은 성향 회원이 몇 쌍만 있어도 상위 지점은 1 에 가까워져 다른 점수가 지나치게 낮아진다)
     * 70 = 100 * (1 - exp(-p50 / tau))  ->  tau = p50 / ln(1 / 0.3) */
    {
        size_t nu = m->nusers > 0 ? (size_t)m->nusers : 1;
        float *best = (float *)calloc(nu, sizeof *best);
        double p50 = 0.3;

        if (best && m->nusers > 1) {
            for (i = 0; i < m->nusers; i++)
                for (j = 0; j < m->nusers; j++)
                    if (i != j) {
                        float c = (float)people_cos_uu(m, i, j);
                        if (c > best[i])
                            best[i] = c;
                    }
            qsort(best, nu, sizeof *best, float_desc);
            p50 = best[m->nusers / 2];   /* 중앙값 */
        }
        free(best);
        if (p50 < 0.05)
            p50 = 0.05;
        m->tau = p50 / log(1.0 / 0.3);
    }

    m->ready = 1;
    log_info("회원 매칭 모델 학습 (%s): 회원 %d, 모집글 %d, 좋아요 %d, tau %.3f",
             m->dim ? "임베딩 + TF-IDF" : "TF-IDF", m->nusers, m->nrecs,
             m->like_off ? m->like_off[m->nusers] : 0, m->tau);   /* like_off 마지막 값 = 전체 ♥ 수 */
    return 1;
}

/* 회원 모델이 최신인지 확인하고 아니면 다시 학습 (g_lock 을 잡은 상태에서 부른다) */
static int people_ensure(void)
{
    char sig[240];

    if (g_people.ready) {
        people_signature(sig, sizeof sig);
        if (strcmp(sig, g_people.signature) == 0 && (g_people.dim || !embed_available()))
            return 1;
    }
    people_free(&g_people);
    return people_train(&g_people);
}

/* 회원 모델의 유사도 → 0~100 점수 (to_score 와 같은 식, tau 만 다름) */
static int people_score(double cos)
{
    double s;

    if (cos <= 0 || g_people.tau <= 0)
        return 0;
    s = 100.0 * (1.0 - exp(-cos / g_people.tau));
    return (int)(s + 0.5);
}

/* id 로 문서 배열에서 인덱스 찾기 (선형 탐색). 없으면 -1. */
static int find_doc(const Doc *docs, int n, unsigned id)
{
    int i;
    for (i = 0; i < n; i++)
        if (docs[i].id == id)
            return i;
    return -1;
}

/* 나와 비슷한 학생 순위 (ml.h 참고) */
int ml_match_users(unsigned user_id, int min_score, MlHit *out, int max)
{
    int me, i, n = 0;   /* me: 내 인덱스 */
    MlHit *all;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!people_ensure()) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    me = find_doc(g_people.users, g_people.nusers, user_id);
    /* 모델에 없거나(관리자) 벡터가 비었으면(키워드·자기소개가 없음) 비교할 근거가 없다 */
    if (me < 0 || g_people.users[me].v.n == 0) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    all = (MlHit *)calloc((size_t)g_people.nusers, sizeof *all);
    if (!all) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    for (i = 0; i < g_people.nusers; i++) {
        int s;
        if (i == me)        /* 자기 자신 제외 */
            continue;
        s = people_score(people_cos_uu(&g_people, me, i));
        if (s >= min_score) {
            all[n].id = (unsigned)i;
            all[n].score = s;
            n++;
        }
    }
    qsort(all, (size_t)n, sizeof *all, hit_desc);
    if (n > max)
        n = max;
    for (i = 0; i < n; i++) {
        int ui = (int)all[i].id;
        out[i] = all[i];
        out[i].id = g_people.users[ui].id;
        explain(&g_people.users[ui].v, &g_people.users[me].v, &out[i]);   /* 겹친 키워드·낱말 */
        explain_social(&g_people, me, ui, &out[i]);                        /* ♥ 관계 (있으면 맨 앞) */
    }
    free(all);
    LeaveCriticalSection(&g_lock);
    return n;
}

/* 모집글마다 나와 맞는 정도 (ml.h 참고). 반환: 1 = 점수 매김, 0 = 내가 모델에 없음, -1 = 오류 */
int ml_score_recruits(unsigned user_id, const unsigned *recruit_ids, int n, MlHit *out)
{
    int me, i;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!people_ensure()) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    me = find_doc(g_people.users, g_people.nusers, user_id);
    for (i = 0; i < n; i++) {
        int ri = find_doc(g_people.recs, g_people.nrecs, recruit_ids[i]);

        memset(&out[i], 0, sizeof out[i]);   /* 점수 0, 근거 없음으로 시작 */
        out[i].id = recruit_ids[i];
        if (me < 0 || ri < 0)
            continue;
        out[i].score = people_score(people_cos_ur(&g_people, me, ri));
        explain(&g_people.recs[ri].v, &g_people.users[me].v, &out[i]);
    }
    LeaveCriticalSection(&g_lock);
    return me < 0 ? 0 : 1;
}
