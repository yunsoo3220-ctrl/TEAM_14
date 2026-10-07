/* 아주 작은 JSON 파서/빌더.
 * 파서는 요청 본문과 학교 홈페이지 REST 응답을 읽는 데 쓰고,
 * 빌더는 Buf 에 응답 JSON 을 조립하는 데 쓴다. */
#ifndef SKU_JSON_H
#define SKU_JSON_H

#include "common.h"

typedef enum {
    JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ
} JsType;

typedef struct Json Json;
struct Json {
    JsType  type;
    double  num;        /* JS_NUM */
    int     bval;       /* JS_BOOL */
    char   *str;        /* JS_STR (이스케이프 해제된 UTF-8) */
    Json  **items;      /* JS_ARR / JS_OBJ 의 값들 */
    char  **keys;       /* JS_OBJ 의 키들 */
    int     n;          /* items/keys 개수 */
};

Json       *json_parse(const char *text);
void        json_free(Json *j);

/* 객체에서 키로 값 찾기. 없으면 NULL. */
const Json *json_get(const Json *obj, const char *key);
/* 중첩 경로 조회. 예: json_path(o, "title", "rendered") */
const Json *json_path(const Json *obj, const char *k1, const char *k2);

const char *json_str(const Json *obj, const char *key, const char *def);
long        json_int(const Json *obj, const char *key, long def);
int         json_bool(const Json *obj, const char *key, int def);

/* 빌더: 문자열을 JSON 문자열 리터럴(따옴표 포함)로 써 넣는다. */
void json_write_str(Buf *b, const char *s);
/* "key":"value" 형태. value 가 NULL 이면 null. */
void json_kv_str(Buf *b, const char *key, const char *val);
void json_kv_int(Buf *b, const char *key, long long val);
void json_kv_bool(Buf *b, const char *key, int val);

#endif
