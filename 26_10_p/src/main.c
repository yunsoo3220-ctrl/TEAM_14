/* 서경대학교 공모전 팀원 모집 커뮤니티 - 서버 진입점
 *
 * 사용법:
 *   server.exe [옵션]
 *     --port N           HTTP 포트 (기본 80)
 *     --bind-all         모든 인터페이스에 바인드 (기본은 127.0.0.1 전용)
 *     --host NAME        정규 호스트 이름 (기본 www.sku14.com)
 *     --db-host H        MySQL 호스트 (기본 127.0.0.1)
 *     --db-port N        MySQL 포트 (기본 3306)
 *     --db-user U        MySQL 사용자 (기본 root)
 *     --db-pass P        MySQL 비밀번호 (환경변수 SKU_DB_PASS 가 우선 기본값)
 *     --db-name N        데이터베이스 이름 (기본 sku_contest)
 *     --webroot DIR      정적 파일 디렉터리 (기본 www)
 *     --add-admin 학번 이름 비밀번호   관리자 계정을 만들고 종료
 *     --crawl            공지를 한 번 수집하고 종료
 *     --ai-analyze       아직 분석하지 않은 게시물을 AI 로 분석하고 종료
 *                        (--force 를 붙이면 전체를 다시 분석)
 *     --test-mail 주소   SMTP 설정(SKU_SMTP_*)으로 시험 메일을 한 통 보내고 종료
 *
 * AI 기능은 환경변수 ANTHROPIC_API_KEY 가 있을 때만 켜진다.
 *
 * 실행 순서 요약:
 *   1) (--test-mail 이면 DB 없이 메일만 보내고 끝)
 *   2) 기본 설정값 채우기 → 명령줄 인자로 덮어쓰기
 *   3) DB 접속
 *   4) 일회성 작업 모드(--add-admin / --crawl / --ai-analyze)면 그 작업만 하고 종료
 *   5) 아니면 기초 데이터·기능 상태를 점검해 로그로 알리고 HTTP 서버를 띄운다(무한 대기)
 */
#include "api.h"
#include "db.h"
#include "ai.h"
#include "crawler.h"
#include "http.h"
#include "mailer.h"

#include <windows.h>   /* SetConsoleOutputCP */
#include <stdio.h>
#include <stdlib.h>    /* getenv, atoi */
#include <string.h>

/* 도움말 출력 (--help 또는 잘못된 인자일 때) */
static void usage(void)
{
    printf(
        "서경대 공모전 커뮤니티 서버\n"
        "\n"
        "  server.exe [--port 80] [--bind-all] [--host www.sku14.com]\n"
        "             [--db-host 127.0.0.1] [--db-port 3306]\n"
        "             [--db-user root] [--db-pass ****] [--db-name sku_contest]\n"
        "             [--webroot www]\n"
        "  server.exe --add-admin <학번> <이름> <비밀번호>\n"
        "  server.exe --crawl\n"
        "  server.exe --ai-analyze [--force]\n"
        "  server.exe --test-mail 받을주소@skuniv.ac.kr\n"
        "\n"
        "DB 비밀번호는 환경변수 SKU_DB_PASS 로 주는 것을 권합니다.\n"
        "AI 분석·추천은 환경변수 ANTHROPIC_API_KEY 가 있을 때만 켜집니다.\n"
        "www.sku14.com 으로 열려면 hosts 파일에 아래 두 줄이 있어야 합니다.\n"
        "  127.0.0.1  www.sku14.com\n"
        "  127.0.0.1  sku14.com\n"
        "scripts\\setup-domain.ps1 을 관리자 권한으로 실행하면 자동으로 넣어줍니다.\n");
}

