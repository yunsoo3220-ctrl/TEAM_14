/* 그룹 스터디
 *
 *   GET    /api/studies                        스터디 목록 ?mine=1 이면 내가 속한 것만
 *   POST   /api/studies                        스터디 만들기 {name, description?, max_members?} (로그인)
 *   GET    /api/studies/{id}                   스터디 + 멤버
 *   DELETE /api/studies/{id}                   스터디 삭제 (방장·관리자)
 *   POST   /api/studies/{id}/join              참여하기 (로그인, 정원 안에서)
 *   GET    /api/studies/{id}/candidates?q=     멤버로 추가할 회원 검색 (방장·관리자)
 *   POST   /api/studies/{id}/members           멤버 추가 {user_id, role?} (방장·관리자)
 *   PUT    /api/studies/{id}/members/{uid}     역할 바꾸기 {role:"member"|"mentor"|"mentee"} (방장·관리자)
 *   DELETE /api/studies/{id}/members/{uid}     내보내기 (방장·관리자) / 나가기 (본인)
 *
 * 방장은 스터디를 만든 사람이다. 만들 때 자동으로 멤버가 되고, 나갈 수는 없다 (스터디를 지운다).
 * 멘토·멘티는 선택이다. 역할을 정하지 않은 멤버는 member 로 남는다.
 * 다른 회원에게 보이는 것은 표시 이름·학과뿐이다 (api_community.c 와 같은 원칙).
 *
 * 관련 테이블:
 *   study_groups   스터디 (owner_id = 방장)
 *   study_members  (스터디, 회원, 역할)
 */
#include "api.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STUDY_MAX_MEMBERS 50     /* 정원 상한 */
#define STUDY_LIST_MAX    200    /* 목록 최대 개수 */
#define CANDIDATE_MAX     10     /* 멤버 검색 결과 최대 수 */

/* ------------------------------------------------------------ 도우미 */

/* 입력한 역할 이름을 코드 상수로 바꾼다. 모르는 값이면 NULL.
 * SQL 에는 사용자 입력이 아니라 이 상수를 넣는다. */
static const char *role_code(const char *s)
{
    if (!s)                       return NULL;
    if (!strcmp(s, "member"))     return "member";
    if (!strcmp(s, "mentor"))     return "mentor";
    if (!strcmp(s, "mentee"))     return "mentee";
    return NULL;
}

/* "/api/studies/12/members/34" 에서 두 id 를 꺼낸다. 형식이 맞으면 1. */
static int path_two_ids(const char *path, unsigned *gid, unsigned *uid)
{
    static const char prefix[] = "/api/studies/";
    static const char mid[] = "/members/";
    const char *p;
    char *end;
    unsigned long a, b;

    if (strncmp(path, prefix, sizeof prefix - 1) != 0)
        return 0;
    p = path + sizeof prefix - 1;
    if (*p < '0' || *p > '9')
        return 0;
    a = strtoul(p, &end, 10);
    if (strncmp(end, mid, sizeof mid - 1) != 0)
        return 0;
    p = end + sizeof mid - 1;
    if (*p < '0' || *p > '9')
        return 0;
    b = strtoul(p, &end, 10);
    if (*end != '\0' || a == 0 || b == 0 || a > 0xFFFFFFFFul || b > 0xFFFFFFFFul)
        return 0;
    *gid = (unsigned)a;
    *uid = (unsigned)b;
    return 1;
}

/* 스터디 방장 id. 스터디가 없으면 -1. */
static long long study_owner(unsigned gid)
{
    Buf sql;
    long long owner;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT owner_id FROM study_groups WHERE id = %u", gid);
    owner = db_scalar(sql.data, -1);
    buf_free(&sql);
    return owner;
}

/* 방장이거나 관리자인지. 반환: 1 = 관리 권한 있음, 0 = 없음, -1 = 스터디 없음 */
static int study_manage_ok(unsigned gid, const CurrentUser *u)
{
    long long owner = study_owner(gid);

    if (owner < 0)
        return -1;
    return u->is_admin || (unsigned)owner == u->id;
}

/* ------------------------------------------------------------ 목록 */

/* 목록 행: id, name, description 앞부분, max_members, created_at, owner_id, 방장 표시 이름,
 *          멤버 수, 멘토 수, 멘티 수, 내 역할('' = 멤버 아님)
 * db_sqlf 의 첫 인자로 지금 회원 번호(%u, 비로그인은 0)를 준다. */
