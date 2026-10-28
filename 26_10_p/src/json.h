/* 아주 작은 JSON 파서/빌더.
 * 파서는 요청 본문과 학교 홈페이지 REST 응답을 읽는 데 쓰고,
 * 빌더는 Buf 에 응답 JSON 을 조립하는 데 쓴다.
 *
 * 파서는 JSON 텍스트 전체를 한 번에 읽어 Json 노드 트리를 만든다(DOM 방식).
 * 예: {"a":[1,true]} →  OBJ(n=1, keys=["a"])
 *                         └ ARR(n=2)
 *                             ├ NUM(1)
 *                             └ BOOL(1)
 * 트리의 모든 노드와 문자열은 힙에 있으므로 다 쓰고 나면 루트에 json_free 한 번.
 */
#ifndef SKU_JSON_H
#define SKU_JSON_H

#include "common.h"

/* JSON 값의 종류 (JSON 표준의 6가지 타입에 대응) */
typedef enum {
    JS_NULL,   /* null */
    JS_BOOL,   /* true / false */
    JS_NUM,    /* 숫자 (정수/실수 구분 없이 double 로 저장) */
    JS_STR,    /* "문자열" */
    JS_ARR,    /* [ 배열 ] */
    JS_OBJ     /* { 객체 } */
} JsType;

typedef struct Json Json;
/* JSON 트리의 노드 하나. type 에 따라 쓰이는 필드가 다르다. */
struct Json {
    JsType  type;
    double  num;        /* JS_NUM */
    int     bval;       /* JS_BOOL */
    char   *str;        /* JS_STR (이스케이프 해제된 UTF-8) */
    Json  **items;      /* JS_ARR / JS_OBJ 의 값들 */
    char  **keys;       /* JS_OBJ 의 키들 (items[i] 의 키가 keys[i]) */
    int     n;          /* items/keys 개수 */
};

/* text 를 파싱해 트리를 만든다. 문법 오류나 메모리 부족이면 NULL. */
Json       *json_parse(const char *text);
/* 트리 전체를 재귀적으로 해제한다. NULL 이어도 안전. */
void        json_free(Json *j);

/* 객체에서 키로 값 찾기. 없으면 NULL. (obj 가 객체가 아니어도 NULL) */
const Json *json_get(const Json *obj, const char *key);
/* 중첩 경로 조회. 예: json_path(o, "title", "rendered")
 * → o["title"]["rendered"] 와 같다. 중간에 없으면 NULL. */
const Json *json_path(const Json *obj, const char *k1, const char *k2);

/* 타입을 확인하며 값을 꺼내는 도우미. 키가 없거나 타입이 다르면 def 를 돌려준다.
 * 이렇게 하면 클라이언트가 이상한 값을 보내도 NULL 역참조로 죽지 않는다. */
const char *json_str(const Json *obj, const char *key, const char *def);
long        json_int(const Json *obj, const char *key, long def);
int         json_bool(const Json *obj, const char *key, int def);

/* 빌더: 문자열을 JSON 문자열 리터럴(따옴표 포함)로 써 넣는다.
 * 따옴표("), 역슬래시(\), 제어문자는 \" \\ \n \u00XX 등으로 이스케이프한다.
 * 한글 같은 UTF-8 바이트는 그대로 내보낸다. (<, > 같은 HTML 특수문자는 건드리지
 * 않으므로, 화면에 넣을 때는 프론트엔드에서 textContent 등으로 안전하게 넣어야 한다.) */
void json_write_str(Buf *b, const char *s);
/* "key":"value" 형태. value 가 NULL 이면 null. */
void json_kv_str(Buf *b, const char *key, const char *val);
/* "key":123 형태 */
void json_kv_int(Buf *b, const char *key, long long val);
/* "key":true / "key":false 형태 */
void json_kv_bool(Buf *b, const char *key, int val);

#endif /* SKU_JSON_H */