int main(int argc, char **argv)
{
    DbConfig db;                                  /* DB 접속 정보 */
    unsigned short port = 80;                     /* HTTP 포트 (80 이면 주소에 포트를 안 써도 됨) */
    const char *webroot = "www";                  /* index.html 등이 있는 폴더 */
    const char *canonical_host = "www.sku14.com"; /* 대표 도메인 */
    const char *env_pass;                         /* 환경변수 SKU_DB_PASS 값 */
    int bind_all = 0;                             /* 1 이면 외부 접속 허용 */
    int do_crawl = 0;                             /* --crawl 모드 */
    int do_ai = 0, ai_force = 0;                  /* --ai-analyze 모드, --force 여부 */
    const char *admin_no = NULL, *admin_name = NULL, *admin_pw = NULL;   /* --add-admin 인자 */
    int i;

    /* 콘솔과 소스 파일 모두 UTF-8 이므로 출력 코드페이지를 맞춘다.
     * (한국어 Windows 콘솔의 기본은 CP949 라서, 이것이 없으면 한글 로그가 깨져 보인다) */
    SetConsoleOutputCP(CP_UTF8);

    /* --test-mail: DB 없이 SMTP 설정만 시험한다. */
    if (argc == 3 && !strcmp(argv[1], "--test-mail")) {
        char err[200];
        if (!mail_enabled()) {
            log_err("SKU_SMTP_URL 이 없습니다. smtp.env 를 채우고 scripts\\send-test-mail.ps1 로 실행하세요.");
            return 1;
        }
        if (!mail_send(argv[2], "[SKU14] 메일 발송 시험",
                       "SKU14 서버의 메일 설정이 올바르게 동작합니다.\n"
                       "이 메일을 받았다면 가입 인증 코드도 같은 경로로 발송됩니다.\n", err, sizeof err)) {
            log_err("%s", err);
            return 1;
        }
        log_info("시험 메일을 보냈습니다: %s", argv[2]);
        return 0;
    }

    /* 1) 기본값 */
    memset(&db, 0, sizeof db);
    str_copy(db.host, sizeof db.host, "127.0.0.1");
    db.port = 3306;
    str_copy(db.user, sizeof db.user, "root");
    str_copy(db.name, sizeof db.name, "sku_contest");

    /* 비밀번호는 환경변수로 받는 것이 기본이다. 명령줄 인자는 작업 관리자 등에서
     * 다른 사용자에게 보일 수 있고 셸 기록에도 남기 때문이다. */
    env_pass = getenv("SKU_DB_PASS");
    if (env_pass)
        str_copy(db.pass, sizeof db.pass, env_pass);

    /* 2) 명령줄 인자 해석. 값을 받는 옵션은 "i + 1 < argc" 로 다음 인자가 있는지 먼저 확인하고,
     *    argv[++i] 로 값을 읽으면서 인덱스를 한 칸 더 넘긴다. */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(a, "--crawl")) {
            do_crawl = 1;
        } else if (!strcmp(a, "--ai-analyze")) {
            do_ai = 1;
        } else if (!strcmp(a, "--force")) {
            ai_force = 1;
        } else if (!strcmp(a, "--add-admin") && i + 3 < argc) {   /* 값 3개 필요 */
            admin_no   = argv[++i];
            admin_name = argv[++i];
            admin_pw   = argv[++i];
        } else if (!strcmp(a, "--port") && i + 1 < argc) {
            port = (unsigned short)atoi(argv[++i]);   /* 숫자가 아니면 0 → 아래에서 거부 */
        } else if (!strcmp(a, "--bind-all")) {
            bind_all = 1;
        } else if (!strcmp(a, "--host") && i + 1 < argc) {
            canonical_host = argv[++i];
        } else if (!strcmp(a, "--db-host") && i + 1 < argc) {
            str_copy(db.host, sizeof db.host, argv[++i]);
        } else if (!strcmp(a, "--db-port") && i + 1 < argc) {
            db.port = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(a, "--db-user") && i + 1 < argc) {
            str_copy(db.user, sizeof db.user, argv[++i]);
        } else if (!strcmp(a, "--db-pass") && i + 1 < argc) {
            str_copy(db.pass, sizeof db.pass, argv[++i]);
        } else if (!strcmp(a, "--db-name") && i + 1 < argc) {
            str_copy(db.name, sizeof db.name, argv[++i]);
        } else if (!strcmp(a, "--webroot") && i + 1 < argc) {
            webroot = argv[++i];
        } else {
            /* 모르는 옵션이거나 값이 빠진 옵션 */
            log_err("알 수 없는 인자: %s", a);
            usage();
            return 2;   /* 종료 코드 2 = 사용법 오류 (관례) */
        }
    }

    if (!port) {
        log_err("포트 번호가 올바르지 않습니다.");
        return 2;
    }

    /* 3) DB 접속 - 아래 모든 모드가 DB 를 필요로 한다 */
    if (!db_init(&db))
        return 1;

    /* 4) 일회성 작업 모드들 */

    /* --add-admin: 계정만 만들고 종료 */
    if (admin_no) {
        int ok = api_create_admin(admin_no, admin_name, admin_pw);
        db_close();
        return ok ? 0 : 1;
    }

    /* --crawl: 공지 수집만 하고 종료 (작업 스케줄러에 걸어 쓸 수 있다) */
    if (do_crawl) {
        CrawlResult cr;
        /* 수집 기간(일)은 DB 설정값을 쓰고, 설정이 없으면 730일(약 2년) */
        int window = (int)db_scalar(
            "SELECT CAST(v AS SIGNED) FROM app_config WHERE k = 'crawl.window_days'", 730);
        int ok = crawl_notices(window, &cr);

        if (!ok)
            log_err("수집 실패: %s", cr.error);
        db_close();
        return ok ? 0 : 1;
    }

    /* --ai-analyze: 분석만 하고 종료 */
    if (do_ai) {
        int failed = ai_run_sync(ai_force);
        db_close();
        return failed == 0 ? 0 : 1;   /* 하나라도 실패하면 종료 코드 1 */
    }

    /* 5) 웹 서버 모드 */
    api_set_webroot(webroot);
    api_set_canonical_host(canonical_host, port);

    /* 서버를 띄우기 전에 기초 데이터가 들어있는지 확인한다.
     * (없어도 서버는 뜨지만 회원가입·관리가 안 되므로 경고로 알려 준다) */
    if (db_scalar("SELECT COUNT(*) FROM departments", 0) == 0)
        log_warn("학과 데이터가 비어 있습니다. sql/02_seed_departments.sql 을 먼저 적용하세요.");
    if (db_scalar("SELECT COUNT(*) FROM users WHERE role = 'admin'", 0) == 0)
        log_warn("관리자 계정이 없습니다. server.exe --add-admin <학번> <이름> <비밀번호>");

    /* 선택 기능들의 켜짐/꺼짐 상태를 로그로 알린다 */
    if (ai_enabled())
        log_info("AI 분석·추천 켜짐 (%s)", AI_MODEL);
    else
        log_warn("ANTHROPIC_API_KEY 가 없어 AI 분석·추천이 꺼져 있습니다.");

    if (mail_enabled())
        log_info("가입 인증 메일 켜짐 (%s)", getenv("SKU_SMTP_URL"));
    else if (mail_dev_mode())
        log_warn("메일 개발 모드: 인증 코드를 보내지 않고 서버 로그에만 남깁니다 (SKU_MAIL_DEV=1).");
    else
        log_warn("메일 서버(SKU_SMTP_URL)가 없어 회원가입 인증 메일을 보낼 수 없습니다. smtp.env 를 채우세요.");

    log_info("정적 파일 루트: %s", webroot);
    if (port == 80)
        log_info("접속 주소: http://%s/", canonical_host);
    else
        log_info("접속 주소: http://%s:%u/", canonical_host, port);

    /* HTTP 서버 실행. 정상이라면 여기서 영원히 머문다(돌아오면 시작 실패). */
    if (!http_serve(port, bind_all, api_dispatch)) {
        db_close();
        return 1;
    }

    db_close();
    return 0;
}
