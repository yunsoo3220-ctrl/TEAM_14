/* 작은 재귀 하강 JSON 파서와 문자열 이스케이프 빌더
 *
 * 재귀 하강(recursive descent) 파서란?
 *   문법 규칙 하나마다 함수 하나를 두고, 규칙이 다른 규칙을 포함하면 함수가 다른
 *   함수를 부르는 방식이다. JSON 문법은
 *       value  = string | number | object | array | true | false | null
 *       object = '{' [ string ':' value { ',' string ':' value } ] '}'
 *       array  = '[' [ value { ',' value } ] ']'
 *   처럼 value 가 다시 object/array 를 포함하므로, parse_value 가 parse_container 를
 *   부르고 parse_container 가 다시 parse_value 를 부르는 상호 재귀 구조가 된다.
 *
 * 오류 처리: 파서 상태(Parser)에 failed 플래그를 두고, 한 번 실패하면 모든 단계가
 * 즉시 빠져나오며 그때까지 만든 노드를 해제한다.
 */
#include "json.h"

#include <stdlib.h>   /* calloc, realloc, free, strtod, strtol */
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* 중첩 최대 깊이. [[[[...]]]] 처럼 수만 겹 중첩된 악의적 입력을 받으면 재귀가 너무 깊어져
 * 스택이 넘친다(서버 다운). 깊이를 제한해 그런 입력은 오류로 거절한다. */
#define JSON_MAX_DEPTH 32

/* 파서 상태 */
typedef struct {
    const char *p;    /* 지금 읽고 있는 위치 */
    int depth;        /* 현재 중첩 깊이 */
    int failed;       /* 1 이면 오류 발생 - 이후 모든 처리 중단 */
} Parser;

static Json *parse_value(Parser *ps);   /* 상호 재귀를 위한 전방 선언 */

/* 타입 t 인 빈 노드를 만든다. calloc 이라 모든 필드가 0/NULL 로 시작한다. */
static Json *node_new(JsType t)
{
    Json *j = (Json *)calloc(1, sizeof *j);
    if (j)
        j->type = t;
    return j;
}

/* 노드와 그 아래 모든 자식을 재귀적으로 해제한다. */
void json_free(Json *j)
{
    int i;

    if (!j)
        return;
    for (i = 0; i < j->n; i++) {
        if (j->keys)            /* 배열은 keys 가 NULL 이다 */
            free(j->keys[i]);
        json_free(j->items[i]); /* 자식 노드부터 해제 (후위 순회) */
    }
    free(j->keys);
    free(j->items);
    free(j->str);
    free(j);
}

