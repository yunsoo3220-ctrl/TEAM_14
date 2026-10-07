/* MySQL 접근 계층.
 *
 * 연결은 하나만 열고 재진입 가능한 임계구역으로 감싼다. 요청마다 스레드가
 * 생기는 구조이므로 공유 연결을 동시에 쓰면 안 되고, 과제 규모의 동시 접속에는
 * 직렬화로 충분하다. 트랜잭션처럼 여러 질의를 묶어야 할 때는 db_lock/db_unlock
 * 으로 바깥을 한 번 더 잠그면 된다. */
#ifndef SKU_DB_H
#define SKU_DB_H

#include "common.h"
#include <mysql.h>

typedef struct {
    char         host[128];
    unsigned int port;
    char         user[64];
    char         pass[128];
    char         name[64];
} DbConfig;

int  db_init(const DbConfig *cfg);
void db_close(void);

/* 스레드마다 시작/끝에 한 번 호출 (MySQL 클라이언트 라이브러리 요구사항) */
void db_thread_begin(void);
void db_thread_end(void);

void db_lock(void);
void db_unlock(void);

/* SQL 조립. 지원하는 지정자:
 *   %Q  문자열을 이스케이프해 작은따옴표로 감싼다 (NULL 이면 NULL 리터럴)
 *   %s  이스케이프 없이 그대로 (검증된 식별자/열거값에만 쓸 것)
 *   %d  int, %u  unsigned, %L  long long
 *   %%  리터럴 % */
void db_sqlf(Buf *b, const char *fmt, ...);
/* 문자열 하나를 'escaped' 형태로 b 에 추가 */
void db_quote(Buf *b, const char *s);

/* 질의 실행. db_exec 는 성공 시 1, db_query 는 결과셋(해제는 호출자) 또는 NULL. */
int        db_exec(const char *sql);
MYSQL_RES *db_query(const char *sql);

/* Buf 에 조립한 SQL 을 바로 실행하는 편의 함수 (Buf 는 비우지 않는다) */
int        db_exec_buf(Buf *b);
MYSQL_RES *db_query_buf(Buf *b);

unsigned long long db_last_id(void);
unsigned long long db_affected(void);
const char        *db_error(void);
/* 마지막 실패의 MySQL 오류 번호. 유일 키 위반은 ER_DUP_ENTRY(1062). */
unsigned int       db_errno(void);

#define DB_ERR_DUP_ENTRY 1062

/* 단일 값 조회 도우미. 결과가 없으면 def 를 돌려준다. */
long long  db_scalar(const char *sql, long long def);

/* 트랜잭션 */
int  db_begin(void);
int  db_commit(void);
void db_rollback(void);

#endif
