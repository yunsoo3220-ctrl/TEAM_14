/* SHA-256 과 비밀번호 해싱 (외부 암호 라이브러리 없이 자체 구현)
 *
 * SHA-256 은 임의 길이 입력을 32바이트(256비트) 요약값으로 바꾸는 단방향 해시 함수다.
 *   - 같은 입력은 항상 같은 출력
 *   - 출력에서 입력을 되돌려 알아낼 수 없음
 *   - 입력이 1비트만 달라도 출력이 완전히 달라짐
 * 그래서 DB 에는 비밀번호 대신 해시만 저장하고, 로그인 때 입력값을 다시 해시해 비교한다.
 *
 * 솔트(salt): 사용자마다 다른 난수를 비밀번호 앞에 붙여 해시한다. 같은 비밀번호라도
 * 해시가 달라지므로, 미리 계산해 둔 해시표(레인보우 테이블) 공격을 막는다.
 * 반복 해싱(key stretching): 해시를 여러 번 반복해 한 번 시도하는 데 드는 시간을 늘린다.
 * 그러면 DB 가 유출되더라도 무차별 대입으로 비밀번호를 찾는 속도가 크게 느려진다.
 */
#ifndef SKU_SHA256_H
#define SKU_SHA256_H

#include <stddef.h>

#define SHA256_DIGEST_LEN 32   /* SHA-256 출력 바이트 수 */
#define PW_SALT_HEX_LEN   32   /* 16바이트 = hex 32자 */
#define PW_HASH_HEX_LEN   64   /* 32바이트 = hex 64자 */

/* msg 의 len 바이트에 대한 SHA-256 다이제스트를 out(32바이트)에 쓴다. */
void sha256(const unsigned char *msg, size_t len, unsigned char out[SHA256_DIGEST_LEN]);

/* salt(hex) + password 를 반복 해싱해 hex 64자를 out 에 쓴다. out 은 65바이트 이상. */
void pw_hash(const char *salt_hex, const char *password, char *out_hex);

/* 비밀번호 검증. 같으면 1.
 * 비교는 상수 시간으로 해서, 응답 시간 차이로 해시를 한 글자씩 알아내는
 * 타이밍 공격을 막는다. */
int  pw_verify(const char *salt_hex, const char *hash_hex, const char *password);

#endif /* SKU_SHA256_H */
