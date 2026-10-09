/* 자체 추천 모델 (딥러닝 문장 임베딩 + TF-IDF + Rocchio). 설명은 ml.h 참고. */
#include "ml.h"
#include "common.h"
#include "db.h"
#include "embed.h"

#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TITLE_WEIGHT   2.0f
#define BIGRAM_WEIGHT  0.6f
#define ROCCHIO_ALPHA  1.0f
#define ROCCHIO_BETA   0.75f
#define CENTROID_TOP   40      /* 학습한 게시물 특징은 가중치 상위 이만큼만 쓴다 (잡음 억제) */
#define SHRINK_K       1.5f    /* 학습 데이터가 적을수록 반영을 줄인다: beta * w / (w + k) */
#define TERM_TEXT_MAX  40
#define EMB_WEIGHT     0.7     /* 최종 유사도 = 0.7 x 임베딩 + 0.3 x TF-IDF (임베딩이 있을 때) */

/* ============================================================ 밀집 벡터 (임베딩) */

static void dense_normalize(float *v, int dim)
{
    double s = 0;
    int i;

    for (i = 0; i < dim; i++)
        s += (double)v[i] * v[i];
    if (s <= 0)
        return;
    s = sqrt(s);
    for (i = 0; i < dim; i++)
        v[i] = (float)(v[i] / s);
}

static double dense_dot(const float *a, const float *b, int dim)
{
    double s = 0;
    int i;

    for (i = 0; i < dim; i++)
        s += (double)a[i] * b[i];
    return s;
}

/* 트랜스포머 임베딩의 코사인은 관계없는 글끼리도 0.7~0.8 쯤 나온다.
 * 전체 평균(mu) 아래는 0, 위는 0~1 로 펴서 TF-IDF 코사인과 같은 척도로 맞춘다. */
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

static void free_texts(char **texts, int n)
{
    int i;

    if (!texts)
        return;
    for (i = 0; i < n; i++)
        free(texts[i]);
    free(texts);
}

/* a + "\n" + b + "\n" + c 를 새로 만든다 (NULL 은 빈 문자열). */
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

typedef struct {
    uint32_t key;
    float    w;
    char     kind;                 /* 'w' 낱말, 'b' 두 글자 묶음 */
    char     text[TERM_TEXT_MAX];
} Term;

typedef struct {
    Term *t;
    int   n, cap;
} Vec;

static void vec_free(Vec *v)
{
    free(v->t);
    v->t = NULL;
    v->n = v->cap = 0;
}

static int vec_push(Vec *v, uint32_t key, float w, char kind, const char *text)
{
    if (v->n == v->cap) {
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

static int term_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Term *)a)->key, y = ((const Term *)b)->key;
    return x < y ? -1 : x > y;
}

/* 키 순으로 정렬하고 같은 키를 합친다. */
static void vec_compact(Vec *v)
{
    int i, k = 0;

    if (v->n == 0)
        return;
    qsort(v->t, (size_t)v->n, sizeof *v->t, term_cmp);
    for (i = 1; i < v->n; i++) {
        if (v->t[i].key == v->t[k].key)
            v->t[k].w += v->t[i].w;
        else
            v->t[++k] = v->t[i];
    }
    v->n = k + 1;
}

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

/* out += scale * v  (둘 다 정렬된 상태를 유지한다) */
static void vec_add_scaled(Vec *out, const Vec *v, float scale)
{
    int i;

    for (i = 0; i < v->n; i++)
        vec_push(out, v->t[i].key, v->t[i].w * scale, v->t[i].kind, v->t[i].text);
    vec_compact(out);
}

