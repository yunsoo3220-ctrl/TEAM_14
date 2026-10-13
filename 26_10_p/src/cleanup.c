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
#define SOLD_BOOK_DAYS      30                     /* 판매 완료한 전공책을 남겨 둘 날 수 */
#define NOTIFY_KEEP_DAYS    90                     /* 알림을 남겨 둘 날 수 */

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

    /* study_groups.kind: 그룹 스터디 / 멘토링 구분 (sql/15_mentoring.sql 과 같은 내용) */
    if (db_scalar("SELECT COUNT(*) FROM information_schema.columns "
                  "WHERE table_schema = DATABASE() AND table_name = 'study_groups' "
                  "AND column_name = 'kind'", 1) == 0) {
        if (db_exec("ALTER TABLE study_groups ADD COLUMN kind ENUM('study','mentoring') "
                    "NOT NULL DEFAULT 'study' AFTER end_date"))
            log_info("study_groups 에 구분(kind) 열을 추가했습니다.");
        else
            log_warn("study_groups 에 kind 열을 추가하지 못했습니다. sql/15_mentoring.sql 을 적용하세요.");
    }

    /* users.suspended_until / suspend_reason: 관리자의 회원 정지 (sql/19_suspend.sql 과 같은 내용) */
    if (db_scalar("SELECT COUNT(*) FROM information_schema.columns "
                  "WHERE table_schema = DATABASE() AND table_name = 'users' "
                  "AND column_name = 'suspended_until'", 1) == 0) {
        if (db_exec("ALTER TABLE users ADD COLUMN suspended_until DATETIME NULL AFTER role, "
                    "ADD COLUMN suspend_reason VARCHAR(300) NOT NULL DEFAULT '' AFTER suspended_until"))
            log_info("users 에 정지(suspended_until, suspend_reason) 열을 추가했습니다.");
        else
            log_warn("users 에 정지 열을 추가하지 못했습니다. sql/19_suspend.sql 을 적용하세요.");
    }

    /* 전공책 장터 표 (sql/16_books.sql 과 같은 내용). IF NOT EXISTS 라 이미 있으면 아무 일도 하지 않는다. */
    if (!db_exec("CREATE TABLE IF NOT EXISTS books ("
                 "  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
                 "  seller_id INT UNSIGNED NOT NULL,"
                 "  title VARCHAR(200) NOT NULL,"
                 "  author VARCHAR(100) NOT NULL DEFAULT '',"
                 "  course VARCHAR(100) NOT NULL DEFAULT '',"
                 "  department_id INT UNSIGNED NULL,"
                 "  price INT UNSIGNED NOT NULL DEFAULT 0,"
                 "  book_condition ENUM('new','good','fair','poor') NOT NULL DEFAULT 'good',"
                 "  description VARCHAR(2000) NOT NULL DEFAULT '',"
                 "  status ENUM('selling','reserved','sold') NOT NULL DEFAULT 'selling',"
                 "  sold_at DATETIME NULL,"
                 "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                 "  PRIMARY KEY (id),"
                 "  KEY ix_books_status (status, created_at),"
                 "  KEY ix_books_seller (seller_id),"
                 "  KEY ix_books_department (department_id),"
                 "  CONSTRAINT fk_books_seller FOREIGN KEY (seller_id) REFERENCES users (id) ON DELETE CASCADE,"
                 "  CONSTRAINT fk_books_department FOREIGN KEY (department_id) REFERENCES departments (id) ON DELETE SET NULL"
                 ") ENGINE=InnoDB") ||
        !db_exec("CREATE TABLE IF NOT EXISTS book_photos ("
                 "  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
                 "  book_id INT UNSIGNED NOT NULL,"
                 "  sort_order TINYINT UNSIGNED NOT NULL DEFAULT 0,"
                 "  mime VARCHAR(20) NOT NULL,"
                 "  data MEDIUMBLOB NOT NULL,"
                 "  PRIMARY KEY (id),"
                 "  KEY ix_book_photos_book (book_id, sort_order),"
                 "  CONSTRAINT fk_bp_book FOREIGN KEY (book_id) REFERENCES books (id) ON DELETE CASCADE"
                 ") ENGINE=InnoDB") ||
        !db_exec("CREATE TABLE IF NOT EXISTS book_comments ("
                 "  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
                 "  book_id INT UNSIGNED NOT NULL,"
                 "  author_id INT UNSIGNED NOT NULL,"
                 "  body VARCHAR(1000) NOT NULL,"
                 "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                 "  PRIMARY KEY (id),"
                 "  KEY ix_book_comments (book_id, created_at),"
                 "  CONSTRAINT fk_bc_book FOREIGN KEY (book_id) REFERENCES books (id) ON DELETE CASCADE,"
                 "  CONSTRAINT fk_bc_author FOREIGN KEY (author_id) REFERENCES users (id) ON DELETE CASCADE"
                 ") ENGINE=InnoDB"))
        log_warn("전공책 장터 표를 만들지 못했습니다. sql/16_books.sql 을 적용하세요.");

    /* 참여 권유 기록 (sql/17_invitations.sql 과 같은 내용) */
    if (!db_exec("CREATE TABLE IF NOT EXISTS invitations ("
                 "  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
                 "  target_kind ENUM('recruit','study') NOT NULL,"
                 "  target_id INT UNSIGNED NOT NULL,"
                 "  inviter_id INT UNSIGNED NOT NULL,"
                 "  invitee_id INT UNSIGNED NOT NULL,"
                 "  accepted_at DATETIME NULL,"
                 "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                 "  PRIMARY KEY (id),"
                 "  UNIQUE KEY uq_invitations (target_kind, target_id, invitee_id),"
                 "  KEY ix_invitations_inviter (inviter_id, created_at),"
                 "  CONSTRAINT fk_inv_inviter FOREIGN KEY (inviter_id) REFERENCES users (id) ON DELETE CASCADE,"
                 "  CONSTRAINT fk_inv_invitee FOREIGN KEY (invitee_id) REFERENCES users (id) ON DELETE CASCADE"
                 ") ENGINE=InnoDB"))
        log_warn("참여 권유 표를 만들지 못했습니다. sql/17_invitations.sql 을 적용하세요.");
    /* invitations.accepted_at: 알림함에서 [참여하기] 를 누른 시각 (표가 먼저 만들어진 DB 용) */
    if (db_scalar("SELECT COUNT(*) FROM information_schema.columns "
                  "WHERE table_schema = DATABASE() AND table_name = 'invitations' "
                  "AND column_name = 'accepted_at'", 1) == 0) {
        if (db_exec("ALTER TABLE invitations ADD COLUMN accepted_at DATETIME NULL AFTER invitee_id"))
            log_info("invitations 에 수락 시각(accepted_at) 열을 추가했습니다.");
        else
            log_warn("invitations 에 accepted_at 열을 추가하지 못했습니다. sql/17_invitations.sql 을 보세요.");
    }

    /* 알림함 (sql/18_notifications.sql 과 같은 내용) */
    if (!db_exec("CREATE TABLE IF NOT EXISTS notifications ("
                 "  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
                 "  user_id INT UNSIGNED NOT NULL,"
                 "  actor_id INT UNSIGNED NULL,"
                 "  kind VARCHAR(20) NOT NULL,"
                 "  title VARCHAR(300) NOT NULL,"
                 "  body VARCHAR(1000) NOT NULL DEFAULT '',"
                 "  link VARCHAR(200) NOT NULL DEFAULT '',"
                 "  read_at DATETIME NULL,"
                 "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                 "  PRIMARY KEY (id),"
                 "  KEY ix_notifications_user (user_id, read_at, created_at),"
                 "  CONSTRAINT fk_noti_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,"
                 "  CONSTRAINT fk_noti_actor FOREIGN KEY (actor_id) REFERENCES users (id) ON DELETE SET NULL"
                 ") ENGINE=InnoDB"))
        log_warn("알림함 표를 만들지 못했습니다. sql/18_notifications.sql 을 적용하세요.");
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
    long long posts, recruits, studies, books, notices;
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

    /* 전공책: 판매 완료한 지 SOLD_BOOK_DAYS 일이 지났으면 (사진·댓글도 함께) */
    snprintf(sql, sizeof sql,
             "DELETE FROM books WHERE status = 'sold' AND sold_at < NOW() - INTERVAL %d DAY", SOLD_BOOK_DAYS);
    books = exec_count(sql);

    /* 참여 권유: 대상 모집글·스터디가 지워졌으면 기록도 지운다 (대상이 두 종류라 외래 키를 걸 수 없다) */
    exec_count("DELETE i FROM invitations i LEFT JOIN recruits r ON r.id = i.target_id "
               "WHERE i.target_kind = 'recruit' AND r.id IS NULL");
    exec_count("DELETE i FROM invitations i LEFT JOIN study_groups g ON g.id = i.target_id "
               "WHERE i.target_kind = 'study' AND g.id IS NULL");

    /* 알림함: 오래된 알림 */
    snprintf(sql, sizeof sql,
             "DELETE FROM notifications WHERE created_at < NOW() - INTERVAL %d DAY", NOTIFY_KEEP_DAYS);
    exec_count(sql);

    /* 수집 기간(window_days) 밖의 공지 기록은 다시 수집될 일이 없으므로 지운다.
     * 게시물이 남아 있는 것은 공지 ↔ 게시물 연결을 위해 둔다. */
    snprintf(sql, sizeof sql,
             "DELETE FROM notices WHERE post_id IS NULL "
             "AND published_at < CURDATE() - INTERVAL %lld DAY", window_days + 30);
    notices = exec_count(sql);

    if (posts < 0 || recruits < 0 || studies < 0 || books < 0 || notices < 0) {
        log_warn("지난 글 정리 중 일부가 실패했습니다.");
        return -1;
    }
    if (posts || recruits || studies || books)
        log_info("지난 글 정리: 공모전 %lld, 팀원 모집 %lld, 스터디 %lld, 전공책 %lld건 삭제",
                 posts, recruits, studies, books);
    return (int)(posts + recruits + studies + books);
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