#define STUDY_SELECT \
    "SELECT g.id, g.name, LEFT(REPLACE(REPLACE(g.description, '\\r', ''), '\\n', ' '), 120), " \
    "       g.max_members, DATE_FORMAT(g.created_at, '%%Y-%%m-%%d %%H:%%i'), g.owner_id, u.nickname, " \
    "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id), " \
    "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id AND m.role = 'mentor'), " \
    "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id AND m.role = 'mentee'), " \
    "       IFNULL((SELECT m.role FROM study_members m WHERE m.group_id = g.id AND m.user_id = %u), '') " \
    "FROM study_groups g JOIN users u ON u.id = g.owner_id "

/* STUDY_SELECT 결과 한 행을 JSON 객체로 쓴다 */
static void write_study_row(Buf *b, MYSQL_ROW row)
{
    buf_putc(b, '{');
    json_kv_int(b, "id", atoll(row[0]));                     buf_putc(b, ',');
    json_kv_str(b, "name", row[1]);                          buf_putc(b, ',');
    json_kv_str(b, "excerpt", row[2]);                       buf_putc(b, ',');
    json_kv_int(b, "max_members", atoll(row[3]));            buf_putc(b, ',');
    json_kv_str(b, "created_at", row[4]);                    buf_putc(b, ',');
    json_kv_int(b, "owner_id", atoll(row[5]));               buf_putc(b, ',');
    json_kv_str(b, "owner_nickname", row[6]);                buf_putc(b, ',');
    json_kv_int(b, "member_count", atoll(row[7]));           buf_putc(b, ',');
    json_kv_int(b, "mentor_count", atoll(row[8]));           buf_putc(b, ',');
    json_kv_int(b, "mentee_count", atoll(row[9]));           buf_putc(b, ',');
    json_kv_str(b, "my_role", row[10][0] ? row[10] : NULL);  /* null = 멤버 아님 */
    buf_putc(b, '}');
}

/* GET /api/studies?mine=1 */
static void handle_study_list(Request *req, Response *res)
{
    CurrentUser u;
    char mine[4] = "";
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    auth_current(req, &u);
    req_query(req, "mine", mine, sizeof mine);

    buf_init(&sql);
    db_sqlf(&sql, STUDY_SELECT, u.id);
    if (mine[0] == '1' && u.id)
        db_sqlf(&sql, "WHERE EXISTS (SELECT 1 FROM study_members x WHERE x.group_id = g.id AND x.user_id = %u) ",
                u.id);
    db_sqlf(&sql, "ORDER BY g.created_at DESC, g.id DESC LIMIT %d", STUDY_LIST_MAX);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "스터디 목록을 읽을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"studies\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (!first)
            buf_putc(&b, ',');
        first = 0;
        write_study_row(&b, row);
    }
    mysql_free_result(qr);
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 만들기 · 상세 · 삭제 */

/* POST /api/studies {name, description?, max_members?} - 만든 사람이 방장이자 첫 멤버가 된다 */
static void handle_study_create(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    char *name = NULL, *desc = NULL;
    int max;
    Buf sql;
    unsigned long long gid;

    if (!auth_require(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;

    name = str_dup(json_str(in, "name", ""));
    desc = str_dup(json_str(in, "description", ""));
    max = (int)json_int(in, "max_members", 10);
    if (!name || !desc) {
        res_error(res, 500, "oom", "메모리가 부족합니다.");
        goto out;
    }
    str_trim(name);
    str_trim(desc);

    /* 길이는 바이트 기준 (한글 1글자 = 3바이트): 이름 100자, 소개 1000자 */
    if (strlen(name) < 2 || strlen(name) > 300) {
        res_error(res, 400, "bad_name", "스터디 이름을 2~100자로 정하세요.");
        goto out;
    }
    if (strlen(desc) > 3000) {
        res_error(res, 400, "bad_description", "소개는 1000자까지 쓸 수 있습니다.");
        goto out;
    }
    if (max < 2 || max > STUDY_MAX_MEMBERS) {
        res_error(res, 400, "bad_max", "정원은 2~50명으로 정하세요.");
        goto out;
    }

    if (!db_begin()) {
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }
    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO study_groups (owner_id, name, description, max_members) "
                  "VALUES (%u, %Q, %Q, %d)", u.id, name, desc, max);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "스터디를 만들 수 없습니다.");
        goto out;
    }
    gid = db_last_id();
    buf_reset(&sql);
    db_sqlf(&sql, "INSERT INTO study_members (group_id, user_id) VALUES (%u, %u)", (unsigned)gid, u.id);
    if (!db_exec_buf(&sql) || !db_commit()) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "스터디를 만들 수 없습니다.");
        goto out;
    }
    buf_free(&sql);

    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "id", (long long)gid);
        buf_putc(&b, '}');
        res_json_buf(res, 201, &b);
        buf_free(&b);
    }

