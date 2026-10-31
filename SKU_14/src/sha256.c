/* SHA-256 (FIPS 180-4) 과 솔트 반복 해싱 기반 비밀번호 저장
 *
 * SHA-256 동작 개요:
 *   1) 메시지를 64바이트(512비트) 블록으로 나눈다.
 *   2) 마지막 블록에는 패딩(0x80, 0들, 원래 길이)을 붙여 64바이트 배수로 맞춘다.
 *   3) 32비트 상태값 8개(h[0..7])를 초기값에서 시작해, 블록마다 64라운드의
 *      비트 연산(회전, XOR, AND, 덧셈)으로 섞는다.
 *   4) 모든 블록을 처리한 뒤의 상태값 8개(=32바이트)가 해시 결과다.
 * 표준 문서: NIST FIPS 180-4 "Secure Hash Standard"
 */
#include "sha256.h"

#include <string.h>   /* memcpy, memset, strlen */

#define PW_ITERATIONS 12000   /* 무작위 대입을 늦추기 위한 반복 횟수 */

/* 라운드 상수 K[0..63].
 * 처음 64개 소수(2, 3, 5, ..., 311)의 세제곱근에서 소수점 아래 32비트를 딴 값이다.
 * "임의로 고른 값이 아님" 을 보여 주는(nothing-up-my-sleeve) 상수로 표준에 정해져 있다. */
static const unsigned int K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

/* 32비트 오른쪽 회전(rotate right). 오른쪽으로 밀려 나간 n 비트가 왼쪽으로 다시 들어온다.
 * 예: ROR(0x00000001, 1) = 0x80000000.  (x 는 반드시 32비트 unsigned 여야 한다) */
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* 64바이트 블록 하나를 처리해 상태 h[8] 를 갱신한다 (압축 함수). */
static void sha256_block(unsigned int h[8], const unsigned char block[64])
{
    unsigned int w[64];                              /* 메시지 스케줄 (라운드마다 쓸 64개 워드) */
    unsigned int a, b, c, d, e, f, g, hh, t1, t2;    /* 작업 변수 (hh: 매개변수 h 와 이름 충돌 회피) */
    int i;

    /* 1단계: 블록 64바이트를 32비트 워드 16개로 읽는다.
     * SHA-256 은 빅엔디언(상위 바이트가 먼저)이므로 바이트를 직접 조립한다.
     * (x86 은 리틀엔디언이라 memcpy 로 읽으면 바이트 순서가 뒤집힌다.) */
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)block[i * 4] << 24) |
               ((unsigned int)block[i * 4 + 1] << 16) |
               ((unsigned int)block[i * 4 + 2] << 8) |
               ((unsigned int)block[i * 4 + 3]);

    /* 2단계: 나머지 48개 워드를 앞의 워드들로부터 확장한다.
     * σ0, σ1 함수(s0, s1)가 비트를 섞어 입력의 작은 변화가 퍼지게 한다. */
    for (i = 16; i < 64; i++) {
        unsigned int s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;   /* 덧셈은 2^32 로 나눈 나머지 (unsigned 오버플로) */
    }

    /* 3단계: 작업 변수를 현재 상태로 초기화 */
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    /* 4단계: 64 라운드 압축 */
    for (i = 0; i < 64; i++) {
        unsigned int S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);   /* Σ1(e) */
        unsigned int ch = (e & f) ^ ((~e) & g);                   /* Ch: e 가 1 이면 f, 0 이면 g 를 고른다 */
        unsigned int S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);   /* Σ0(a) */
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);           /* Maj: a,b,c 중 다수결 비트 */

        t1 = hh + S1 + ch + K[i] + w[i];
        t2 = S0 + maj;

        /* 변수들을 한 칸씩 밀고, 새 값 두 개(e, a)를 넣는다 */
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    /* 5단계: 이번 블록의 결과를 상태에 더한다 (다음 블록의 입력 상태가 됨) */
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* msg 전체의 SHA-256 을 계산한다. 메시지를 한 번에 메모리에 들고 있는 단순 버전. */
void sha256(const unsigned char *msg, size_t len, unsigned char out[SHA256_DIGEST_LEN])
{
    /* 초기 해시값: 처음 8개 소수(2..19)의 제곱근 소수부 32비트 (표준에 정해진 값) */
    unsigned int h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    unsigned char tail[128];   /* 마지막 자투리 + 패딩 (최대 블록 2개) */
    size_t full = len / 64;    /* 패딩 없이 바로 처리할 수 있는 완전한 블록 수 */
    size_t rest = len % 64;    /* 마지막에 남는 바이트 수 (0~63) */
    size_t tail_len;
    unsigned long long bits = (unsigned long long)len * 8;   /* 메시지 길이(비트 단위) */
    size_t i;

    /* 완전한 블록들은 원본에서 바로 처리 */
    for (i = 0; i < full; i++)
        sha256_block(h, msg + i * 64);

    /* 패딩: 0x80 한 바이트 + 0 들 + 길이 8바이트(빅엔디언)
     * 남은 바이트 + 0x80(1) + 길이(8) 가 64 안에 들어가면(rest < 56) 블록 1개,
     * 아니면 블록 2개(128바이트)가 필요하다. */
    memcpy(tail, msg + full * 64, rest);
    tail[rest] = 0x80;                                  /* 1 비트 하나를 붙이는 것과 같다 */
    tail_len = (rest < 56) ? 64 : 128;
    memset(tail + rest + 1, 0, tail_len - rest - 9);    /* 길이 자리(8) 앞까지 0 으로 */
    for (i = 0; i < 8; i++)                             /* 맨 끝 8바이트에 비트 길이를 빅엔디언으로 */
        tail[tail_len - 1 - i] = (unsigned char)(bits >> (8 * i));

    sha256_block(h, tail);
    if (tail_len == 128)
        sha256_block(h, tail + 64);

    /* 상태 8워드를 빅엔디언 바이트 32개로 내보낸다 */
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(h[i]);
    }
}

