/* 바깥으로 나가는 HTTP(S) 요청 (libcurl).
 *
 * Claude API(ai.c), 학교 홈페이지 수집(crawler.c), 임베딩 서비스(embed.c)가 함께 쓴다.
 * 예전에는 Windows 전용 WinHTTP 를 썼는데, 리눅스·맥에서도 돌도록 메일 발송(mailer.c)에
 * 이미 쓰던 libcurl 로 모았다.
 */
#ifndef SKU_NET_H
#define SKU_NET_H

#include "common.h"

/* libcurl 전역 초기화. 여러 번 불러도 처음 한 번만 실제로 초기화한다.
 * curl_global_init 은 스레드 안전하지 않으므로 main 에서 스레드를 만들기 전에 불러 둔다. */
void net_init(void);

/* 요청 하나를 보내고 응답 본문을 out 에 붙인다.
 *   method      "GET" / "POST"
 *   url         "https://api.anthropic.com/v1/messages" 처럼 전체 주소
 *   headers     "이름: 값\r\n" 줄들을 이은 문자열 (없으면 NULL)
 *   body        보낼 본문 (없으면 NULL)
 *   connect_ms  접속(이름 해석 포함)까지의 제한 시간
 *   total_ms    요청 전체의 제한 시간
 *   no_proxy    1 이면 프록시 환경변수를 무시한다 (127.0.0.1 호출용)
 *   status      HTTP 상태 코드 (응답을 못 받으면 0)
 * 응답을 받으면 1 (HTTP 상태와 무관), 연결·전송 실패면 0 과 err 메시지. */
int net_request(const char *method, const char *url, const char *headers, const char *body,
                long connect_ms, long total_ms, int no_proxy,
                Buf *out, long *status, char *err, size_t errsz);

#endif /* SKU_NET_H */
