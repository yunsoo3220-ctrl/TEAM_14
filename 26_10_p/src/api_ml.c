/* 자체 추천 모델 API (src/ml.c).
 *
 *   GET  /api/related?department_id=&limit=   학과와 관련 있는 게시물 (누구나)
 *        department_id 를 빼면 로그인한 학생의 학과를 쓴다.
 *   GET  /api/ml/info                          모델 상태 (누구나)
 *   POST /api/ml/retrain                       강제 재학습 (관리자)
 *   POST /api/ml/predict  {title, body}        아직 올리지 않은 글이 학과마다 맞을 확률 (관리자)
 *        재학습 없이 지금 모델로 바로 계산한다. 글쓰기 화면에서 입력하는 동안 부른다. */
#include "api.h"
#include "db.h"
#include "ml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RELATED_MIN_SCORE 25      /* 이 점수 미만은 관련 없음으로 본다 */
#define RELATED_MAX       60

static void write_info(Buf *b, const MlInfo *info)
{
    buf_putc(b, '{');
    json_kv_str(b, "name", info->dim ? "SKU14 자체 모델 (딥러닝 임베딩 + TF-IDF + Rocchio)"
                                     : "SKU14 자체 모델 (TF-IDF + Rocchio)");   buf_putc(b, ',');
    json_kv_str(b, "embedding_model", info->dim ? info->embed_model : NULL);  buf_putc(b, ',');
    json_kv_int(b, "embedding_dim", info->dim);                    buf_putc(b, ',');
    json_kv_int(b, "posts", info->posts);                          buf_putc(b, ',');
    json_kv_int(b, "departments", info->departments);              buf_putc(b, ',');
    json_kv_int(b, "labels", info->labels);                        buf_putc(b, ',');
    buf_printf(b, "\"calibration\":{\"a\":%.4f,\"b\":%.4f,", info->pa, info->pb);
    json_kv_int(b, "positives", info->positives);                  buf_putc(b, ',');
    json_kv_int(b, "negatives", info->negatives);                  buf_puts(b, "},");
    json_kv_str(b, "trained_at", info->trained_at[0] ? info->trained_at : NULL);
    buf_putc(b, '}');
}

#define PREDICT_MAX     64
#define PREDICT_TEXT_MAX 60000

