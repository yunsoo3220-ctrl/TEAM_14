/* 메일 발송 (libcurl SMTP).
 *
 * 환경변수로 설정한다.
 *   SKU_SMTP_URL   smtps://smtp.gmail.com:465  또는  smtp://smtp.office365.com:587 (STARTTLS)
 *   SKU_SMTP_USER  로그인 계정
 *   SKU_SMTP_PASS  비밀번호 (Gmail 은 앱 비밀번호)
 *   SKU_SMTP_FROM  보내는 주소 (없으면 SKU_SMTP_USER)
 * scripts\start-server.ps1 은 프로젝트 폴더의 smtp.env 에서 이 값들을 읽어 준다.
 *
 * SKU_SMTP_URL 이 없으면 메일을 보낼 수 없다 (mail_send 실패).
 * 시험용으로 SKU_MAIL_DEV=1 을 주면 개발 모드: 보내지 않고 내용을 서버 로그에만 남긴다.
 *
 * 용도: 회원가입 시 학교 이메일(@skuniv.ac.kr) 인증 링크를 보내는 데 쓴다.
 * smtps:// 는 처음부터 TLS 로 접속(465 포트), smtp:// + 587 은 평문으로 접속한 뒤
 * STARTTLS 명령으로 암호화를 시작하는 방식이다. 비밀번호를 환경변수로 받는 이유는
 * 소스 코드나 저장소에 비밀 정보가 남지 않게 하기 위해서다.
 */
#ifndef SKU_MAILER_H
#define SKU_MAILER_H

#include <stddef.h>

/* SMTP 가 설정되어 있으면 1 */
int mail_enabled(void);

/* SMTP 가 없고 SKU_MAIL_DEV=1 이면 1 (로그로만 남기는 시험 모드) */
int mail_dev_mode(void);

/* 일반 텍스트(UTF-8) 메일을 보낸다. 성공(개발 모드에서는 로그 기록) 시 1.
 *   to      : 받는 사람 주소
 *   subject : 제목 (한글 가능 - 내부에서 MIME 인코딩)
 *   text    : 본문 (일반 텍스트)
 *   err     : 실패 시 사유를 받을 버퍼 (errsz 바이트) */
int mail_send(const char *to, const char *subject, const char *text, char *err, size_t errsz);

#endif /* SKU_MAILER_H */
