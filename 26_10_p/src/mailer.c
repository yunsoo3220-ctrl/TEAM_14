/* 메일 발송 (libcurl SMTP). 설정은 mailer.h 참고. */
#include "mailer.h"
#include "common.h"

#include <curl/curl.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *env(const char *k)
{
    const char *v = getenv(k);
    return v && v[0] ? v : NULL;
}

int mail_enabled(void)
{
    return env("SKU_SMTP_URL") != NULL;
}

int mail_dev_mode(void)
{
    const char *d = env("SKU_MAIL_DEV");
    return !mail_enabled() && d && strcmp(d, "1") == 0;
}

static void b64(Buf *b, const unsigned char *s, size_t n, int wrap)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, col = 0;

    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) |
                     (i + 2 < n ? s[i + 2] : 0);
        buf_putc(b, T[v >> 18 & 63]);
        buf_putc(b, T[v >> 12 & 63]);
        buf_putc(b, i + 1 < n ? T[v >> 6 & 63] : '=');
        buf_putc(b, i + 2 < n ? T[v & 63] : '=');
        if (wrap && (col += 4) >= 76 && i + 3 < n) {
            buf_puts(b, "\r\n");
            col = 0;
        }
    }
}

/* 헤더에 넣을 주소에 줄바꿈 같은 것이 섞이지 않았는지 */
static int safe_header(const char *s)
{
    for (; *s; s++)
        if (*s == '\r' || *s == '\n' || *s == '<' || *s == '>' || *s == '"')
            return 0;
    return 1;
}

typedef struct { const char *p; size_t left; } Src;

static size_t read_cb(char *dst, size_t size, size_t nmemb, void *ud)
{
    Src *s = (Src *)ud;
    size_t n = size * nmemb;

    if (n > s->left)
        n = s->left;
    memcpy(dst, s->p, n);
    s->p += n;
    s->left -= n;
    return n;
}

int mail_send(const char *to, const char *subject, const char *text, char *err, size_t errsz)
{
    const char *url = env("SKU_SMTP_URL"), *user = env("SKU_SMTP_USER"), *pass = env("SKU_SMTP_PASS");
    const char *from = env("SKU_SMTP_FROM") ? env("SKU_SMTP_FROM") : user;
    static volatile LONG inited;
    Buf msg, rcpt_addr;
    char date[64], id[24];
    struct curl_slist *rcpt = NULL;
    CURL *c;
    CURLcode rc;
    Src src;
    time_t now = time(NULL);
    struct tm g;

    err[0] = '\0';
    if (!safe_header(to) || (from && !safe_header(from))) {
        str_copy(err, errsz, "메일 주소 형식이 올바르지 않습니다.");
        return 0;
    }
    if (!url) {
        if (mail_dev_mode()) {
            /* 시험용 개발 모드: 실제로 보내지 않는다. 서버를 띄운 사람만 볼 수 있는 로그에 남긴다. */
            log_warn("[메일 개발 모드] 보내지 않았습니다. 받는 사람 %s, 제목 '%s'\n%s", to, subject, text);
            return 1;
        }
        str_copy(err, errsz, "메일 서버가 설정되지 않아 인증 메일을 보낼 수 없습니다. 관리자에게 문의하세요.");
        log_err("메일 서버(SKU_SMTP_URL)가 설정되지 않아 %s 에게 메일을 보내지 못했습니다. smtp.env 를 확인하세요.", to);
        return 0;
    }
    if (!from) {
        str_copy(err, errsz, "SKU_SMTP_FROM 또는 SKU_SMTP_USER 를 설정하세요.");
        return 0;
    }

    gmtime_s(&g, &now);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S +0000", &g);
    if (!random_hex(id, 16))
        str_copy(id, sizeof id, "0");

    buf_init(&msg);
    buf_printf(&msg, "Date: %s\r\nTo: <%s>\r\nFrom: SKU14 <%s>\r\nMessage-ID: <%s@sku14.com>\r\nSubject: =?UTF-8?B?",
               date, to, from, id);
    b64(&msg, (const unsigned char *)subject, strlen(subject), 0);
    buf_puts(&msg, "?=\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
                   "Content-Transfer-Encoding: base64\r\n\r\n");
    b64(&msg, (const unsigned char *)text, strlen(text), 1);
    buf_puts(&msg, "\r\n");

    if (InterlockedCompareExchange(&inited, 1, 0) == 0)
        curl_global_init(CURL_GLOBAL_DEFAULT);
    c = curl_easy_init();
    if (!c) {
        buf_free(&msg);
        str_copy(err, errsz, "메일 모듈을 초기화하지 못했습니다.");
        return 0;
    }
    buf_init(&rcpt_addr);
    buf_printf(&rcpt_addr, "<%s>", to);
    rcpt = curl_slist_append(rcpt, rcpt_addr.data);
    src.p = msg.data;
    src.left = msg.len;

    {
        Buf mail_from;
        buf_init(&mail_from);
        buf_printf(&mail_from, "<%s>", from);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);   /* 평문 SMTP 는 쓰지 않는다 */
        if (user) curl_easy_setopt(c, CURLOPT_USERNAME, user);
        if (pass) curl_easy_setopt(c, CURLOPT_PASSWORD, pass);
        curl_easy_setopt(c, CURLOPT_MAIL_FROM, mail_from.data);
        curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);
        curl_easy_setopt(c, CURLOPT_READFUNCTION, read_cb);
        curl_easy_setopt(c, CURLOPT_READDATA, &src);
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        rc = curl_easy_perform(c);
        buf_free(&mail_from);
    }

    if (rc != CURLE_OK) {
        snprintf(err, errsz, "메일을 보내지 못했습니다 (%s).", curl_easy_strerror(rc));
        log_err("메일 발송 실패 (%s): %s", to, curl_easy_strerror(rc));
    } else {
        log_info("메일 발송: %s '%s'", to, subject);
    }
    curl_slist_free_all(rcpt);
    curl_easy_cleanup(c);
    buf_free(&rcpt_addr);
    buf_free(&msg);
    return rc == CURLE_OK;
}
