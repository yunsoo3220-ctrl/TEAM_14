/* MySQL 접근 계층.
 *
 * 연결은 하나만 열고 재진입 가능한 임계구역으로 감싼다. 요청마다 스레드가
 * 생기는 구조이므로 공유 연결을 동시에 쓰면 안 되고, 과제 규모의 동시 접속에는
 * 직렬화로 충분하다. 트랜잭션처럼 여러 질의를 묶어야 할 때는 db_lock/db_unlock
 * 으로 바깥을 한 번 더 잠그면 된다.
 *
 * "재진입 가능(reentrant)" 하다는 것은 같은 스레드가 이미 잠근 상태에서 다시
 * 잠가도 교착(deadlock)되지 않는다는 뜻이다. Windows 의 CRITICAL_SECTION 이 이런
 * 성질을 가지므로, db_lock 으로 바깥을 잠근 뒤 안에서 db_exec 를 불러도 괜찮다.
 *
 * SQL 인젝션 방지: 사용자가 보낸 문자열은 반드시 db_sqlf 의 %Q 또는 db_quote 로
 * 이스케이프해서 SQL 에 넣는다. %s 는 코드에서 직접 정한 상수에만 쓴다.
 */
#ifndef SKU_DB_H
#define SKU_DB_H

#include "common.h"
#include <mysql.h>   /* MySQL/MariaDB C 클라이언트 라이브러리 */

/* DB 접속 정보 (main.c 에서 명령줄 옵션/환경변수로 채운다) */
typedef struct {
    char         host[128];   /* 서버 주소 (보통 127.0.0.1) */
    unsigned int port;        /* 포트 (기본 3306) */
    char         user[64];    /* 접속 계정 */
    char         pass[128];   /* 비밀번호 */
    char         name[64];    /* 사용할 데이터베이스(스키마) 이름 */
} DbConfig;

/* 접속을 열고 문자셋(utf8mb4)을 설정한다. 성공 1, 실패 0. 프로그램 시작 시 한 번. */
int  db_init(const DbConfig *cfg);
/* 접속을 닫고 라이브러리 자원을 정리한다. 프로그램 종료 시 한 번. */
void db_close(void);

/* 스레드마다 시작/끝에 한 번 호출 (MySQL 클라이언트 라이브러리 요구사항)
 * 라이브러리가 스레드별 내부 상태를 만들고 정리할 수 있게 해 준다.
 * 빠뜨리면 스레드가 끝날 때 메모리가 샌다. */
void db_thread_begin(void);
void db_thread_end(void);

/* 공유 연결을 독점한다. 여러 질의를 다른 스레드와 섞이지 않게 묶을 때 쓴다.
 * 반드시 lock 한 횟수만큼 unlock 해야 한다. */
void db_lock(void);
void db_unlock(void);

/* SQL 조립. 지원하는 지정자:
 *   %Q  문자열을 이스케이프해 작은따옴표로 감싼다 (NULL 이면 NULL 리터럴)
 *   %s  이스케이프 없이 그대로 (검증된 식별자/열거값에만 쓸 것)
 *   %d  int, %u  unsigned, %L  long long
 *   %%  리터럴 %
 * 예: db_sqlf(&sql, "SELECT id FROM users WHERE student_no=%Q", no);
 *     → SELECT id FROM users WHERE student_no='2024\'001'  (따옴표가 이스케이프됨) */
void db_sqlf(Buf *b, const char *fmt, ...);
/* 문자열 하나를 'escaped' 형태로 b 에 추가 */
void db_quote(Buf *b, const char *s);

/* 질의 실행. db_exec 는 성공 시 1, db_query 는 결과셋(해제는 호출자) 또는 NULL.
 * db_query 결과는 mysql_fetch_row 로 한 행씩 읽고 mysql_free_result 로 해제한다. */
int        db_exec(const char *sql);
MYSQL_RES *db_query(const char *sql);

/* Buf 에 조립한 SQL 을 바로 실행하는 편의 함수 (Buf 는 비우지 않는다) */
int        db_exec_buf(Buf *b);
MYSQL_RES *db_query_buf(Buf *b);

/* 마지막 INSERT 가 만든 AUTO_INCREMENT id */
unsigned long long db_last_id(void);
/* 마지막 INSERT/UPDATE/DELETE 가 바꾼 행 수 */
unsigned long long db_affected(void);
/* 마지막 오류 메시지 (사람이 읽는 문자열, 로그용) */
const char        *db_error(void);
/* 마지막 실패의 MySQL 오류 번호. 유일 키 위반은 ER_DUP_ENTRY(1062). */
unsigned int       db_errno(void);

/* 유일 키(UNIQUE) 중복 오류 번호. 예: 이미 가입된 학번으로 가입 시도 */
#define DB_ERR_DUP_ENTRY 1062

/* 단일 값 조회 도우미. 결과가 없으면 def 를 돌려준다.
 * 예: db_scalar("SELECT COUNT(*) FROM posts", 0) */
long long  db_scalar(const char *sql, long long def);

/* 트랜잭션
 * 여러 질의를 "모두 성공 아니면 모두 취소" 로 묶는다.
 * db_begin 이 스스로 연결을 잠그고, db_commit/db_rollback 이 그 잠금을 푼다.
 * 그래서 트랜잭션 도중에 다른 스레드의 질의가 끼어들지 않는다.
 * 사용 예: if (db_begin()) { ...질의들... ok ? db_commit() : db_rollback(); } */
int  db_begin(void);     /* START TRANSACTION. 성공 1 (실패하면 잠금도 풀고 0) */
int  db_commit(void);    /* COMMIT 후 잠금 해제. 성공 1 */
void db_rollback(void);  /* ROLLBACK 후 잠금 해제 - 트랜잭션 안에서 한 변경을 모두 되돌린다 */

#endif /* SKU_DB_H */