/* 바이트 배열을 소문자 16진 문자열로 바꾼다. out 은 n*2+1 바이트 이상. */
static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[i * 2]     = hex[in[i] >> 4];    /* 상위 4비트 */
        out[i * 2 + 1] = hex[in[i] & 0xF];   /* 하위 4비트 */
    }
    out[n * 2] = '\0';
}

/* 비밀번호 해시를 만든다.
 *   첫 회:  D0 = SHA256(salt || password)
 *   반복:  D(k+1) = SHA256(Dk || salt)   (PW_ITERATIONS 번)
 * 결과 D 를 16진 64글자로 out_hex 에 쓴다.
 * 같은 솔트·비밀번호면 항상 같은 결과가 나오므로 로그인 때 다시 계산해 비교할 수 있다. */
void pw_hash(const char *salt_hex, const char *password, char *out_hex)
{
    unsigned char digest[SHA256_DIGEST_LEN];   /* 현재 회차의 해시값 */
    unsigned char seed[512];                   /* 해시할 입력을 조립하는 버퍼 */
    size_t slen = strlen(salt_hex);
    size_t plen = strlen(password);
    int i;

    /* 솔트는 32글자로 고정이고 비밀번호 길이는 api.c 에서 제한하지만,
     * 혹시라도 버퍼를 넘지 않도록 한 번 더 막는다. */
    if (slen + plen > sizeof seed)
        plen = sizeof seed - slen;      /* 지나치게 긴 입력은 잘라낸다 */

    memcpy(seed, salt_hex, slen);
    memcpy(seed + slen, password, plen);
    sha256(seed, slen + plen, digest);

    /* 솔트를 매 회차에 다시 섞어 반복한다.
     * 반복 1회가 수 마이크로초라면 12000회는 수십 밀리초 - 로그인 한 번에는 티가 안 나지만,
     * 공격자가 비밀번호 후보 수억 개를 시험하려면 12000배 오래 걸린다. */
    for (i = 0; i < PW_ITERATIONS; i++) {
        memcpy(seed, digest, SHA256_DIGEST_LEN);
        memcpy(seed + SHA256_DIGEST_LEN, salt_hex, slen);
        sha256(seed, SHA256_DIGEST_LEN + slen, digest);
    }

    to_hex(digest, SHA256_DIGEST_LEN, out_hex);
}

/* 입력한 비밀번호가 저장된 해시와 맞는지 확인한다. 맞으면 1. */
int pw_verify(const char *salt_hex, const char *hash_hex, const char *password)
{
    char calc[PW_HASH_HEX_LEN + 1];   /* 입력 비밀번호로 다시 계산한 해시 */
    unsigned char diff = 0;           /* 다른 비트가 하나라도 있으면 0 이 아니게 된다 */
    int i;

    pw_hash(salt_hex, password, calc);

    /* 길이가 같으므로 상수 시간 비교로 맞춰 둔다.
     * strcmp 는 첫 번째로 다른 글자에서 바로 멈추기 때문에, 응답 시간을 정밀하게 재면
     * "몇 글자까지 맞았는지" 가 새어 나갈 수 있다(타이밍 공격).
     * 여기서는 64글자를 끝까지 XOR 해 누적하므로 맞든 틀리든 걸리는 시간이 같다. */
    for (i = 0; i < PW_HASH_HEX_LEN; i++)
        diff |= (unsigned char)(calc[i] ^ hash_hex[i]);
    return diff == 0;
}
