/* AI 추천과 분석 관리 API.
 *
 *   GET  /api/recommendations   로그인한 학생의 학과와 관련도가 높은 게시물
 *   GET  /api/ai/status         분석 진행 상황 (관리자)
 *   POST /api/ai/analyze        분석 시작 (관리자). {"force": true} 면 전체 다시 분석
 *
 * 추천은 post_ai_departments 에 저장된 점수만 읽는다. 여기서는 Claude 를 부르지 않는다.
 *
 * 관련 테이블:
 *   post_ai              게시물 하나당 한 행: 요약(summary), 태그(tags, JSON 배열 문자열),
 *                        주최(host), 마감일(deadline)
 *   post_ai_departments  (게시물, 학과) 쌍마다 한 행: 관련도 점수(score 0~100)와 이유(reason)
 */
#include "api.h"
#include "db.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 이 점수 이상인 학과만 "관련 있음" 으로 보고 추천한다. */
#define RECO_MIN_SCORE 50
#define RECO_LIMIT_MAX 30   /* 한 번에 돌려줄 수 있는 추천 게시물 최대 개수 */

/* GET /api/recommendations?limit=N
 * 응답 예: {"ai_enabled":true,"analyzed_count":42,"department_name":"컴퓨터공학과",
 *           "posts":[{"id":3,"title":"...","score":87,...}, ...]} */
static void handle_recommendations(Request *req, Response *res)
{
    CurrentUser u;
    char limit_s[8];     /* 쿼리의 limit 값 (문자열) */
    int limit;
    Buf sql, b;          /* sql: 질의문, b: 응답 JSON */
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;       /* JSON 배열에서 첫 원소 앞에는 쉼표를 안 붙이기 위한 플래그 */

    if (!auth_require(req, res, &u))   /* 로그인 필수 (학과를 알아야 하므로) */
        return;

    /* limit 이 없거나 범위를 벗어나면 기본 6개 */
    req_query(req, "limit", limit_s, sizeof limit_s);
    limit = atoi(limit_s);
    if (limit <= 0 || limit > RECO_LIMIT_MAX)
        limit = 6;

    buf_init(&b);
    buf_puts(&b, "{");
    json_kv_bool(&b, "ai_enabled", ai_enabled());
    buf_putc(&b, ',');
    json_kv_int(&b, "analyzed_count", db_scalar("SELECT COUNT(*) FROM post_ai", 0));
    buf_putc(&b, ',');

    /* 학과가 없는 사용자(관리자 등)는 추천할 기준이 없으므로 빈 목록 */
    if (!u.department_id) {
        json_kv_str(&b, "department_name", NULL);
        buf_puts(&b, ",\"posts\":[]}");
        res_json_buf(res, 200, &b);
        buf_free(&b);
        return;
    }
    json_kv_str(&b, "department_name", u.department_name);
    buf_puts(&b, ",\"posts\":[");

    /* 마감이 지난 글은 뒤로 보내고, 같은 조건이면 관련도와 최신순으로 정렬한다.
     *  - IFNULL(p.deadline, a.deadline): 관리자가 직접 입력한 마감일을 우선, 없으면 AI 가 추출한 값
     *  - ORDER BY 첫 기준은 "마감 지남" 여부(참=1, 거짓=0)라서, 0 인 글(아직 유효)이 앞에 온다
     *  - db_sqlf 에서 %% 는 리터럴 % 이므로 '%%Y-%%m-%%d' 는 MySQL 에 '%Y-%m-%d' 로 전달된다
     *  - 마지막 열은 상관 부분질의로 댓글 수를 센다 */
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT p.id, p.kind, p.title, a.summary, a.tags, ad.score, ad.reason, "
            "       IFNULL(DATE_FORMAT(IFNULL(p.deadline, a.deadline), '%%Y-%%m-%%d'), ''), "
            "       IFNULL(p.host, IFNULL(a.host, '')), "
            "       DATE_FORMAT(p.created_at, '%%Y-%%m-%%d %%H:%%i'), "
            "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id) "
            "FROM post_ai_departments ad "
            "JOIN posts p ON p.id = ad.post_id "
            "JOIN post_ai a ON a.post_id = p.id "
            "WHERE ad.department_id = %u AND ad.score >= %d "
            "ORDER BY (IFNULL(p.deadline, a.deadline) IS NOT NULL "
            "          AND IFNULL(p.deadline, a.deadline) < CURDATE()), "
            "         ad.score DESC, p.created_at DESC, p.id DESC "
            "LIMIT %d",
            u.department_id, RECO_MIN_SCORE, limit);
    qr = db_query_buf(&sql);
    buf_free(&sql);

    /* 결과 행 하나를 JSON 객체 하나로 변환. row[i] 는 SELECT 의 i 번째 열(문자열) */
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_putc(&b, '{');
            json_kv_int(&b, "id", atoll(row[0]));                        buf_putc(&b, ',');
            json_kv_str(&b, "kind", row[1]);                             buf_putc(&b, ',');
            json_kv_str(&b, "title", row[2]);                            buf_putc(&b, ',');
            json_kv_str(&b, "summary", row[3]);                          buf_putc(&b, ',');
            /* tags 는 서버(ai.c)가 json_write_str 로 만든 배열이라 그대로 싣는다.
             * 혹시 형식이 깨져 있으면(첫 글자가 '[' 가 아니면) 빈 배열로 대체해 JSON 이 망가지지 않게 한다. */
            buf_puts(&b, "\"tags\":");
            buf_puts(&b, row[4] && row[4][0] == '[' ? row[4] : "[]");    buf_putc(&b, ',');
            json_kv_int(&b, "score", atoll(row[5]));                     buf_putc(&b, ',');
            json_kv_str(&b, "reason", row[6]);                           buf_putc(&b, ',');
            /* 빈 문자열(IFNULL 로 바꾼 값)은 JSON null 로 내보낸다 */
            json_kv_str(&b, "deadline", row[7][0] ? row[7] : NULL);      buf_putc(&b, ',');
            json_kv_str(&b, "host", row[8][0] ? row[8] : NULL);          buf_putc(&b, ',');
            json_kv_str(&b, "created_at", row[9]);                       buf_putc(&b, ',');
            json_kv_int(&b, "comment_count", atoll(row[10]));
            buf_putc(&b, '}');
        }
        mysql_free_result(qr);
    }
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* 분석 진행 상황을 JSON 객체로 b 에 쓴다. (status 와 analyze 응답에서 공용) */
static void write_status(Buf *b)
{
    AiStatus s;

    ai_status(&s);   /* 백그라운드 스레드가 갱신하는 값을 안전하게 복사 */
    buf_putc(b, '{');
    json_kv_bool(b, "enabled", s.enabled);                            buf_putc(b, ',');
    json_kv_str(b, "model", AI_MODEL);                                buf_putc(b, ',');
    json_kv_bool(b, "running", s.running);                            buf_putc(b, ',');
    json_kv_int(b, "total", s.total);                                 buf_putc(b, ',');
    json_kv_int(b, "done", s.done);                                   buf_putc(b, ',');
    json_kv_int(b, "failed", s.failed);                               buf_putc(b, ',');
    json_kv_str(b, "last_error", s.last_error[0] ? s.last_error : NULL); buf_putc(b, ',');
    json_kv_str(b, "finished_at", s.finished_at[0] ? s.finished_at : NULL); buf_putc(b, ',');
    /* 전체 게시물 수 대비 분석된 수 → 관리자 화면에서 "42 / 50 분석됨" 처럼 표시 */
    json_kv_int(b, "post_count", db_scalar("SELECT COUNT(*) FROM posts", 0));
    buf_putc(b, ',');
    json_kv_int(b, "analyzed_count", db_scalar("SELECT COUNT(*) FROM post_ai", 0));
    buf_putc(b, '}');
}