/* JSON 에서 허용하는 공백 4종(스페이스, 탭, LF, CR)을 건너뛴다. */
static void skip_ws(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

/* 코드포인트를 UTF-8 로 Buf 에 추가
 * UTF-8 인코딩 규칙 (x = 코드포인트의 비트):
 *   U+0000 ~ U+007F    : 0xxxxxxx                              (1바이트, ASCII)
 *   U+0080 ~ U+07FF    : 110xxxxx 10xxxxxx                     (2바이트)
 *   U+0800 ~ U+FFFF    : 1110xxxx 10xxxxxx 10xxxxxx            (3바이트, 한글이 여기)
 *   U+10000 ~ U+10FFFF : 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   (4바이트, 이모지 등) */
static void put_utf8(Buf *b, unsigned cp)
{
    if (cp < 0x80) {
        buf_putc(b, (char)cp);
    } else if (cp < 0x800) {
        buf_putc(b, (char)(0xC0 | (cp >> 6)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_putc(b, (char)(0xE0 | (cp >> 12)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(b, (char)(0xF0 | (cp >> 18)));
        buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* p 에서 16진 숫자 4글자를 읽어 *out 에 값을 쓴다. 16진이 아닌 글자가 있으면 0.
 * (\uXXXX 이스케이프 처리용. 문자열이 일찍 끝나면 NUL 에서 실패하므로 넘침 없음) */
static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;                                                    /* 자리 올림 */
        if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* 여는 따옴표에서 시작해 문자열을 읽고 malloc 한 UTF-8 문자열을 돌려준다.
 * 반환값의 소유권은 호출자에게 넘어간다. */
static char *parse_string_raw(Parser *ps)
{
    Buf b;   /* 이스케이프를 푼 결과를 모으는 버퍼 */

    if (*ps->p != '"') {     /* 문자열은 반드시 " 로 시작 */
        ps->failed = 1;
        return NULL;
    }
    ps->p++;
    buf_init(&b);

    while (*ps->p && *ps->p != '"') {
        if (*ps->p != '\\') {               /* 보통 글자는 그대로 */
            buf_putc(&b, *ps->p++);
            continue;
        }
        ps->p++;                            /* 역슬래시 다음 글자로 이스케이프 종류 판단 */
        switch (*ps->p) {
        case '"':  buf_putc(&b, '"');  ps->p++; break;
        case '\\': buf_putc(&b, '\\'); ps->p++; break;
        case '/':  buf_putc(&b, '/');  ps->p++; break;
        case 'b':  buf_putc(&b, '\b'); ps->p++; break;   /* 백스페이스 */
        case 'f':  buf_putc(&b, '\f'); ps->p++; break;   /* 폼피드 */
        case 'n':  buf_putc(&b, '\n'); ps->p++; break;
        case 'r':  buf_putc(&b, '\r'); ps->p++; break;
        case 't':  buf_putc(&b, '\t'); ps->p++; break;
        case 'u': {                                      /* \uXXXX 유니코드 이스케이프 */
            unsigned cp;
            ps->p++;
            if (!hex4(ps->p, &cp)) {
                ps->failed = 1;
                buf_free(&b);
                return NULL;
            }
            ps->p += 4;
            /* 서러게이트 쌍이면 하위 서러게이트까지 읽어 합친다.
             * JSON 의 \u 는 UTF-16 단위라서 U+FFFF 를 넘는 글자(이모지 등)는
             * 😀 처럼 두 개(상위 D800~DBFF + 하위 DC00~DFFF)로 온다.
             * 둘을 합쳐 실제 코드포인트 = 0x10000 + (상위-0xD800)<<10 + (하위-0xDC00). */
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;   /* "\uXXXX" 6글자 소비 */
                }
            }
            put_utf8(&b, cp);
            break;
        }
        default:                            /* 정의되지 않은 이스케이프 (\x 등) */
            ps->failed = 1;
            buf_free(&b);
            return NULL;
        }
    }

    if (*ps->p != '"') {     /* 닫는 따옴표 없이 입력이 끝남 */
        ps->failed = 1;
        buf_free(&b);
        return NULL;
    }
    ps->p++;

    /* 빈 문자열 "" 이면 버퍼가 한 번도 할당되지 않아 data 가 NULL 이다.
     * 호출자가 NULL 과 빈 문자열을 구분할 수 있게 "" 를 따로 만들어 준다. */
    return b.data ? b.data : str_dup("");
}

/* 배열/객체 j 에 값(과 키)을 하나 덧붙인다. 메모리 부족이면 0.
 * 원소마다 realloc 하므로 원소가 아주 많으면 느리지만, 이 서버가 다루는 JSON 크기에서는
 * 단순함이 더 중요하다. */
static int push_item(Json *j, char *key, Json *val)
{
    Json **it = (Json **)realloc(j->items, sizeof(Json *) * (size_t)(j->n + 1));
    if (!it)
        return 0;
    j->items = it;

    if (j->type == JS_OBJ) {     /* 객체면 키 배열도 같이 늘린다 */
        char **ks = (char **)realloc(j->keys, sizeof(char *) * (size_t)(j->n + 1));
        if (!ks)
            return 0;
        j->keys = ks;
        j->keys[j->n] = key;     /* 키 문자열 소유권이 노드로 넘어간다 */
    }
    j->items[j->n] = val;
    j->n++;
    return 1;
}

/* 배열/객체 본문을 읽어 j 에 채운다. 실패 시 0.
 * 호출 시점에 여는 괄호('{' 또는 '[')는 이미 소비된 상태이며, close 는 닫는 괄호. */
static int parse_container(Parser *ps, Json *j, char close)
{
    int is_obj = (j->type == JS_OBJ);

    skip_ws(ps);
    if (*ps->p == close) {   /* {} 또는 [] - 빈 컨테이너 */
        ps->p++;
        return 1;
    }

    for (;;) {
        char *key = NULL;
        Json *val;

        skip_ws(ps);
        if (is_obj) {
            /* 객체 원소: "키" : 값 */
            key = parse_string_raw(ps);
            if (ps->failed) {
                free(key);
                return 0;
            }
            skip_ws(ps);
            if (*ps->p != ':') {
                free(key);
                ps->failed = 1;
                return 0;
            }
            ps->p++;
        }

        val = parse_value(ps);                    /* 값 (재귀) */
        if (ps->failed || !push_item(j, key, val)) {
            free(key);
            json_free(val);
            ps->failed = 1;
            return 0;
        }

        skip_ws(ps);
        if (*ps->p == ',') {     /* 다음 원소가 있다 */
            ps->p++;
            continue;
        }
        if (*ps->p == close) {   /* 컨테이너 끝 */
            ps->p++;
            return 1;
        }
        ps->failed = 1;          /* , 도 닫는 괄호도 아니면 문법 오류 */
        return 0;
    }
}

/* 값 하나를 읽는다. 첫 글자로 어떤 타입인지 판단한다. */
static Json *parse_value(Parser *ps)
{
    Json *j = NULL;

    /* 들어올 때 깊이 +1, 나갈 때 -1. 제한을 넘으면 실패. */
    if (ps->failed || ++ps->depth > JSON_MAX_DEPTH) {
        ps->failed = 1;
        ps->depth--;
        return NULL;
    }
    skip_ws(ps);

    switch (*ps->p) {
    case '"':                                    /* 문자열 */
        j = node_new(JS_STR);
        if (!j) {
            ps->failed = 1;
            break;
        }
        j->str = parse_string_raw(ps);
        if (ps->failed) {
            json_free(j);
            j = NULL;
        }
        break;

    case '{':                                    /* 객체 */
    case '[': {                                  /* 배열 */
        int is_obj = (*ps->p == '{');
        ps->p++;
        j = node_new(is_obj ? JS_OBJ : JS_ARR);
        if (!j) {
            ps->failed = 1;
            break;
        }
        if (!parse_container(ps, j, is_obj ? '}' : ']')) {
            json_free(j);        /* 반쯤 채운 컨테이너도 통째로 해제 */
            j = NULL;
        }
        break;
    }

    case 't':                                    /* true */
        if (strncmp(ps->p, "true", 4) != 0) {
            ps->failed = 1;
            break;
        }
        ps->p += 4;
        j = node_new(JS_BOOL);
        if (j)
            j->bval = 1;
        else
            ps->failed = 1;
        break;

    case 'f':                                    /* false (bval 은 calloc 으로 이미 0) */
        if (strncmp(ps->p, "false", 5) != 0) {
            ps->failed = 1;
            break;
        }
        ps->p += 5;
        j = node_new(JS_BOOL);
        if (!j)
            ps->failed = 1;
        break;

    case 'n':                                    /* null */
        if (strncmp(ps->p, "null", 4) != 0) {
            ps->failed = 1;
            break;
        }
        ps->p += 4;
        j = node_new(JS_NULL);
        if (!j)
            ps->failed = 1;
        break;

    default: {                                   /* 그 밖은 숫자로 시도 */
        char *end;
        /* strtod 가 숫자를 읽고 end 에 "읽기를 멈춘 위치" 를 알려 준다.
         * 한 글자도 못 읽었으면(end == p) 숫자가 아니다.
         * (strtod 는 JSON 보다 너그러워 "0x1F", "inf" 등도 받지만, 실사용에 문제는 없다) */
        double v = strtod(ps->p, &end);
        if (end == ps->p) {
            ps->failed = 1;
            break;
        }
        ps->p = end;
        j = node_new(JS_NUM);
        if (j)
            j->num = v;
        else
            ps->failed = 1;
        break;
    }
    }

    ps->depth--;
    return j;
}

/* 공개 진입점: 텍스트 전체를 파싱한다.
 * 참고: 최상위 값 뒤에 남는 글자는 검사하지 않는다. ({"a":1} xyz 도 성공으로 본다) */
Json *json_parse(const char *text)
{
    Parser ps;
    Json *j;

    if (!text)
        return NULL;
    ps.p = text;
    ps.depth = 0;
    ps.failed = 0;

    j = parse_value(&ps);
    if (ps.failed) {
        json_free(j);
        return NULL;
    }
    return j;
}

/* 객체에서 key 를 선형 탐색한다. (키가 수십 개 이하라 해시표보다 단순한 쪽이 낫다)
 * 같은 키가 여러 번 있으면 처음 것을 돌려준다. */
const Json *json_get(const Json *obj, const char *key)
{
    int i;

    if (!obj || obj->type != JS_OBJ)
        return NULL;
    for (i = 0; i < obj->n; i++)
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    return NULL;
}

/* obj[k1][k2]. json_get 이 NULL 입력을 받아도 NULL 을 돌려주므로 중간 검사가 필요 없다. */
const Json *json_path(const Json *obj, const char *k1, const char *k2)
{
    return json_get(json_get(obj, k1), k2);
}

/* 문자열 값. 반환된 포인터는 트리 안의 메모리이므로 json_free 이후에는 쓰면 안 된다. */
const char *json_str(const Json *obj, const char *key, const char *def)
{
    const Json *v = json_get(obj, key);
    return (v && v->type == JS_STR) ? v->str : def;
}

/* 정수 값. 숫자는 소수점 아래를 버리고, "12" 같은 숫자 문자열도 받아 준다.
 * (폼에서 온 값이나 외부 API 가 숫자를 문자열로 주는 경우를 위해) */
long json_int(const Json *obj, const char *key, long def)
{
    const Json *v = json_get(obj, key);

    if (!v)
        return def;
    if (v->type == JS_NUM)
        return (long)v->num;
    if (v->type == JS_STR)
        return strtol(v->str, NULL, 10);
    return def;
}

/* 불리언 값. true/false 와 숫자(0 이면 거짓, 그 밖은 참)를 받아 준다. */
int json_bool(const Json *obj, const char *key, int def)
{
    const Json *v = json_get(obj, key);

    if (!v)
        return def;
    if (v->type == JS_BOOL)
        return v->bval;
    if (v->type == JS_NUM)
        return v->num != 0;
    return def;
}

/* 문자열을 "..." JSON 리터럴로 써 넣는다. NULL 은 빈 문자열 "" 이 된다. */
void json_write_str(Buf *b, const char *s)
{
    buf_putc(b, '"');
    if (s) {
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
            case '"':  buf_puts(b, "\\\""); break;   /* "  → \"  */
            case '\\': buf_puts(b, "\\\\"); break;   /* \  → \\  */
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            case '\b': buf_puts(b, "\\b");  break;
            case '\f': buf_puts(b, "\\f");  break;
            default:
                /* JSON 은 0x00~0x1F 제어문자를 문자열 안에 그대로 두는 것을 금지한다 */
                if (c < 0x20)
                    buf_printf(b, "\\u%04x", c);
                else
                    buf_putc(b, (char)c);   /* UTF-8 바이트는 그대로 내보낸다 */
            }
        }
    }
    buf_putc(b, '"');
}

/* "key":"val" 또는 "key":null */
void json_kv_str(Buf *b, const char *key, const char *val)
{
    json_write_str(b, key);
    buf_putc(b, ':');
    if (val)
        json_write_str(b, val);
    else
        buf_puts(b, "null");
}

/* "key":123 */
void json_kv_int(Buf *b, const char *key, long long val)
{
    json_write_str(b, key);
    buf_printf(b, ":%lld", val);
}

/* "key":true / "key":false */
void json_kv_bool(Buf *b, const char *key, int val)
{
    json_write_str(b, key);
    buf_puts(b, val ? ":true" : ":false");
}
