/* 기간이 지난 글 자동 정리 (규칙은 cleanup.h 참고)
 *
 * 웹 서버 모드에서는 백그라운드 스레드가 시작할 때 한 번, 이후 1시간마다 돌린다.
 * 날짜 비교는 MySQL 의 CURDATE() 를 쓰므로 DB 서버의 날짜 기준이다.
 */
#include "cleanup.h"
#include "common.h"
#include "crawler.h"
#include "db.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

#define CLEANUP_INTERVAL_MS (60u * 60u * 1000u)   /* 1시간 */
#define NO_DEADLINE_DAYS    60                     /* 마감일을 모르는 학교 공지를 남겨 둘 날 수 (기본값) */

/* SQL 하나를 실행하고 지운(바뀐) 행 수를 돌려준다. 실패 시 -1.
 * 다른 스레드의 질의가 끼어들면 db_affected 가 그쪽 값이 되므로 둘을 함께 잠근다. */
static long long exec_count(const char *sql)
{
    long long n = -1;

    db_lock();
    if (db_exec(sql))
        n = (long long)db_affected();
    db_unlock();
    return n;
}

void cleanup_ensure_schema(void)
{
    /* study_groups.end_date: 스터디 종료일 (sql/14_auto_cleanup.sql 과 같은 내용).
     * MySQL 8 은 ADD COLUMN IF NOT EXISTS 를 모르므로 information_schema 로 먼저 확인한다. */
    if (db_scalar("SELECT COUNT(*) FROM information_schema.columns "
                  "WHERE table_schema = DATABASE() AND table_name = 'study_groups' "
                  "AND column_name = 'end_date'", 1) == 0) {
        if (db_exec("ALTER TABLE study_groups ADD COLUMN end_date DATE NULL AFTER max_members"))
            log_info("study_groups 에 종료일(end_date) 열을 추가했습니다.");
        else
            log_warn("study_groups 에 end_date 열을 추가하지 못했습니다. sql/14_auto_cleanup.sql 을 적용하세요.");
    }
}

int cleanup_backfill_deadlines(void)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    int filled = 0;

    res = db_query("SELECT id, body, DATE_FORMAT(created_at, '%Y-%m-%d') FROM posts "
                   "WHERE deadline IS NULL AND author_id IS NULL");
    if (!res)
        return 0;

    while ((row = mysql_fetch_row(res)) != NULL) {
        char deadline[11];
        Buf sql;

        if (!notice_guess_deadline(row[1], row[2], deadline))
            continue;
        buf_init(&sql);
        db_sqlf(&sql, "UPDATE posts SET deadline = %Q WHERE id = %u",
                deadline, (unsigned)strtoul(row[0], NULL, 10));
        if (db_exec_buf(&sql))
            filled++;
        buf_free(&sql);
    }
    mysql_free_result(res);

    if (filled)
        log_info("학교 공지 게시물 %d건의 마감일을 본문에서 찾아 채웠습니다.", filled);
    return filled;
}

int cleanup_expired(void)
{
    long long no_deadline_days = db_scalar(
        "SELECT CAST(v AS SIGNED) FROM app_config WHERE k = 'cleanup.no_deadline_days'", NO_DEADLINE_DAYS);
    long long window_days = db_scalar(
        "SELECT CAST(v AS SIGNED) FROM app_config WHERE k = 'crawl.window_days'", 730);
    long long posts, recruits, studies, notices;
    char sql[512];

    if (no_deadline_days < 1)
        no_deadline_days = NO_DEADLINE_DAYS;

    /* 공모전: 마감일이 지났거나, 마감일을 모르는 학교 공지가 오래됐으면 */
    snprintf(sql, sizeof sql,
             "DELETE FROM posts WHERE deadline < CURDATE() "
             "OR (deadline IS NULL AND author_id IS NULL "
             "    AND created_at < CURDATE() - INTERVAL %lld DAY)", no_deadline_days);
    posts = exec_count(sql);

    /* 팀원 모집: 마감했거나 마감일이 지났으면 */
    recruits = exec_count("DELETE FROM recruits WHERE status = 'closed' OR deadline < CURDATE()");

    /* 스터디: 종료일이 지났으면 */
    studies = exec_count("DELETE FROM study_groups WHERE end_date < CURDATE()");

    /* 수집 기간(window_days) 밖의 공지 기록은 다시 수집될 일이 없으므로 지운다.
     * 게시물이 남아 있는 것은 공지 ↔ 게시물 연결을 위해 둔다. */
    snprintf(sql, sizeof sql,
             "DELETE FROM notices WHERE post_id IS NULL "
             "AND published_at < CURDATE() - INTERVAL %lld DAY", window_days + 30);
    notices = exec_count(sql);

    if (posts < 0 || recruits < 0 || studies < 0 || notices < 0) {
        log_warn("지난 글 정리 중 일부가 실패했습니다.");
        return -1;
    }
    if (posts || recruits || studies)
        log_info("지난 글 정리: 공모전 %lld, 팀원 모집 %lld, 스터디 %lld건 삭제", posts, recruits, studies);
    return (int)(posts + recruits + studies);
}

static void cleanup_thread(void *arg)
{
    (void)arg;
    db_thread_begin();
    cleanup_backfill_deadlines();
    for (;;) {
        cleanup_expired();
        sleep_ms(CLEANUP_INTERVAL_MS);
    }
}

void cleanup_start(void)
{
    if (!thread_start(cleanup_thread, NULL))
        log_warn("지난 글 자동 정리 스레드를 시작하지 못했습니다.");
}