/* GET /api/ai/status (관리자) */
static void handle_ai_status(Request *req, Response *res)
{
    CurrentUser u;
    Buf b;

    if (!auth_require_admin(req, res, &u))
        return;
    buf_init(&b);
    write_status(&b);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* POST /api/ai/analyze (관리자) - 백그라운드 분석 시작 */
static void handle_ai_analyze(Request *req, Response *res)
{
    CurrentUser u;
    int force = 0, started;
    Buf b;

    if (!auth_require_admin(req, res, &u))
        return;

    /* 본문은 선택 사항. 있으면 {"force":true} 를 읽는다 */
    if (req->body && req->body_len) {
        Json *in = body_object(req, res);
        if (!in)                 /* JSON 형식 오류 → body_object 가 400 을 채움 */
            return;
        force = json_bool(in, "force", 0);
        json_free(in);
    }

    started = ai_start_background(force);   /* 1 시작, 0 이미 실행 중, -1 AI 꺼짐 */
    if (started < 0) {
        res_error(res, 503, "ai_disabled",
                  "ANTHROPIC_API_KEY 가 설정되지 않아 AI 분석을 할 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"ok\":true,");
    json_kv_bool(&b, "started", started == 1);
    buf_puts(&b, ",\"status\":");
    write_status(&b);
    buf_putc(&b, '}');
    /* 새로 시작했으면 202 Accepted (작업이 접수되어 비동기로 진행 중이라는 뜻),
     * 이미 돌고 있었으면 200 */
    res_json_buf(res, started == 1 ? 202 : 200, &b);
    buf_free(&b);
}

/* 이 모듈이 담당하는 경로면 처리하고 1, 아니면 0 */
int route_ai(Request *req, Response *res)
{
    if (req_is(req, "GET", "/api/recommendations")) {
        handle_recommendations(req, res);
        return 1;
    }
    if (req_is(req, "GET", "/api/ai/status")) {
        handle_ai_status(req, res);
        return 1;
    }
    if (req_is(req, "POST", "/api/ai/analyze")) {
        handle_ai_analyze(req, res);
        return 1;
    }
    return 0;
}