static int weight_desc(const void *a, const void *b)
{
    float x = ((const Term *)a)->w, y = ((const Term *)b)->w;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* 가중치가 큰 k 개만 남긴다 (키 순 정렬은 유지). */
static void vec_keep_top(Vec *v, int k)
{
    if (v->n <= k)
        return;
    qsort(v->t, (size_t)v->n, sizeof *v->t, weight_desc);
    v->n = k;
    qsort(v->t, (size_t)v->n, sizeof *v->t, term_cmp);
}

static double vec_dot(const Vec *a, const Vec *b)
{
    int i = 0, j = 0;
    double s = 0;

    while (i < a->n && j < b->n) {
        if (a->t[i].key == b->t[j].key)
            s += (double)a->t[i++].w * b->t[j++].w;
        else if (a->t[i].key < b->t[j].key)
            i++;
        else
            j++;
    }
    return s;
}

/* ============================================================ 토큰화 */

static uint32_t fnv1a(const char *prefix, const char *s)
{
    uint32_t h = 2166136261u;

    for (; *prefix; prefix++) { h ^= (unsigned char)*prefix; h *= 16777619u; }
    for (; *s; s++)           { h ^= (unsigned char)*s;      h *= 16777619u; }
    return h;
}

/* 공지에 거의 늘 나오는 낱말. 학과를 가르는 데 도움이 되지 않는다. */
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
/* 낱말 끝에 붙은 조사 한 글자는 떼어 낸 형태도 함께 쓴다. */
static const char *JOSA[] = {
    "을", "를", "이", "가", "은", "는", "의", "에", "로", "과", "와", "도", "만", NULL
};

/* 같은 뜻으로 보는 낱말. 왼쪽이 나오면 오른쪽 낱말도 함께 센다 (0.8 배). */
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

static const char *synonym_of(const char *word)
{
    int i;

    for (i = 0; SYNONYMS[i][0]; i++)
        if (strcmp(word, SYNONYMS[i][0]) == 0)
            return SYNONYMS[i][1];
    return NULL;
}

static int in_list(const char *s, const char **list)
{
    for (; *list; list++)
        if (strcmp(s, *list) == 0)
            return 1;
    return 0;
}

/* UTF-8 한 글자를 읽어 코드포인트를 돌려준다. len 에 바이트 수. */
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

#define CLS_NONE   0
#define CLS_HANGUL 1
#define CLS_LATIN  2

static int char_class(unsigned cp)
{
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return CLS_HANGUL;
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9'))
        return CLS_LATIN;
    return CLS_NONE;
}

static void add_token(Vec *v, const char *prefix, const char *text, float w)
{
    vec_push(v, fnv1a(prefix, text), w, prefix[0], text);
}

/* 한 낱말(같은 종류 글자가 이어진 구간)을 토큰으로 만든다.
 * start/len 은 원문 안의 바이트 범위, nchars 는 글자 수. */
static void flush_word(Vec *v, const char *start, int len, int nchars, int cls, float w)
{
    char word[64];

    if (len <= 0 || len >= (int)sizeof word)
        return;
    memcpy(word, start, (size_t)len);
    word[len] = '\0';

    if (cls == CLS_LATIN) {
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
        /* 한글 음절은 UTF-8 로 모두 3바이트다. */
        for (i = 0; i + 6 <= len; i += 3) {
            char pair[8];
            memcpy(pair, word + i, 6);
            pair[6] = '\0';
            if (!in_list(pair, STOP_BIGRAMS))
                add_token(v, "b:", pair, w * BIGRAM_WEIGHT);
        }
    }
}

static void tokenize(Vec *v, const char *text, float w)
{
    const unsigned char *s = (const unsigned char *)(text ? text : "");
    const char *word_start = NULL;
    int word_cls = CLS_NONE, nchars = 0;

    for (;;) {
        int len = 1;
        unsigned cp = *s ? utf8_next(s, &len) : 0;
        int cls = *s ? char_class(cp) : CLS_NONE;

        if (cls != word_cls) {
            if (word_cls != CLS_NONE)
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

typedef struct {
    unsigned id;
    Vec      v;
} Doc;

/* 문서 빈도 표 (키 순 정렬) */
typedef struct { uint32_t key; int df; } Df;

typedef struct {
    int      ready;
    char     signature[160];
    Doc     *posts;
    int      nposts;
    Doc     *depts;
    int      ndepts;
    float   *cos;          /* [nposts * ndepts] */
    Df      *df;           /* 개인 맞춤 질의를 같은 기준으로 벡터화하려고 남겨 둔다 */
    int      ndf, ndocs;
    double   tau;
    int      labels;
    char     trained_at[32];
    int      dim;          /* 임베딩 차원. 0 이면 임베딩 없이 TF-IDF 만 */
    float   *pemb;         /* [nposts * dim] 게시물 임베딩 */
    float   *demb;         /* [ndepts * dim] 학과 임베딩 (Rocchio 반영) */
    double   mu;           /* 게시물-학과 임베딩 코사인 평균 (hybrid 참고) */
    char     emb_model[128];
    double   pa, pb;       /* 확률 보정 P = sigmoid(pa * s + pb) */
    int      npos, nneg;   /* 보정에 쓴 양성·음성 쌍 */
} Model;

typedef struct { int pi, di; float w; } Label;

static int float_desc(const void *a, const void *b);

static double sigmoid(double x)
{
    return 1.0 / (1.0 + exp(-x));
}

/* 확률 보정: 유사도 s 를 "이 학과에 맞을 확률" 로 바꾸는 로지스틱 함수의 a, b 를 맞춘다.
 *   양성 = 정답 쌍 (가중치 = 체크 1.0, 댓글·♥ 0.5)
 *   음성 = 정답이 하나라도 있는 글의 나머지 학과 (정답이 없는 글은 모르므로 뺀다)
 * 정답이 적을 때를 위해 사전값 (모든 쌍의 중앙값 -> 5%, 상위 10% 지점 -> 70%) 을 정규분포 사전으로
 * 두고 MAP 를 뉴턴법으로 푼다. 정답이 없으면 사전값 그대로다. */
static void fit_calibration(Model *m, const Label *lab, int nlab)
{
    size_t npairs = (size_t)m->nposts * (size_t)m->ndepts, k;
    float *yw = NULL, *sorted = NULL;
    char *has = NULL;
    double s_med = 0, s_hi = m->tau * log(5.0);
    double L0 = log(0.05 / 0.95), L1 = log(0.7 / 0.3), a0, b0, sa, sb = 1.5, a, b;
    int i, j, it;

    m->npos = m->nneg = 0;
    if (npairs == 0)
        return;
    sorted = (float *)malloc(npairs * sizeof *sorted);
    if (sorted) {
        memcpy(sorted, m->cos, npairs * sizeof *sorted);
        qsort(sorted, npairs, sizeof *sorted, float_desc);
        s_med = sorted[npairs / 2];
        free(sorted);
    }
    a0 = (L1 - L0) / (s_hi - s_med > 0.02 ? s_hi - s_med : 0.02);
    b0 = L1 - a0 * s_hi;
    sa = a0 / 2;
    a = a0;
    b = b0;

    yw = (float *)calloc(npairs, sizeof *yw);
    has = (char *)calloc((size_t)m->nposts, 1);
    if (yw && has) {
        for (i = 0; i < nlab; i++) {
            yw[(size_t)lab[i].pi * m->ndepts + lab[i].di] = lab[i].w;
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

        for (it = 0; it < 50 && m->npos > 0; it++) {
            double ga = (a - a0) / (sa * sa), gb = (b - b0) / (sb * sb);
            double haa = 1 / (sa * sa), hbb = 1 / (sb * sb), hab = 0, det, da, db;

            for (i = 0; i < m->nposts; i++) {
                if (!has[i])
                    continue;
                for (j = 0; j < m->ndepts; j++) {
                    k = (size_t)i * m->ndepts + j;
                    {
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
            det = haa * hbb - hab * hab;
            if (det <= 0)
                break;
            da = (hbb * ga - hab * gb) / det;
            db = (haa * gb - hab * ga) / det;
            a -= da;
            b -= db;
            if (fabs(da) < 1e-7 && fabs(db) < 1e-7)
                break;
        }
    }
    free(yw);
    free(has);
    m->pa = a;
    m->pb = b;
}

static volatile LONG    g_init;
static CRITICAL_SECTION g_lock;
static Model            g_model;

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

/* 학습 데이터가 바뀌었는지 가늠하는 문자열. 다르면 다시 학습한다. */
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
static int df_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Df *)a)->key, y = ((const Df *)b)->key;
    return x < y ? -1 : x > y;
}

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

static int float_desc(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int train(Model *m)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    my_ulonglong rows;
    Df *df = NULL;
    int ndf = 0, i, j, total_terms = 0;
    Vec *centroid = NULL;
    float *cweight = NULL, *ecent = NULL, *ecos = NULL;
    char **ptexts = NULL, **dtexts = NULL;
    Label *lab = NULL;
    int nlab = 0, caplab = 0;
    double t0 = (double)GetTickCount64();

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
        ptexts[m->nposts++] = join3(row[1], row[2], NULL);
        d->id = (unsigned)strtoul(row[0], NULL, 10);
        tokenize(&d->v, row[1], TITLE_WEIGHT);
        tokenize(&d->v, row[2], 1.0f);
        vec_compact(&d->v);
    }
    mysql_free_result(qr);

    /* 학과 (프로필이 없으면 학과 이름만으로 시작한다) */
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

    /* 문서 빈도 (게시물 + 학과 프로필을 한 말뭉치로 본다) */
    for (i = 0; i < m->nposts; i++) total_terms += m->posts[i].v.n;
    for (i = 0; i < m->ndepts; i++) total_terms += m->depts[i].v.n;
    df = (Df *)malloc((size_t)(total_terms ? total_terms : 1) * sizeof *df);
    if (!df)
        goto fail;
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
            int pi = -1, di = -1;

            for (i = 0; i < m->nposts; i++) if (m->posts[i].id == pid) { pi = i; break; }
            for (i = 0; i < m->ndepts; i++) if (m->depts[i].id == did) { di = i; break; }
            if (pi < 0 || di < 0)
                continue;
            vec_add_scaled(&centroid[di], &m->posts[pi].v, w);
            if (ecent)
                for (j = 0; j < m->dim; j++)
                    ecent[(size_t)di * m->dim + j] += w * m->pemb[(size_t)pi * m->dim + j];
            cweight[di] += w;
            m->labels++;
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
    for (i = 0; i < m->ndepts; i++) {
        Vec dv = { 0 };

        vec_add_scaled(&dv, &m->depts[i].v, ROCCHIO_ALPHA);
        if (cweight[i] > 0) {
            float beta = ROCCHIO_BETA * cweight[i] / (cweight[i] + SHRINK_K);

            vec_keep_top(&centroid[i], CENTROID_TOP);
            vec_normalize(&centroid[i]);
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
        vec_free(&m->depts[i].v);
        m->depts[i].v = dv;
        vec_free(&centroid[i]);
    }

    /* 유사도 표: 임베딩 코사인의 평균을 먼저 구한 뒤 TF-IDF 와 섞는다. */
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
        m->mu = sum / ((double)m->nposts * m->ndepts);
    }
    for (i = 0; i < m->nposts; i++)
        for (j = 0; j < m->ndepts; j++)
            m->cos[i * m->ndepts + j] = (float)hybrid(vec_dot(&m->posts[i].v, &m->depts[j].v),
                                                      ecos ? ecos[i * m->ndepts + j] : 0,
                                                      m->mu, m->dim);

    /* 점수 보정: 게시물마다 가장 잘 맞는 학과의 유사도를 모아, 그 상위 10% 지점이 80점이 되게 한다.
     * score = 100 * (1 - exp(-cos / tau)),  80 = 100 * (1 - exp(-p90 / tau))  ->  tau = p90 / ln 5 */
    {
        size_t np = m->nposts > 0 ? (size_t)m->nposts : 1;
        float *best = (float *)malloc(np * sizeof *best);
        double p90 = 0.2;

        if (best && m->nposts > 0) {
            for (i = 0; i < m->nposts; i++) {
                best[i] = 0;
                for (j = 0; j < m->ndepts; j++)
                    if (m->cos[i * m->ndepts + j] > best[i])
                        best[i] = m->cos[i * m->ndepts + j];
            }
            qsort(best, (size_t)m->nposts, sizeof *best, float_desc);
            p90 = best[m->nposts / 10];
        }
        free(best);
        if (p90 < 0.05)
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

/* 필요하면 다시 학습한다. g_lock 을 잡은 상태에서 부른다. */
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

static int to_score(double cos)
{
    double s;

    if (cos <= 0 || g_model.tau <= 0)
        return 0;
    s = 100.0 * (1.0 - exp(-cos / g_model.tau));
    return (int)(s + 0.5);
}

static double to_prob(double s)
{
    return sigmoid(g_model.pa * s + g_model.pb);
}

/* a 가 b 에 조사 한 글자만 붙은 형태인지 (프로젝트를 / 프로젝트) */
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
typedef struct { float c; const char *text; int bigram; } Contrib;

static int contrib_desc(const void *a, const void *b)
{
    float x = ((const Contrib *)a)->c, y = ((const Contrib *)b)->c;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void explain(const Vec *post, const Vec *dept, MlHit *hit)
{
    Contrib *cs = (Contrib *)malloc((size_t)(post->n ? post->n : 1) * sizeof *cs);
    int i = 0, j = 0, n = 0, k;

    hit->nterms = 0;
    if (!cs)
        return;
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

    /* 근거는 온전한 낱말로 보여준다. 겹친 낱말이 하나도 없을 때만 두 글자 묶음을 쓴다. */
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

static int hit_desc(const void *a, const void *b)
{
    const MlHit *x = (const MlHit *)a, *y = (const MlHit *)b;
    if (x->score != y->score)
        return y->score - x->score;
    return x->id < y->id ? 1 : -1;     /* 같은 점수면 최신 글(큰 번호) 먼저 */
}

int ml_rank_posts(unsigned department_id, int min_score, MlHit *out, int max, MlInfo *info)
{
    int di = -1, i, n = 0;
    MlHit *all;

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
        return di < 0 ? 0 : -1;
    }

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

static int prob_desc(const void *a, const void *b)
{
    double x = ((const MlHit *)a)->prob, y = ((const MlHit *)b)->prob;
    return x < y ? 1 : x > y ? -1 : 0;
}

int ml_predict_departments(const char *title, const char *body, MlHit *out, int max, MlInfo *info)
{
    Vec v = { 0 };
    float *emb = NULL;
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

int ml_rank_posts_personal(unsigned department_id, const char *profile_text, int min_score,
                           MlHit *out, int max, MlInfo *info)
{
    int di = -1, i, n = 0;
    Vec q = { 0 }, u = { 0 };
    float *qemb = NULL;
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

    /* 질의 = 학과 벡터 + 0.7 x 내 관심사(키워드·자기소개) 벡터 */
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
        explain(&g_model.posts[pi].v, &q, &out[i]);
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
 *     유사도에 더한다 (협업 필터링). 근거에는 "내 모집글에 ♥" 같은 말로 보여 준다. */

#define TAG_WEIGHT       3.0f
#define LIKE_TEXT_WEIGHT 0.5f
#define CO_LIKE_BONUS    0.15
#define AUTHOR_BONUS     0.10

typedef struct {
    int    ready;
    char   signature[240];
    Doc   *users;
    int    nusers;
    Doc   *recs;
    int    nrecs;
    double tau;
    int    dim;            /* 임베딩 차원 (0 = TF-IDF 만) */
    float *uemb, *remb;    /* 회원, 모집글 임베딩 */
    double mu_uu, mu_ur;   /* 회원-회원, 회원-모집글 임베딩 코사인 평균 */
    int   *like_off;       /* [nusers + 1] 회원 i 가 좋아한 모집글 = like_rec[like_off[i] .. like_off[i+1]) */
    int   *like_rec;       /* 모집글 색인, 회원마다 오름차순 */
    int   *rec_author;     /* [nrecs] 작성자의 회원 색인 (회원 모델에 없으면 -1) */
} PeopleModel;

static PeopleModel g_people;

static int find_doc(const Doc *docs, int n, unsigned id);

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

static int int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

/* 좋아요 관계를 읽는다. 실패해도 좋아요 없이 동작한다. */
static void load_likes(PeopleModel *m)
{
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int i, n = 0, *cnt;

    m->rec_author = (int *)malloc((size_t)(m->nrecs ? m->nrecs : 1) * sizeof *m->rec_author);
    m->like_off = (int *)calloc((size_t)m->nusers + 1, sizeof *m->like_off);
    if (!m->rec_author || !m->like_off)
        return;
    for (i = 0; i < m->nrecs; i++)
        m->rec_author[i] = -1;

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
        int *pair_u = (int *)malloc((size_t)(mysql_num_rows(qr) + 1) * sizeof *pair_u);
        int *pair_r = (int *)malloc((size_t)(mysql_num_rows(qr) + 1) * sizeof *pair_r);

        while (pair_u && pair_r && (row = mysql_fetch_row(qr)) != NULL) {
            int u = find_doc(m->users, m->nusers, (unsigned)strtoul(row[0], NULL, 10));
            int r = find_doc(m->recs, m->nrecs, (unsigned)strtoul(row[1], NULL, 10));
            if (u < 0 || r < 0)
                continue;
            pair_u[n] = u;
            pair_r[n] = r;
            cnt[u]++;
            n++;
        }
        /* 회원별로 묶는다 (계수 정렬) */
        for (i = 0; i < m->nusers; i++)
            m->like_off[i + 1] = m->like_off[i] + cnt[i];
        memset(cnt, 0, (size_t)m->nusers * sizeof *cnt);
        for (i = 0; i < n; i++)
            m->like_rec[m->like_off[pair_u[i]] + cnt[pair_u[i]]++] = pair_r[i];
        for (i = 0; i < m->nusers; i++)
            qsort(&m->like_rec[m->like_off[i]], (size_t)cnt[i], sizeof(int), int_cmp);
        free(pair_u);
        free(pair_r);
    }
    free(cnt);
    mysql_free_result(qr);
}

static int likes_of(const PeopleModel *m, int u, const int **list)
{
    if (!m->like_off || !m->like_rec) {
        *list = NULL;
        return 0;
    }
    *list = &m->like_rec[m->like_off[u]];
    return m->like_off[u + 1] - m->like_off[u];
}

#define SOCIAL_CO        1     /* 같은 모집글을 좋아함 */
#define SOCIAL_LIKED_ME  2     /* j 가 i 의 모집글을 좋아함 */
#define SOCIAL_I_LIKED   4     /* i 가 j 의 모집글을 좋아함 */

/* 회원 i, j 사이 좋아요 관계로 생기는 가산점. *flags 에 관계를 적는다. */
static double social_bonus(const PeopleModel *m, int i, int j, int *flags)
{
    const int *a, *b;
    int na = likes_of(m, i, &a), nb = likes_of(m, j, &b);
    int x = 0, y = 0, common = 0, k;
    double bonus = 0;

    *flags = 0;
    while (x < na && y < nb) {
        if (a[x] == b[y]) { common++; x++; y++; }
        else if (a[x] < b[y]) x++;
        else y++;
    }
    if (common) {
        bonus += CO_LIKE_BONUS * common / (double)(na + nb - common);
        *flags |= SOCIAL_CO;
    }
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

/* 근거 키워드 앞에 좋아요 관계를 끼워 넣는다. */
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

static double people_cos_ur(const PeopleModel *m, int i, int r)
{
    double e = m->dim ? dense_dot(&m->uemb[(size_t)i * m->dim], &m->remb[(size_t)r * m->dim], m->dim) : 0;
    return hybrid(vec_dot(&m->users[i].v, &m->recs[r].v), e, m->mu_ur, m->dim);
}

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

/* "영상,기획,팀장" 처럼 쉼표로 이은 키워드를 토큰으로 넣는다. */
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

/* *texts 에 임베딩할 글을 함께 담는다 (호출자가 free_texts). */
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

static int people_train(PeopleModel *m)
{
    Df *df = NULL;
    int ndf = 0, i, j, total = 0;
    char **utexts = NULL, **rtexts = NULL;

    memset(m, 0, sizeof *m);
    people_signature(m->signature, sizeof m->signature);

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

    /* 딥러닝 임베딩. 회원끼리는 대칭 비교라 둘 다 query 로 넣는다. */
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
    free(df);

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
            p50 = best[m->nusers / 2];
        }
        free(best);
        if (p50 < 0.05)
            p50 = 0.05;
        m->tau = p50 / log(1.0 / 0.3);
    }

    m->ready = 1;
    log_info("회원 매칭 모델 학습 (%s): 회원 %d, 모집글 %d, 좋아요 %d, tau %.3f",
             m->dim ? "임베딩 + TF-IDF" : "TF-IDF", m->nusers, m->nrecs,
             m->like_off ? m->like_off[m->nusers] : 0, m->tau);
    return 1;
}

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

static int people_score(double cos)
{
    double s;

    if (cos <= 0 || g_people.tau <= 0)
        return 0;
    s = 100.0 * (1.0 - exp(-cos / g_people.tau));
    return (int)(s + 0.5);
}

static int find_doc(const Doc *docs, int n, unsigned id)
{
    int i;
    for (i = 0; i < n; i++)
        if (docs[i].id == id)
            return i;
    return -1;
}

int ml_match_users(unsigned user_id, int min_score, MlHit *out, int max)
{
    int me, i, n = 0;
    MlHit *all;

    ensure_init();
    EnterCriticalSection(&g_lock);
    if (!people_ensure()) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }
    me = find_doc(g_people.users, g_people.nusers, user_id);
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
        if (i == me)
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
        explain(&g_people.users[ui].v, &g_people.users[me].v, &out[i]);
        explain_social(&g_people, me, ui, &out[i]);
    }
    free(all);
    LeaveCriticalSection(&g_lock);
    return n;
}

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

        memset(&out[i], 0, sizeof out[i]);
        out[i].id = recruit_ids[i];
        if (me < 0 || ri < 0)
            continue;
        out[i].score = people_score(people_cos_ur(&g_people, me, ri));
        explain(&g_people.recs[ri].v, &g_people.users[me].v, &out[i]);
    }
    LeaveCriticalSection(&g_lock);
    return me < 0 ? 0 : 1;
}
