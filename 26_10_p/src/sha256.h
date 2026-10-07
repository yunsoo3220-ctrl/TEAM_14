/* SHA-256 과 비밀번호 해싱 (외부 암호 라이브러리 없이 자체 구현) */
#ifndef SKU_SHA256_H
#define SKU_SHA256_H

#include <stddef.h>

#define SHA256_DIGEST_LEN 32
#define PW_SALT_HEX_LEN   32   /* 16바이트 = hex 32자 */
#define PW_HASH_HEX_LEN   64   /* 32바이트 = hex 64자 */

void sha256(const unsigned char *msg, size_t len, unsigned char out[SHA256_DIGEST_LEN]);

/* salt(hex) + password 를 반복 해싱해 hex 64자를 out 에 쓴다. out 은 65바이트 이상. */
void pw_hash(const char *salt_hex, const char *password, char *out_hex);

/* 비밀번호 검증. 같으면 1. */
int  pw_verify(const char *salt_hex, const char *hash_hex, const char *password);

#endif
