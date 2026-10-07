/* 작은 재귀 하강 JSON 파서와 문자열 이스케이프 빌더 */
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define JSON_MAX_DEPTH 32

typedef struct {
    const char *p;
    int depth;
    int failed;
} Parser;

static Json *parse_value(Parser *ps);

static Json *node_new(JsType t)
{
    Json *j = (Json *)calloc(1, sizeof *j);
    if (j)
        j->type = t;
    return j;
}

void json_free(Json *j)
{
    int i;

    if (!j)
        return;
    for (i = 0; i < j->n; i++) {
        if (j->keys)
            free(j->keys[i]);
        json_free(j->items[i]);
    }
    free(j->keys);
    free(j->items);
    free(j->str);
    free(j);
}

static void skip_ws(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
        ps->p++;
}

/* 코드포인트를 UTF-8 로 Buf 에 추가 */
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

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
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
    Buf b;

    if (*ps->p != '"') {
        ps->failed = 1;
        return NULL;
    }
    ps->p++;
    buf_init(&b);

    while (*ps->p && *ps->p != '"') {
        if (*ps->p != '\\') {
            buf_putc(&b, *ps->p++);
            continue;
        }
        ps->p++;
        switch (*ps->p) {
        case '"':  buf_putc(&b, '"');  ps->p++; break;
        case '\\': buf_putc(&b, '\\'); ps->p++; break;
        case '/':  buf_putc(&b, '/');  ps->p++; break;
        case 'b':  buf_putc(&b, '\b'); ps->p++; break;
        case 'f':  buf_putc(&b, '\f'); ps->p++; break;
        case 'n':  buf_putc(&b, '\n'); ps->p++; break;
        case 'r':  buf_putc(&b, '\r'); ps->p++; break;
        case 't':  buf_putc(&b, '\t'); ps->p++; break;
        case 'u': {
            unsigned cp;
            ps->p++;
            if (!hex4(ps->p, &cp)) {
                ps->failed = 1;
                buf_free(&b);
                return NULL;
            }
            ps->p += 4;
            /* 서러게이트 쌍이면 하위 서러게이트까지 읽어 합친다. */
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            put_utf8(&b, cp);
            break;
        }
        default:
            ps->failed = 1;
            buf_free(&b);
            return NULL;
        }
    }

    if (*ps->p != '"') {
        ps->failed = 1;
        buf_free(&b);
        return NULL;
    }
    ps->p++;

    return b.data ? b.data : str_dup("");
}

static int push_item(Json *j, char *key, Json *val)
{
    Json **it = (Json **)realloc(j->items, sizeof(Json *) * (size_t)(j->n + 1));
    if (!it)
        return 0;
    j->items = it;

    if (j->type == JS_OBJ) {
        char **ks = (char **)realloc(j->keys, sizeof(char *) * (size_t)(j->n + 1));
        if (!ks)
            return 0;
        j->keys = ks;
        j->keys[j->n] = key;
    }
    j->items[j->n] = val;
    j->n++;
    return 1;
}

/* 배열/객체 본문을 읽어 j 에 채운다. 실패 시 0. */
static int parse_container(Parser *ps, Json *j, char close)
{
    int is_obj = (j->type == JS_OBJ);

    skip_ws(ps);
    if (*ps->p == close) {
        ps->p++;
        return 1;
    }

    for (;;) {
        char *key = NULL;
        Json *val;

        skip_ws(ps);
        if (is_obj) {
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

        val = parse_value(ps);
        if (ps->failed || !push_item(j, key, val)) {
            free(key);
            json_free(val);
            ps->failed = 1;
            return 0;
        }

        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == close) {
            ps->p++;
            return 1;
        }
        ps->failed = 1;
        return 0;
    }
}

static Json *parse_value(Parser *ps)
{
    Json *j = NULL;

    if (ps->failed || ++ps->depth > JSON_MAX_DEPTH) {
        ps->failed = 1;
        ps->depth--;
        return NULL;
    }
    skip_ws(ps);

    switch (*ps->p) {
    case '"':
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

    case '{':
    case '[': {
        int is_obj = (*ps->p == '{');
        ps->p++;
        j = node_new(is_obj ? JS_OBJ : JS_ARR);
        if (!j) {
            ps->failed = 1;
            break;
        }
        if (!parse_container(ps, j, is_obj ? '}' : ']')) {
            json_free(j);
            j = NULL;
        }
        break;
    }

    case 't':
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

    case 'f':
        if (strncmp(ps->p, "false", 5) != 0) {
            ps->failed = 1;
            break;
        }
        ps->p += 5;
        j = node_new(JS_BOOL);
        if (!j)
            ps->failed = 1;
        break;

    case 'n':
        if (strncmp(ps->p, "null", 4) != 0) {
            ps->failed = 1;
            break;
        }
        ps->p += 4;
        j = node_new(JS_NULL);
        if (!j)
            ps->failed = 1;
        break;

    default: {
        char *end;
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

const Json *json_path(const Json *obj, const char *k1, const char *k2)
{
    return json_get(json_get(obj, k1), k2);
}

const char *json_str(const Json *obj, const char *key, const char *def)
{
    const Json *v = json_get(obj, key);
    return (v && v->type == JS_STR) ? v->str : def;
}

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

void json_write_str(Buf *b, const char *s)
{
    buf_putc(b, '"');
    if (s) {
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n");  break;
            case '\r': buf_puts(b, "\\r");  break;
            case '\t': buf_puts(b, "\\t");  break;
            case '\b': buf_puts(b, "\\b");  break;
            case '\f': buf_puts(b, "\\f");  break;
            default:
                if (c < 0x20)
                    buf_printf(b, "\\u%04x", c);
                else
                    buf_putc(b, (char)c);   /* UTF-8 바이트는 그대로 내보낸다 */
            }
        }
    }
    buf_putc(b, '"');
}

void json_kv_str(Buf *b, const char *key, const char *val)
{
    json_write_str(b, key);
    buf_putc(b, ':');
    if (val)
        json_write_str(b, val);
    else
        buf_puts(b, "null");
}

void json_kv_int(Buf *b, const char *key, long long val)
{
    json_write_str(b, key);
    buf_printf(b, ":%lld", val);
}

void json_kv_bool(Buf *b, const char *key, int val)
{
    json_write_str(b, key);
    buf_puts(b, val ? ":true" : ":false");
}