static void handle_predict(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    const char *title, *body;
    MlHit hits[PREDICT_MAX];
    MlInfo info;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    Buf b;
    int n, i;

    if (!auth_require_admin(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;
    title = json_str(in, "title", "");
    body = json_str(in, "body", "");
    if (strlen(title) + strlen(body) > PREDICT_TEXT_MAX) {
        json_free(in);
        res_error(res, 400, "too_long", "글이 너무 깁니다.");
        return;
    }

    memset(&info, 0, sizeof info);
    n = ml_predict_departments(title, body, hits, PREDICT_MAX, &info);
    json_free(in);
    if (n < 0) {
        res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
        return;
    }

    /* 학과 이름은 한 번에 읽어 둔다 (학과 수십 개). */
    qr = db_query("SELECT d.id, d.name, c.name FROM departments d JOIN colleges c ON c.id = d.college_id");

    buf_init(&b);
    buf_puts(&b, "{\"model\":");
    write_info(&b, &info);
    buf_puts(&b, ",\"departments\":[");
    for (i = 0; i < n; i++) {
        const char *name = "", *college = "";

        if (qr) {
            mysql_data_seek(qr, 0);
            while ((row = mysql_fetch_row(qr)) != NULL)
                if ((unsigned)strtoul(row[0], NULL, 10) == hits[i].id) {
                    name = row[1];
                    college = row[2];
                    break;
                }
        }
        if (i)
            buf_putc(&b, ',');
        buf_putc(&b, '{');
        json_kv_int(&b, "id", hits[i].id);                    buf_putc(&b, ',');
        json_kv_str(&b, "name", name);                        buf_putc(&b, ',');
        json_kv_str(&b, "college_name", college);             buf_putc(&b, ',');
        buf_printf(&b, "\"prob\":%.4f,", hits[i].prob);
        json_kv_int(&b, "score", hits[i].score);              buf_putc(&b, ',');
        ml_write_terms(&b, &hits[i]);
        buf_putc(&b, '}');
    }
    buf_puts(&b, "]}");
    if (qr)
        mysql_free_result(qr);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

void ml_write_terms(Buf *b, const MlHit *h)
{
    int i;

    buf_puts(b, "\"terms\":[");
    for (i = 0; i < h->nterms; i++) {
        if (i)
            buf_putc(b, ',');
        json_write_str(b, h->terms[i]);
    }
    buf_putc(b, ']');
}

static void handle_related(Request *req, Response *res)
{
    CurrentUser u;
    char dept_s[16], limit_s[8];
    unsigned dept;
    int limit, n, i, first = 1, personal = 0;
    char today[16];
    MlHit hits[RELATED_MAX];
    MlInfo info;
    Buf sql, b;
    MYSQL_RES *qr, *dq;
    MYSQL_ROW row;

    auth_current(req, &u);
    req_query(req, "department_id", dept_s, sizeof dept_s);
    req_query(req, "limit", limit_s, sizeof limit_s);

    dept = (unsigned)strtoul(dept_s, NULL, 10);
    if (!dept)
        dept = u.department_id;
    if (!dept) {
        res_error(res, 400, "no_department", "학과를 고르세요.");
        return;
    }
    limit = atoi(limit_s);
    if (limit <= 0 || limit > RELATED_MAX)
        limit = 30;

    /* 학과 정보 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT d.id, d.name, c.name FROM departments d "
                  "JOIN colleges c ON c.id = d.college_id WHERE d.id = %u", dept);
    dq = db_query_buf(&sql);
    buf_free(&sql);
    row = dq ? mysql_fetch_row(dq) : NULL;
    if (!row) {
        if (dq)
            mysql_free_result(dq);
        res_error(res, 404, "not_found", "그런 학과가 없습니다.");
        return;
    }

    memset(&info, 0, sizeof info);
    /* 내 학과를 볼 때는 내 관심 키워드·자기소개를 더해 개인 맞춤으로 매긴다. */
    {
        Buf prof;
        buf_init(&prof);
        personal = u.id && u.department_id == dept && profile_text(u.id, &prof);
        n = personal
            ? ml_rank_posts_personal(dept, prof.data, RELATED_MIN_SCORE, hits, limit, &info)
            : ml_rank_posts(dept, RELATED_MIN_SCORE, hits, limit, &info);
        buf_free(&prof);
    }
    if (n < 0) {
        mysql_free_result(dq);
        res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
        return;
    }

    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_s(&tmv, &now);
        strftime(today, sizeof today, "%Y-%m-%d", &tmv);
    }

    buf_init(&b);
    buf_puts(&b, "{\"model\":");
    write_info(&b, &info);
    buf_putc(&b, ',');
    json_kv_bool(&b, "personal", personal);
    buf_puts(&b, ",\"department\":{");
    json_kv_int(&b, "id", atoll(row[0]));          buf_putc(&b, ',');
    json_kv_str(&b, "name", row[1]);               buf_putc(&b, ',');
    json_kv_str(&b, "college_name", row[2]);
    buf_puts(&b, "},\"posts\":[");
    mysql_free_result(dq);

    if (n > 0) {
        MYSQL_ROW *rows = (MYSQL_ROW *)calloc((size_t)n, sizeof *rows);
        unsigned *ids = (unsigned *)calloc((size_t)n, sizeof *ids);

        /* 게시물 정보를 한 번에 읽고, 모델 순서대로 내보낸다. */
        buf_init(&sql);
        buf_puts(&sql,
                 "SELECT p.id, p.kind, p.title, "
                 "       IFNULL(DATE_FORMAT(IFNULL(p.deadline, a.deadline), '%Y-%m-%d'), ''), "
                 "       IFNULL(p.host, IFNULL(a.host, '')), "
                 "       DATE_FORMAT(p.created_at, '%Y-%m-%d'), "
                 "       (SELECT COUNT(*) FROM comments c WHERE c.post_id = p.id), "
                 "       LEFT(REPLACE(REPLACE(p.body, '\\r', ''), '\\n', ' '), 120) "
                 "FROM posts p LEFT JOIN post_ai a ON a.post_id = p.id WHERE p.id IN (");
        for (i = 0; i < n; i++) {
            if (i)
                buf_putc(&sql, ',');
            db_sqlf(&sql, "%u", hits[i].id);
        }
        buf_putc(&sql, ')');
        qr = db_query_buf(&sql);
        buf_free(&sql);

        if (qr && rows && ids) {
            int k = 0;
            while (k < n && (row = mysql_fetch_row(qr)) != NULL) {
                ids[k] = (unsigned)strtoul(row[0], NULL, 10);
                rows[k++] = row;
            }
            /* 첫 바퀴는 아직 열린 공모전, 둘째 바퀴는 마감이 지난 공모전 (뒤로 보낸다) */
            int pass;
            for (pass = 0; pass < 2; pass++)
            for (i = 0; i < n; i++) {
                int j, closed;
                MYSQL_ROW r = NULL;

                for (j = 0; j < k; j++)
                    if (ids[j] == hits[i].id)
                        r = rows[j];
                if (!r)
                    continue;
                closed = r[3][0] && strcmp(r[3], today) < 0;
                if (closed != pass)
                    continue;
                if (!first)
                    buf_putc(&b, ',');
                first = 0;
                buf_putc(&b, '{');
                json_kv_int(&b, "id", hits[i].id);                    buf_putc(&b, ',');
                json_kv_int(&b, "score", hits[i].score);              buf_putc(&b, ',');
                json_kv_bool(&b, "closed", closed);                   buf_putc(&b, ',');
                ml_write_terms(&b, &hits[i]);                         buf_putc(&b, ',');
                json_kv_str(&b, "kind", r[1]);                        buf_putc(&b, ',');
                json_kv_str(&b, "title", r[2]);                       buf_putc(&b, ',');
                json_kv_str(&b, "deadline", r[3][0] ? r[3] : NULL);   buf_putc(&b, ',');
                json_kv_str(&b, "host", r[4][0] ? r[4] : NULL);       buf_putc(&b, ',');
                json_kv_str(&b, "created_at", r[5]);                  buf_putc(&b, ',');
                json_kv_int(&b, "comment_count", atoll(r[6]));        buf_putc(&b, ',');
                json_kv_str(&b, "excerpt", r[7]);
                buf_putc(&b, '}');
            }
        }
        if (qr)
            mysql_free_result(qr);
        free(rows);
        free(ids);
    }

    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

static void handle_info(Request *req, Response *res, int retrain)
{
    CurrentUser u;
    MlInfo info;
    MlHit dummy;
    Buf b;

    if (retrain) {
        if (!auth_require_admin(req, res, &u))
            return;
        memset(&info, 0, sizeof info);
        if (!ml_retrain(&info)) {
            res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
            return;
        }
    } else {
        /* 아무 학과로 한 번 조회하면 필요할 때 학습되고 상태가 채워진다. */
        memset(&info, 0, sizeof info);
        if (ml_rank_posts(0, 101, &dummy, 1, &info) < 0) {
            res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
            return;
        }
    }

    buf_init(&b);
    write_info(&b, &info);
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

int route_ml(Request *req, Response *res)
{
    if (req_is(req, "GET", "/api/related")) {
        handle_related(req, res);
        return 1;
    }
    if (req_is(req, "GET", "/api/ml/info")) {
        handle_info(req, res, 0);
        return 1;
    }
    if (req_is(req, "POST", "/api/ml/retrain")) {
        handle_info(req, res, 1);
        return 1;
    }
    if (req_is(req, "POST", "/api/ml/predict")) {
        handle_predict(req, res);
        return 1;
    }
    return 0;
}
