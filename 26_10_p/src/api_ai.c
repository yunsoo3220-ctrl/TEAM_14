/* AI 추천과 분석 관리 API.
 *
 *   GET  /api/recommendations   로그인한 학생의 학과와 관련도가 높은 게시물
 *   GET  /api/ai/status         분석 진행 상황 (관리자)
 *   POST /api/ai/analyze        분석 시작 (관리자). {"force": true} 면 전체 다시 분석
 *
 * 추천은 post_ai_departments 에 저장된 점수만 읽는다. 여기서는 Claude 를 부르지 않는다. */
#include "api.h"
#include "db.h"
#include "ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 이 점수 이상인 학과만 "관련 있음" 으로 보고 추천한다. */
#define RECO_MIN_SCORE 50
#define RECO_LIMIT_MAX 30

static void handle_recommendations(Request *req, Response *res)
{
    CurrentUser u;
    char limit_s[8];
    int limit;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    if (!auth_require(req, res, &u))
        return;

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

    if (!u.department_id) {
        json_kv_str(&b, "department_name", NULL);
        buf_puts(&b, ",\"posts\":[]}");
        res_json_buf(res, 200, &b);
        buf_free(&b);
        return;
    }
    json_kv_str(&b, "department_name", u.department_name);
    buf_puts(&b, ",\"posts\":[");

    /* 마감이 지난 글은 뒤로 보내고, 같은 조건이면 관련도와 최신순으로 정렬한다. */
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
            /* tags 는 서버(ai.c)가 json_write_str 로 만든 배열이라 그대로 싣는다. */
            buf_puts(&b, "\"tags\":");
            buf_puts(&b, row[4] && row[4][0] == '[' ? row[4] : "[]");    buf_putc(&b, ',');
            json_kv_int(&b, "score", atoll(row[5]));                     buf_putc(&b, ',');
            json_kv_str(&b, "reason", row[6]);                           buf_putc(&b, ',');
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

static void write_status(Buf *b)
{
    AiStatus s;

    ai_status(&s);
    buf_putc(b, '{');
    json_kv_bool(b, "enabled", s.enabled);                            buf_putc(b, ',');
    json_kv_str(b, "model", AI_MODEL);                                buf_putc(b, ',');
    json_kv_bool(b, "running", s.running);                            buf_putc(b, ',');
    json_kv_int(b, "total", s.total);                                 buf_putc(b, ',');
    json_kv_int(b, "done", s.done);                                   buf_putc(b, ',');
    json_kv_int(b, "failed", s.failed);                               buf_putc(b, ',');
    json_kv_str(b, "last_error", s.last_error[0] ? s.last_error : NULL); buf_putc(b, ',');
    json_kv_str(b, "finished_at", s.finished_at[0] ? s.finished_at : NULL); buf_putc(b, ',');
    json_kv_int(b, "post_count", db_scalar("SELECT COUNT(*) FROM posts", 0));
    buf_putc(b, ',');
    json_kv_int(b, "analyzed_count", db_scalar("SELECT COUNT(*) FROM post_ai", 0));
    buf_putc(b, '}');
}

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

static void handle_ai_analyze(Request *req, Response *res)
{
    CurrentUser u;
    int force = 0, started;
    Buf b;

    if (!auth_require_admin(req, res, &u))
        return;

    if (req->body && req->body_len) {
        Json *in = body_object(req, res);
        if (!in)
            return;
        force = json_bool(in, "force", 0);
        json_free(in);
    }

    started = ai_start_background(force);
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
    res_json_buf(res, started == 1 ? 202 : 200, &b);
    buf_free(&b);
}

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