out:
    free(name);
    free(desc);
    json_free(in);
}

/* GET /api/studies/{id}
 * 응답: {"study":{목록과 같은 형태},"description":"전체 소개","can_manage":bool,
 *        "members":[{user_id,nickname,department,role,is_owner,joined_at}, ...]}
 * 멤버는 멘토 → 멘티 → 일반 순, 같은 역할 안에서는 들어온 순서 */
static void handle_study_detail(Request *req, Response *res, unsigned gid)
{
    CurrentUser u;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int first = 1;

    auth_current(req, &u);

    buf_init(&sql);
    db_sqlf(&sql, STUDY_SELECT "WHERE g.id = %u", u.id, gid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (!row) {
        if (qr)
            mysql_free_result(qr);
        res_error(res, 404, "not_found", "스터디가 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"study\":");
    write_study_row(&b, row);
    mysql_free_result(qr);

    buf_init(&sql);
    db_sqlf(&sql, "SELECT description FROM study_groups WHERE id = %u", gid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    buf_puts(&b, ",\"description\":");
    json_write_str(&b, row ? row[0] : "");
    if (qr)
        mysql_free_result(qr);

    buf_putc(&b, ',');
    json_kv_bool(&b, "can_manage", u.id && study_manage_ok(gid, &u) == 1);

    buf_puts(&b, ",\"members\":[");
    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT m.user_id, u.nickname, IFNULL(d.name, ''), m.role, m.user_id = g.owner_id, "
            "       DATE_FORMAT(m.joined_at, '%%Y-%%m-%%d') "
            "FROM study_members m JOIN users u ON u.id = m.user_id "
            "JOIN study_groups g ON g.id = m.group_id "
            "LEFT JOIN departments d ON d.id = u.department_id "
            "WHERE m.group_id = %u "
            "ORDER BY FIELD(m.role, 'mentor', 'mentee', 'member'), m.joined_at, m.user_id", gid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL) {
            if (!first)
                buf_putc(&b, ',');
            first = 0;
            buf_putc(&b, '{');
            json_kv_int(&b, "user_id", atoll(row[0]));                     buf_putc(&b, ',');
            json_kv_str(&b, "nickname", row[1]);                           buf_putc(&b, ',');
            json_kv_str(&b, "department", row[2][0] ? row[2] : NULL);      buf_putc(&b, ',');
            json_kv_str(&b, "role", row[3]);                               buf_putc(&b, ',');
            json_kv_bool(&b, "is_owner", atoi(row[4]));                    buf_putc(&b, ',');
            json_kv_str(&b, "joined_at", row[5]);
            buf_putc(&b, '}');
        }
        mysql_free_result(qr);
    }
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* DELETE /api/studies/{id} (방장·관리자). 멤버 목록은 CASCADE 로 함께 지워진다. */
static void handle_study_delete(Request *req, Response *res, unsigned gid)
{
    CurrentUser u;
    Buf sql;
    int ok;

    if (!auth_require(req, res, &u))
        return;
    ok = study_manage_ok(gid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "스터디가 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "방장만 지울 수 있습니다."); return; }

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM study_groups WHERE id = %u", gid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "스터디를 지울 수 없습니다.");
        return;
    }
    buf_free(&sql);
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 멤버 */

/* gid 스터디에 uid 회원을 role 로 넣는다. 정원 검사와 INSERT 를 한 트랜잭션으로 묶어
 * 두 사람이 동시에 마지막 자리에 들어오는 일을 막는다 (db_begin 이 커밋까지 연결 잠금을 쥔다).
 * 성공하면 1, 실패하면 응답을 채우고 0. */
static int add_member(Response *res, unsigned gid, unsigned uid, const char *role)
{
    Buf sql;
    long long max, count, exists;

    if (!db_begin()) {
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        return 0;
    }
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u AND user_id = %u", gid, uid);
    exists = db_scalar(sql.data, 0);
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT max_members FROM study_groups WHERE id = %u FOR UPDATE", gid);
    max = db_scalar(sql.data, 0);
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u", gid);
    count = db_scalar(sql.data, 0);

    if (exists) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 409, "already_member", "이미 이 스터디의 멤버입니다.");
        return 0;
    }
    if (count >= max) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 409, "study_full", "정원이 찼습니다.");
        return 0;
    }

    buf_reset(&sql);
    db_sqlf(&sql, "INSERT INTO study_members (group_id, user_id, role) VALUES (%u, %u, '%s')",
            gid, uid, role);
    if (!db_exec_buf(&sql) || !db_commit()) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "멤버를 추가할 수 없습니다.");
        return 0;
    }
    buf_free(&sql);
    return 1;
}

