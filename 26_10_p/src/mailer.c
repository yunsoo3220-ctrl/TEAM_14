/* 메일 발송 (libcurl SMTP). 설정은 mailer.h 참고.
 *
 * 전자우편 한 통은 "헤더 + 빈 줄 + 본문" 으로 된 텍스트다(RFC 5322).
 * 이 파일은 그 텍스트를 직접 조립한 뒤, SMTP 프로토콜 대화(EHLO, AUTH, MAIL FROM,
 * RCPT TO, DATA ...)는 libcurl 에 맡긴다.
 *
 * 한글 처리: 메일 헤더는 원래 ASCII 만 허용하므로 제목은 MIME 인코딩
 * "=?UTF-8?B?<base64>?=" 로 감싸고, 본문도 base64 로 인코딩해 보낸다.
 */
#include "mailer.h"
#include "common.h"

#include <curl/curl.h>   /* libcurl - SMTP/TLS 처리 */
#include <windows.h>     /* InterlockedCompareExchange */
#include <stdio.h>
#include <stdlib.h>      /* getenv */
#include <string.h>
#include <time.h>        /* Date 헤더용 현재 시각 */

/* 환경변수 k 의 값. 없거나 빈 문자열이면 NULL (빈 값도 "설정 안 됨" 으로 취급). */
static const char *env(const char *k)
{
    const char *v = getenv(k);
    return v && v[0] ? v : NULL;
}

/* SMTP 서버 주소가 설정되어 있으면 실제 발송 가능 */
int mail_enabled(void)
{
    return env("SKU_SMTP_URL") != NULL;
}

/* 개발 모드: SMTP 가 "없을 때만" SKU_MAIL_DEV=1 이 효력을 가진다.
 * 실제 SMTP 가 설정되어 있으면 개발 모드 플래그가 있어도 진짜로 보낸다. */
int mail_dev_mode(void)
{
    const char *d = env("SKU_MAIL_DEV");
    return !mail_enabled() && d && strcmp(d, "1") == 0;
}

/* base64 인코딩해 b 에 붙인다.
 *   3바이트(24비트)를 6비트씩 4조각으로 나눠 각각 64개 문자 중 하나로 바꾼다.
 *   입력이 3의 배수가 아니면 모자란 자리는 '=' 로 채운다.
 *   wrap 이 1 이면 76글자마다 줄바꿈(CRLF)을 넣는다 (메일 본문 한 줄 길이 제한, RFC 2045).
 *   제목처럼 한 줄이어야 하는 곳에는 wrap = 0. */
static void b64(Buf *b, const unsigned char *s, size_t n, int wrap)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, col = 0;   /* col: 현재 줄에 쓴 글자 수 */

    for (i = 0; i < n; i += 3) {
        /* 최대 3바이트를 24비트 정수 하나로 합친다 (없는 바이트는 0) */
        unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) |
                     (i + 2 < n ? s[i + 2] : 0);
        buf_putc(b, T[v >> 18 & 63]);                     /* 1번째 6비트 */
        buf_putc(b, T[v >> 12 & 63]);                     /* 2번째 6비트 */
        buf_putc(b, i + 1 < n ? T[v >> 6 & 63] : '=');    /* 2번째 바이트가 없으면 패딩 */
        buf_putc(b, i + 2 < n ? T[v & 63] : '=');         /* 3번째 바이트가 없으면 패딩 */
        if (wrap && (col += 4) >= 76 && i + 3 < n) {      /* 마지막 묶음 뒤에는 줄바꿈 안 함 */
            buf_puts(b, "\r\n");
            col = 0;
        }
    }
}

/* 헤더에 넣을 주소에 줄바꿈 같은 것이 섞이지 않았는지
 * 주소에 "\r\nBcc: 남@메일" 같은 문자열을 넣으면 헤더를 몰래 추가할 수 있다
 * (메일 헤더 주입 공격). 또 <, >, " 는 "<주소>" 형식을 깨뜨릴 수 있으므로 함께 막는다. */
static int safe_header(const char *s)
{
    for (; *s; s++)
        if (*s == '\r' || *s == '\n' || *s == '<' || *s == '>' || *s == '"')
            return 0;
    return 1;
}

/* libcurl 에 메일 본문을 조금씩 넘겨주기 위한 읽기 위치 */
typedef struct { const char *p; size_t left; } Src;   /* p: 다음에 보낼 위치, left: 남은 바이트 */

/* libcurl 읽기 콜백. curl 이 "보낼 데이터 최대 size*nmemb 바이트를 dst 에 채워 달라" 고
 * 반복해서 부른다. 0 을 돌려주면 데이터가 끝났다는 뜻이다. */
static size_t read_cb(char *dst, size_t size, size_t nmemb, void *ud)
{
    Src *s = (Src *)ud;          /* CURLOPT_READDATA 로 넘긴 포인터 */
    size_t n = size * nmemb;     /* 이번에 받을 수 있는 최대 바이트 */

    if (n > s->left)
        n = s->left;
    memcpy(dst, s->p, n);
    s->p += n;
    s->left -= n;
    return n;
}

/* 메일 한 통을 보낸다. 자세한 설명은 mailer.h 참고. */
int mail_send(const char *to, const char *subject, const char *text, char *err, size_t errsz)
{
    const char *url = env("SKU_SMTP_URL"), *user = env("SKU_SMTP_USER"), *pass = env("SKU_SMTP_PASS");
    const char *from = env("SKU_SMTP_FROM") ? env("SKU_SMTP_FROM") : user;   /* 보내는 주소 기본값 = 로그인 계정 */
    static volatile LONG inited;     /* curl_global_init 을 했는지 (프로그램 전체에서 한 번) */
    Buf msg, rcpt_addr;              /* msg: 메일 원문, rcpt_addr: "<받는주소>" */
    char date[64], id[24];           /* Date 헤더 값, Message-ID 의 난수 부분 */
    struct curl_slist *rcpt = NULL;  /* 받는 사람 목록 (curl 연결 리스트) */
    CURL *c;
    CURLcode rc;
    Src src;
    time_t now = time(NULL);
    struct tm g;

    err[0] = '\0';
    /* 1) 주소 안전성 검사 */
    if (!safe_header(to) || (from && !safe_header(from))) {
        str_copy(err, errsz, "메일 주소 형식이 올바르지 않습니다.");
        return 0;
    }
    /* 2) SMTP 설정이 없을 때: 개발 모드면 로그로 대신하고, 아니면 실패 */
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

    /* 3) 헤더 값 준비
     * Date 헤더는 RFC 5322 형식 "Thu, 09 Oct 2026 05:03:12 +0000". UTC(gmtime)로 써서
     * 시간대 계산 문제를 피한다. Message-ID 는 메일마다 고유해야 하므로 난수로 만든다. */
    gmtime_s(&g, &now);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S +0000", &g);
    if (!random_hex(id, 16))
        str_copy(id, sizeof id, "0");   /* Message-ID 는 보안 값이 아니므로 실패해도 진행 */

    /* 4) 메일 원문 조립: 헤더들 → 빈 줄(\r\n\r\n) → base64 본문 */
    buf_init(&msg);
    buf_printf(&msg, "Date: %s\r\nTo: <%s>\r\nFrom: SKU14 <%s>\r\nMessage-ID: <%s@sku14.com>\r\nSubject: =?UTF-8?B?",
               date, to, from, id);
    b64(&msg, (const unsigned char *)subject, strlen(subject), 0);   /* 제목 = MIME encoded-word */
    buf_puts(&msg, "?=\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
                   "Content-Transfer-Encoding: base64\r\n\r\n");
    b64(&msg, (const unsigned char *)text, strlen(text), 1);         /* 본문 (76자 줄바꿈) */
    buf_puts(&msg, "\r\n");

    /* 5) libcurl 준비
     * curl_global_init 은 스레드 안전하지 않고 한 번만 불러야 한다. 여러 요청 스레드가
     * 동시에 들어와도 처음 한 스레드만 0→1 로 바꾸는 데 성공해 초기화를 한다(원자적 비교-교환). */
    if (InterlockedCompareExchange(&inited, 1, 0) == 0)
        curl_global_init(CURL_GLOBAL_DEFAULT);
    c = curl_easy_init();   /* 이번 발송 한 건에 쓸 핸들 */
    if (!c) {
        buf_free(&msg);
        str_copy(err, errsz, "메일 모듈을 초기화하지 못했습니다.");
        return 0;
    }
    buf_init(&rcpt_addr);
    buf_printf(&rcpt_addr, "<%s>", to);                 /* SMTP RCPT TO 는 <주소> 형식 */
    rcpt = curl_slist_append(rcpt, rcpt_addr.data);
    src.p = msg.data;                                   /* 읽기 콜백이 보낼 데이터 */
    src.left = msg.len;

    {
        Buf mail_from;          /* SMTP MAIL FROM 에 쓸 "<보내는주소>" */
        buf_init(&mail_from);
        buf_printf(&mail_from, "<%s>", from);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);   /* 평문 SMTP 는 쓰지 않는다 */
        if (user) curl_easy_setopt(c, CURLOPT_USERNAME, user);
        if (pass) curl_easy_setopt(c, CURLOPT_PASSWORD, pass);
        curl_easy_setopt(c, CURLOPT_MAIL_FROM, mail_from.data);
        curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);
        curl_easy_setopt(c, CURLOPT_READFUNCTION, read_cb);   /* 본문을 공급할 콜백 */
        curl_easy_setopt(c, CURLOPT_READDATA, &src);          /* 콜백의 ud 인자 */
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);              /* SMTP 에서는 "본문을 올린다" 는 뜻 */
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);     /* 접속 10초 제한 */
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);            /* 전체 30초 제한 (요청 스레드가 오래 묶이지 않게) */
        rc = curl_easy_perform(c);                            /* 실제 발송 (블로킹) */
        buf_free(&mail_from);
    }

    /* 6) 결과 기록과 정리 */
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