/* POST /api/studies/{id}/join - 스스로 참여 (역할은 member 로 시작, 방장이 바꿔 줄 수 있다) */
static void handle_study_join(Request *req, Response *res, unsigned gid)
{
    CurrentUser u;

    if (!auth_require(req, res, &u))
        return;
    if (study_owner(gid) < 0) {
        res_error(res, 404, "not_found", "스터디가 없습니다.");
        return;
    }
    if (add_member(res, gid, u.id, "member"))
        res_json(res, 201, "{\"ok\":true}");
}

/* GET /api/studies/{id}/candidates?q=이름 또는 학과 (방장·관리자)
 * 아직 멤버가 아닌 학생을 표시 이름이나 학과 이름으로 찾는다. 학번·실명으로는 찾지 않는다. */
static void handle_study_candidates(Request *req, Response *res, unsigned gid)
{
    CurrentUser u;
    char q[128] = "";
    Buf pat, sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int ok, first = 1;
    const char *p;

    if (!auth_require(req, res, &u))
        return;
    ok = study_manage_ok(gid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "스터디가 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "방장만 멤버를 추가할 수 있습니다."); return; }

    req_query(req, "q", q, sizeof q);
    str_trim(q);
    if (!q[0]) {
        res_json(res, 200, "{\"users\":[]}");
        return;
    }

    /* LIKE 의 특수 문자(% _ \)는 글자 그대로 찾도록 \ 를 앞에 붙인다 */
    buf_init(&pat);
    buf_putc(&pat, '%');
    for (p = q; *p; p++) {
        if (*p == '%' || *p == '_' || *p == '\\')
            buf_putc(&pat, '\\');
        buf_putc(&pat, *p);
    }
    buf_putc(&pat, '%');

    buf_init(&sql);
    db_sqlf(&sql,
            "SELECT u.id, u.nickname, IFNULL(d.name, '') "
            "FROM users u LEFT JOIN departments d ON d.id = u.department_id "
            "WHERE u.role = 'student' "
            "  AND (u.nickname LIKE %Q OR d.name LIKE %Q) "
            "  AND NOT EXISTS (SELECT 1 FROM study_members m WHERE m.group_id = %u AND m.user_id = u.id) "
            "ORDER BY u.nickname LIMIT %d",
            pat.data, pat.data, gid, CANDIDATE_MAX);
    buf_free(&pat);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "회원을 찾을 수 없습니다.");
        return;
    }

    buf_init(&b);
    buf_puts(&b, "{\"users\":[");
    while ((row = mysql_fetch_row(qr)) != NULL) {
        if (!first)
            buf_putc(&b, ',');
        first = 0;
        buf_putc(&b, '{');
        json_kv_int(&b, "id", atoll(row[0]));                       buf_putc(&b, ',');
        json_kv_str(&b, "nickname", row[1]);                        buf_putc(&b, ',');
        json_kv_str(&b, "department", row[2][0] ? row[2] : NULL);
        buf_putc(&b, '}');
    }
    mysql_free_result(qr);
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* POST /api/studies/{id}/members {user_id, role?} (방장·관리자) */
static void handle_study_add(Request *req, Response *res, unsigned gid)
{
    CurrentUser u;
    Json *in;
    unsigned uid;
    const char *role;
    Buf sql;
    int ok;

    if (!auth_require(req, res, &u))
        return;
    ok = study_manage_ok(gid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "스터디가 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "방장만 멤버를 추가할 수 있습니다."); return; }

    in = body_object(req, res);
    if (!in)
        return;
    uid = (unsigned)json_int(in, "user_id", 0);
    role = role_code(json_str(in, "role", "member"));
    json_free(in);
    if (!role) {
        res_error(res, 400, "bad_role", "역할은 member, mentor, mentee 중 하나입니다.");
        return;
    }

    /* 있는 학생인지 (관리자 계정은 스터디 멤버로 넣지 않는다) */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM users WHERE id = %u AND role = 'student'", uid);
    ok = uid && db_scalar(sql.data, 0) > 0;
    buf_free(&sql);
    if (!ok) {
        res_error(res, 400, "bad_user", "추가할 회원이 없습니다.");
        return;
    }

    if (add_member(res, gid, uid, role))
        res_json(res, 201, "{\"ok\":true}");
}

/* PUT /api/studies/{id}/members/{uid} {role} (방장·관리자) - 멘토/멘티/일반 지정 */
static void handle_study_role(Request *req, Response *res, unsigned gid, unsigned uid)
{
    CurrentUser u;
    Json *in;
    const char *role;
    Buf sql;
    int ok;

    if (!auth_require(req, res, &u))
        return;
    ok = study_manage_ok(gid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "스터디가 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "방장만 역할을 정할 수 있습니다."); return; }

    in = body_object(req, res);
    if (!in)
        return;
    role = role_code(json_str(in, "role", NULL));
    json_free(in);
    if (!role) {
        res_error(res, 400, "bad_role", "역할은 member, mentor, mentee 중 하나입니다.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "UPDATE study_members SET role = '%s' WHERE group_id = %u AND user_id = %u",
            role, gid, uid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "역할을 바꿀 수 없습니다.");
        return;
    }
    buf_free(&sql);
    /* 같은 값으로 바꾸면 affected 가 0 이라 존재 여부는 따로 확인한다 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u AND user_id = %u", gid, uid);
    ok = db_scalar(sql.data, 0) > 0;
    buf_free(&sql);
    if (!ok) {
        res_error(res, 404, "not_member", "이 스터디의 멤버가 아닙니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

/* DELETE /api/studies/{id}/members/{uid}
 * 방장·관리자는 다른 멤버를 내보낼 수 있고, 멤버는 스스로 나갈 수 있다. 방장은 나갈 수 없다. */
static void handle_study_remove(Request *req, Response *res, unsigned gid, unsigned uid)
{
    CurrentUser u;
    Buf sql;
    long long owner;

    if (!auth_require(req, res, &u))
        return;
    owner = study_owner(gid);
    if (owner < 0) {
        res_error(res, 404, "not_found", "스터디가 없습니다.");
        return;
    }
    if ((unsigned)owner == uid) {
        res_error(res, 400, "owner_cannot_leave", "방장은 나갈 수 없습니다. 스터디를 삭제하세요.");
        return;
    }
    if (uid != u.id && !u.is_admin && (unsigned)owner != u.id) {
        res_error(res, 403, "not_allowed", "방장만 멤버를 내보낼 수 있습니다.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "DELETE FROM study_members WHERE group_id = %u AND user_id = %u", gid, uid);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        res_error(res, 500, "db_error", "멤버를 뺄 수 없습니다.");
        return;
    }
    buf_free(&sql);
    if (db_affected() == 0) {
        res_error(res, 404, "not_member", "이 스터디의 멤버가 아닙니다.");
        return;
    }
    res_json(res, 200, "{\"ok\":true}");
}

/* ------------------------------------------------------------ 라우터 */

int route_study(Request *req, Response *res)
{
    unsigned gid, uid;
    const char *m = req->method;

    if (req_is(req, "GET", "/api/studies"))  { handle_study_list(req, res);   return 1; }
    if (req_is(req, "POST", "/api/studies")) { handle_study_create(req, res); return 1; }

    if (path_two_ids(req->path, &gid, &uid)) {
        if (!strcmp(m, "PUT"))    { handle_study_role(req, res, gid, uid);   return 1; }
        if (!strcmp(m, "DELETE")) { handle_study_remove(req, res, gid, uid); return 1; }
        return 0;
    }
    if (!strcmp(m, "POST") && path_id(req->path, "/api/studies/", "/join", &gid)) {
        handle_study_join(req, res, gid);
        return 1;
    }
    if (!strcmp(m, "GET") && path_id(req->path, "/api/studies/", "/candidates", &gid)) {
        handle_study_candidates(req, res, gid);
        return 1;
    }
    if (!strcmp(m, "POST") && path_id(req->path, "/api/studies/", "/members", &gid)) {
        handle_study_add(req, res, gid);
        return 1;
    }
    if (!strcmp(m, "GET") && path_id(req->path, "/api/studies/", NULL, &gid)) {
        handle_study_detail(req, res, gid);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/studies/", NULL, &gid)) {
        handle_study_delete(req, res, gid);
        return 1;
    }
    return 0;
}
