/* 그룹 스터디
 *
 *   GET    /api/studies                        스터디 목록 ?mine=1 이면 내가 속한 것만, ?dept=학과id 면 그 학과 대상만,
 *                                              ?reco=학과id 면 그 학과와 관련 있는 순 (자체 모델, 학과별 추천)
 *   POST   /api/studies                        스터디 만들기 {name, description?, max_members?, end_date?,
 *                                                             department_ids?, role?} (로그인)
 *   GET    /api/studies/{id}                   스터디 + 멤버
 *   PUT    /api/studies/{id}                   스터디 수정 {name, description?, max_members?, end_date?, department_ids?}
 *                                              (방장·관리자. 정원은 지금 인원보다 작게 줄일 수 없다)
 *   DELETE /api/studies/{id}                   스터디 삭제 (방장·관리자)
 *   POST   /api/studies/{id}/join              참여 신청 (로그인). 방장에게 알림이 가고, 방장이 수락해야 멤버가 된다
 *   DELETE /api/studies/{id}/join              내 참여 신청 취소
 *   POST   /api/studies/{id}/requests/{uid}    참여 신청 수락 (방장·관리자)
 *   DELETE /api/studies/{id}/requests/{uid}    참여 신청 거절 (방장·관리자)
 *   GET    /api/studies/{id}/candidates?q=     멤버로 추가할 회원 검색 (방장·관리자)
 *   POST   /api/studies/{id}/members           멤버 추가 {user_id, role?} (방장·관리자)
 *   PUT    /api/studies/{id}/members/{uid}     역할 바꾸기 {role:"member"|"mentor"|"mentee"} (방장·관리자)
 *   DELETE /api/studies/{id}/members/{uid}     내보내기 (방장·관리자) / 나가기 (본인)
 *
 * 참여는 신청제다: 신청 → 방장 수락 → 멤버. 멤버 수가 정원에 닿으면 확정된 그룹(confirmed_at)이 되고,
 * 그때부터는 신청·추가·나가기·내보내기를 막는다 (관리자는 내보낼 수 있다). 남은 신청은 지우고 신청한 사람에게 알린다.
 *
 * 방장은 스터디를 만든 사람이다. 만들 때 자동으로 멤버가 되고, 나갈 수는 없다 (스터디를 지운다).
 * 멘토·멘티는 선택이다. 역할을 정하지 않은 멤버는 member 로 남는다.
 * 다른 회원에게 보이는 것은 표시 이름·학과뿐이다 (api_community.c 와 같은 원칙).
 * 종료일(end_date)을 정한 스터디는 그날이 지나면 자동으로 지워진다 (cleanup.c).
 * 예전의 멘토링 메뉴는 없앴다 (스터디에서 멘토·멘티를 정할 수 있으므로). 남아 있던 멘토링은 서버가 시작할 때
 * 그룹 스터디로 옮긴다 (cleanup.c). study_groups.kind 열은 예전 DB 와 맞추려고 남겨 두었고 늘 'study' 다.
 *
 * 대상 학과는 여러 개 고를 수 있고 선택이다 (없으면 학과 무관). 목록의 "내 학과 대상" 탭이 이 값으로 거른다.
 * 대상 학과가 있으면 그 학과 학생만 멤버가 될 수 있다 (참여하기·방장의 멤버 추가·참여 권유 수락 모두 study_add_member 를 거친다).
 *
 * 관련 테이블:
 *   study_groups       스터디 (owner_id = 방장. department_id 열은 예전의 학과 하나라 쓰지 않는다)
 *   study_departments  (스터디, 대상 학과)
 *   study_members      (스터디, 회원, 역할)
 *   study_requests     (스터디, 신청한 회원) - 방장이 수락하기 전의 참여 신청
 */
#include "api.h"
#include "db.h"
#include "ml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STUDY_MAX_MEMBERS 8      /* 정원 상한 */
#define STUDY_MAX_DEPTS   20     /* 대상 학과 최대 개수 */
#define STUDY_LIST_MAX    200    /* 목록 최대 개수 */
#define CANDIDATE_MAX     20     /* 멤버 검색 결과 최대 수 */

/* ------------------------------------------------------------ 도우미 */

/* "YYYY-MM-DD" 형식만 통과, 아니면 NULL (api_community.c 의 같은 이름 함수와 동일) */
static const char *clean_date(const char *s)
{
    int i;

    if (!s || strlen(s) != 10)
        return NULL;
    for (i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-')
                return NULL;
        } else if (s[i] < '0' || s[i] > '9') {
            return NULL;
        }
    }
    return s;
}

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

/* "/api/studies/12/members/34" (mid = "/members/") 에서 두 id 를 꺼낸다. 형식이 맞으면 1. */
static int path_two_ids(const char *path, const char *mid, unsigned *gid, unsigned *uid)
{
    static const char prefix[] = "/api/studies/";
    size_t midlen = strlen(mid);
    const char *p;
    char *end;
    unsigned long a, b;

    if (strncmp(path, prefix, sizeof prefix - 1) != 0)
        return 0;
    p = path + sizeof prefix - 1;
    if (*p < '0' || *p > '9')
        return 0;
    a = strtoul(p, &end, 10);
    if (strncmp(end, mid, midlen) != 0)
        return 0;
    p = end + midlen;
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

/* uid 회원이 gid 스터디의 대상 학과 제한을 통과하면 1 (대상 학과가 없는 스터디는 누구나). api.h */
int study_department_ok(unsigned gid, unsigned uid)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT NOT EXISTS (SELECT 1 FROM study_departments WHERE group_id = %u) "
                  "    OR EXISTS (SELECT 1 FROM study_departments sd JOIN users u ON u.department_id = sd.department_id "
                  "               WHERE sd.group_id = %u AND u.id = %u)", gid, gid, uid);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

/* 방장이거나 관리자인지. 반환: 1 = 관리 권한 있음, 0 = 없음, -1 = 스터디 없음 */
static int study_manage_ok(unsigned gid, const CurrentUser *u)
{
    long long owner = study_owner(gid);

    if (owner < 0)
        return -1;
    return u->is_admin || (unsigned)owner == u->id;
}

/* 정원이 차서 확정된 스터디면 1 */
static int study_confirmed(unsigned gid)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT confirmed_at IS NOT NULL FROM study_groups WHERE id = %u", gid);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

/* 스터디 이름 (알림 문구용). 없으면 "" */
static void study_name(unsigned gid, char *out, size_t outsz)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;

    out[0] = '\0';
    buf_init(&sql);
    db_sqlf(&sql, "SELECT name FROM study_groups WHERE id = %u", gid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr)
        return;
    if ((row = mysql_fetch_row(qr)) != NULL)
        str_copy(out, outsz, row[0]);
    mysql_free_result(qr);
}

/* 방금 확정된 스터디: 멤버들에게 알리고, 남아 있던 참여 신청은 지우면서 신청한 사람에게 마감을 알린다 */
static void study_announce_confirmed(unsigned gid)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char name[304], title[400], link[32];

    study_name(gid, name, sizeof name);
    snprintf(link, sizeof link, "#/studies/%u", gid);

    snprintf(title, sizeof title, "「%s」 스터디 인원이 모두 모여 확정되었습니다", name);
    buf_init(&sql);
    db_sqlf(&sql, "SELECT user_id FROM study_members WHERE group_id = %u", gid);
    qr = db_query_buf(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL)
            notify_add((unsigned)strtoul(row[0], NULL, 10), 0, "study_confirmed", title,
                       "이제 멤버가 바뀌지 않습니다. 채팅방에서 일정을 정해 보세요.", link);
        mysql_free_result(qr);
    }

    snprintf(title, sizeof title, "「%s」 스터디 정원이 차서 참여 신청이 마감되었습니다", name);
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT user_id FROM study_requests WHERE group_id = %u", gid);
    qr = db_query_buf(&sql);
    if (qr) {
        while ((row = mysql_fetch_row(qr)) != NULL)
            notify_add((unsigned)strtoul(row[0], NULL, 10), 0, "study_reject", title, "", link);
        mysql_free_result(qr);
    }
    buf_reset(&sql);
    db_sqlf(&sql, "DELETE FROM study_requests WHERE group_id = %u", gid);
    db_exec_buf(&sql);
    buf_free(&sql);
}

/* ------------------------------------------------------------ 목록 */

/* 목록 행: id, name, description 앞부분, max_members, created_at, owner_id, 방장 표시 이름,
 *          멤버 수, 멘토 수, 멘티 수, 내 역할('' = 멤버 아님), 종료일('' = 없음), 구분(늘 study),
 *          대상 학과 id 들("3,7", '' = 학과 무관), 대상 학과 이름들("컴퓨터공학과, 소프트웨어학과"),
 *          description 전체(학과별 추천·상세에서 쓴다), 확정 시각('' = 모집 중)
 * db_sqlf 의 첫 인자로 지금 회원 번호(%u, 비로그인은 0)를 준다. */
#define STUDY_SELECT \
    "SELECT g.id, g.name, LEFT(REPLACE(REPLACE(g.description, '\\r', ''), '\\n', ' '), 120), " \
    "       g.max_members, DATE_FORMAT(g.created_at, '%%Y-%%m-%%d %%H:%%i'), g.owner_id, u.nickname, " \
    "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id), " \
    "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id AND m.role = 'mentor'), " \
    "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id AND m.role = 'mentee'), " \
    "       IFNULL((SELECT m.role FROM study_members m WHERE m.group_id = g.id AND m.user_id = %u), ''), " \
    "       IFNULL(DATE_FORMAT(g.end_date, '%%Y-%%m-%%d'), ''), g.kind, " \
    "       IFNULL((SELECT GROUP_CONCAT(d.id ORDER BY d.sort SEPARATOR ',') FROM study_departments sd " \
    "               JOIN departments d ON d.id = sd.department_id WHERE sd.group_id = g.id), ''), " \
    "       IFNULL((SELECT GROUP_CONCAT(d.name ORDER BY d.sort SEPARATOR ', ') FROM study_departments sd " \
    "               JOIN departments d ON d.id = sd.department_id WHERE sd.group_id = g.id), ''), " \
    "       g.description, IFNULL(DATE_FORMAT(g.confirmed_at, '%%Y-%%m-%%d %%H:%%i'), '') " \
    "FROM study_groups g JOIN users u ON u.id = g.owner_id "

/* STUDY_SELECT 결과 한 행을 JSON 객체로 쓴다.
 * hit 가 있으면(학과별 추천) 학과 관련도 점수와 근거 키워드도 덧붙인다. */
static void write_study_row(Buf *b, MYSQL_ROW row, const MlHit *hit)
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
    buf_putc(b, ',');
    json_kv_str(b, "end_date", row[11][0] ? row[11] : NULL); /* null = 종료일 없음 */
    buf_putc(b, ',');
    json_kv_str(b, "kind", row[12]);                         buf_putc(b, ',');
    buf_puts(b, "\"department_ids\":[");                      /* [] = 학과 무관 */
    buf_puts(b, row[13]);                                      /* GROUP_CONCAT 한 숫자들이라 그대로 배열이 된다 */
    buf_puts(b, "],");
    json_kv_str(b, "department", row[14][0] ? row[14] : NULL); /* 학과 이름들, null = 학과 무관 */
    buf_putc(b, ',');
    json_kv_str(b, "confirmed_at", row[16][0] ? row[16] : NULL); /* null = 모집 중, 아니면 확정된 그룹 */
    if (hit) {
        buf_putc(b, ',');
        json_kv_int(b, "score", hit->score);
        buf_putc(b, ',');
        ml_write_terms(b, hit);
    }
    buf_putc(b, '}');
}

/* 학과별 추천: rows 의 스터디를 학과 dept 와 관련 있는 순으로 b 에 쓴다 (ML_MIN_SCORE 미만은 뺀다).
 * 모델에 넘기는 글 = 제목: 스터디 이름, 본문: 소개 + 대상 학과 이름. 모델을 만들 수 없으면 0. */
static int write_reco_studies(Buf *b, MYSQL_ROW *rows, int n, unsigned dept)
{
    unsigned *ids = (unsigned *)calloc((size_t)n, sizeof *ids);
    const char **titles = (const char **)calloc((size_t)n, sizeof *titles);
    char **bodies = (char **)calloc((size_t)n, sizeof *bodies);
    MlHit *hits = (MlHit *)calloc((size_t)n, sizeof *hits);
    int i, k, m = -1;

    if (ids && titles && bodies && hits) {
        for (i = 0; i < n; i++) {
            Buf t;
            buf_init(&t);
            buf_puts(&t, rows[i][15]);
            buf_putc(&t, '\n');
            buf_puts(&t, rows[i][14]);
            ids[i] = (unsigned)strtoul(rows[i][0], NULL, 10);
            titles[i] = rows[i][1];
            bodies[i] = t.data;   /* 메모리가 모자라 NULL 이면 모델이 빈 글로 본다 */
        }
        m = ml_rank_texts(dept, ids, titles, (const char *const *)bodies, n, ML_MIN_SCORE, hits, NULL);
        /* 점수 순(hits)으로, 그 스터디의 행을 찾아 쓴다 (많아야 STUDY_LIST_MAX 개라 그냥 훑는다) */
        for (k = 0; k < m; k++)
            for (i = 0; i < n; i++)
                if (ids[i] == hits[k].id) {
                    if (k)
                        buf_putc(b, ',');
                    write_study_row(b, rows[i], &hits[k]);
                    break;
                }
        for (i = 0; i < n; i++)
            free(bodies[i]);
    }
    free(ids);
    free(titles);
    free(bodies);
    free(hits);
    return m >= 0;
}

/* GET /api/studies?mine=1&dept=학과id&reco=학과id&confirmed=1
 *   확정된 그룹(정원이 차서 멤버가 고정됨)은 기본 목록에서 빠진다. confirmed=1 이면 확정된 그룹만,
 *   mine=1 이면 확정 여부와 상관없이 내가 속한 것 모두 (내 모집 관리·채팅에서 내 그룹을 찾을 수 있게)
 *   mine=1  내가 속한 스터디만
 *   dept    그 학과를 대상으로 정한 스터디만 (내 학과 대상)
 *   reco    자체 모델이 그 학과와 관련 있다고 본 스터디를 관련도 순으로 (학과별 추천) */
static void handle_study_list(Request *req, Response *res)
{
    CurrentUser u;
    char mine[4] = "", dept[12] = "", reco[12] = "", confirmed[4] = "";
    unsigned dept_id, reco_id;
    Buf sql, b;
    MYSQL_RES *qr;
    MYSQL_ROW rows[STUDY_LIST_MAX];   /* 결과 행들 (최신순) */
    int n = 0, i;

    auth_current(req, &u);
    req_query(req, "mine", mine, sizeof mine);
    req_query(req, "dept", dept, sizeof dept);
    req_query(req, "reco", reco, sizeof reco);
    req_query(req, "confirmed", confirmed, sizeof confirmed);
    dept_id = (unsigned)strtoul(dept, NULL, 10);
    reco_id = (unsigned)strtoul(reco, NULL, 10);

    buf_init(&sql);
    db_sqlf(&sql, STUDY_SELECT "WHERE 1 = 1 ", u.id);
    if (mine[0] == '1' && u.id)
        db_sqlf(&sql, "AND EXISTS (SELECT 1 FROM study_members x WHERE x.group_id = g.id AND x.user_id = %u) ",
                u.id);
    if (dept_id)
        db_sqlf(&sql, "AND EXISTS (SELECT 1 FROM study_departments sd WHERE sd.group_id = g.id AND sd.department_id = %u) ",
                dept_id);
    if (confirmed[0] == '1')
        buf_puts(&sql, "AND g.confirmed_at IS NOT NULL ");
    else if (mine[0] != '1')
        buf_puts(&sql, "AND g.confirmed_at IS NULL ");   /* 확정된 그룹은 따로 (confirmed=1) */
    db_sqlf(&sql, "ORDER BY g.created_at DESC, g.id DESC LIMIT %d", STUDY_LIST_MAX);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    if (!qr) {
        res_error(res, 500, "db_error", "스터디 목록을 읽을 수 없습니다.");
        return;
    }
    while (n < STUDY_LIST_MAX && (rows[n] = mysql_fetch_row(qr)) != NULL)
        n++;

    buf_init(&b);
    buf_puts(&b, "{\"studies\":[");
    if (reco_id && n > 0) {
        if (!write_reco_studies(&b, rows, n, reco_id)) {
            buf_free(&b);
            mysql_free_result(qr);
            res_error(res, 500, "model_error", "추천 모델을 만들 수 없습니다.");
            return;
        }
    } else {
        for (i = 0; i < n; i++) {
            if (i)
                buf_putc(&b, ',');
            write_study_row(&b, rows[i], NULL);
        }
    }
    mysql_free_result(qr);   /* rows 포인터를 다 쓴 뒤에 해제 */
    buf_puts(&b, "]}");
    res_json_buf(res, 200, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------ 만들기 · 상세 · 삭제 */

/* POST /api/studies {name, description?, max_members?, end_date?, department_ids?, role?}    (edit_id = 0, 새 스터디)
 * PUT  /api/studies/{id} 같은 항목 (role 은 쓰지 않는다)                                       (edit_id = 고칠 스터디)
 * 만든 사람이 방장이자 첫 멤버가 된다. role 은 방장 자신의 역할 (member/mentor/mentee). 없거나 모르는 값이면 member.
 * department_ids 는 대상 학과들 (없거나 빈 배열이면 학과 무관). 없는 학과 번호는 건너뛴다.
 * 고칠 때 대상 학과를 바꿔도 이미 들어온 멤버는 그대로 둔다 (새로 들어오는 사람만 검사한다). */
static void handle_study_save(Request *req, Response *res, unsigned edit_id)
{
    CurrentUser u;
    Json *in;
    char *name = NULL, *desc = NULL;
    const char *end_date, *role;
    int max, ndepts, i;
    unsigned depts[STUDY_MAX_DEPTS + 1];   /* 상한 초과를 알아채려고 한 칸 더 */
    Buf sql;
    unsigned long long gid;

    if (!auth_require(req, res, &u))
        return;
    if (edit_id) {
        int ok = study_manage_ok(edit_id, &u);
        if (ok < 0) { res_error(res, 404, "not_found", "스터디가 없습니다."); return; }
        if (!ok)    { res_error(res, 403, "not_allowed", "방장만 고칠 수 있습니다."); return; }
    }
    in = body_object(req, res);
    if (!in)
        return;

    name = str_dup(json_str(in, "name", ""));
    desc = str_dup(json_str(in, "description", ""));
    max = (int)json_int(in, "max_members", 6);
    end_date = clean_date(json_str(in, "end_date", NULL));   /* 형식이 틀리거나 없으면 NULL (종료일 없음) */
    ndepts = json_uint_array(json_get(in, "department_ids"), depts, STUDY_MAX_DEPTS + 1);
    role = role_code(json_str(in, "role", NULL));
    if (!role)
        role = "member";
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
        res_error(res, 400, "bad_max", "정원은 2~8명으로 정하세요.");
        goto out;
    }
    if (ndepts > STUDY_MAX_DEPTS) {
        res_error(res, 400, "too_many_departments", "대상 학과는 20개까지 고를 수 있습니다.");
        goto out;
    }
    if (edit_id) {
        /* 이미 들어온 멤버를 내보내지 않도록 정원은 지금 인원보다 작게 줄일 수 없다 */
        Buf chk;
        long long count;
        buf_init(&chk);
        db_sqlf(&chk, "SELECT COUNT(*) FROM study_members WHERE group_id = %u", edit_id);
        count = db_scalar(chk.data, 0);
        buf_free(&chk);
        if (max < count) {
            res_error(res, 400, "bad_max", "정원을 지금 인원보다 적게 줄일 수 없습니다.");
            goto out;
        }
        if (study_confirmed(edit_id) && max != count) {
            res_error(res, 400, "confirmed", "확정된 그룹은 정원을 바꿀 수 없습니다.");
            goto out;
        }
    }

    if (end_date) {
        /* 오늘보다 앞이면 만들자마자 지워지므로 받지 않는다 (고칠 때 원래 종료일을 그대로 두는 것은 괜찮다) */
        Buf chk;
        buf_init(&chk);
        if (edit_id)
            db_sqlf(&chk, "SELECT %Q < CURDATE() AND NOT IFNULL((SELECT end_date = %Q FROM study_groups WHERE id = %u), 0)",
                    end_date, end_date, edit_id);
        else
            db_sqlf(&chk, "SELECT %Q < CURDATE()", end_date);
        if (db_scalar(chk.data, 0)) {
            buf_free(&chk);
            res_error(res, 400, "bad_end_date", "종료일은 오늘이나 그 뒤로 정하세요.");
            goto out;
        }
        buf_free(&chk);
    }

    if (!db_begin()) {
        res_error(res, 500, "db_error", "트랜잭션을 시작할 수 없습니다.");
        goto out;
    }
    buf_init(&sql);
    if (edit_id)
        db_sqlf(&sql, "UPDATE study_groups SET name = %Q, description = %Q, max_members = %d, end_date = %Q "
                      "WHERE id = %u", name, desc, max, end_date, edit_id);
    else
        db_sqlf(&sql, "INSERT INTO study_groups (owner_id, name, description, max_members, end_date) "
                      "VALUES (%u, %Q, %Q, %d, %Q)", u.id, name, desc, max, end_date);
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "스터디를 저장할 수 없습니다.");
        goto out;
    }
    gid = edit_id ? edit_id : db_last_id();
    if (edit_id) {   /* 대상 학과는 통째로 바꾼다 */
        buf_reset(&sql);
        db_sqlf(&sql, "DELETE FROM study_departments WHERE group_id = %u", edit_id);
        if (!db_exec_buf(&sql)) {
            buf_free(&sql);
            db_rollback();
            res_error(res, 500, "db_error", "스터디를 저장할 수 없습니다.");
            goto out;
        }
    }
    /* 대상 학과: 없는 학과 번호는 SELECT 가 비어 건너뛰고, 같은 번호가 두 번 오면 IGNORE 로 하나만 남는다 */
    for (i = 0; i < ndepts; i++) {
        buf_reset(&sql);
        db_sqlf(&sql, "INSERT IGNORE INTO study_departments (group_id, department_id) "
                      "SELECT %u, id FROM departments WHERE id = %u", (unsigned)gid, depts[i]);
        if (!db_exec_buf(&sql)) {
            buf_free(&sql);
            db_rollback();
            res_error(res, 500, "db_error", "스터디를 만들 수 없습니다.");
            goto out;
        }
    }
    buf_reset(&sql);
    if (!edit_id)   /* 새 스터디: 방장을 첫 멤버로 */
        db_sqlf(&sql, "INSERT INTO study_members (group_id, user_id, role) VALUES (%u, %u, %Q)",
                (unsigned)gid, u.id, role);
    if ((!edit_id && !db_exec_buf(&sql)) || !db_commit()) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "스터디를 저장할 수 없습니다.");
        goto out;
    }
    buf_free(&sql);

    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"ok\":true,");
        json_kv_int(&b, "id", (long long)gid);
        buf_putc(&b, '}');
        res_json_buf(res, edit_id ? 200 : 201, &b);
        buf_free(&b);
    }

out:
    free(name);
    free(desc);
    json_free(in);
}

/* GET /api/studies/{id}
 * 응답: {"study":{목록과 같은 형태},"description":"전체 소개","can_manage":bool,
 *        "my_request":bool (내가 신청해 두고 수락을 기다리는지),
 *        "requests":[{user_id,nickname,department,created_at}, ...] (방장·관리자에게만, 아니면 []),
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
    write_study_row(&b, row, NULL);
    buf_puts(&b, ",\"description\":");
    json_write_str(&b, row[15]);   /* STUDY_SELECT 의 마지막 열 = 소개 전체 */
    mysql_free_result(qr);

    buf_putc(&b, ',');
    json_kv_bool(&b, "can_manage", u.id && study_manage_ok(gid, &u) == 1);

    /* 내가 참여를 신청해 두었는지 (방장 수락 대기) */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM study_requests WHERE group_id = %u AND user_id = %u", gid, u.id);
    buf_putc(&b, ',');
    json_kv_bool(&b, "my_request", u.id && db_scalar(sql.data, 0) > 0);
    buf_free(&sql);

    /* 방장·관리자에게만: 대기 중인 참여 신청 (신청한 순서) */
    buf_puts(&b, ",\"requests\":[");
    if (u.id && study_manage_ok(gid, &u) == 1) {
        buf_init(&sql);
        db_sqlf(&sql,
                "SELECT r.user_id, u.nickname, IFNULL(d.name, ''), DATE_FORMAT(r.created_at, '%%Y-%%m-%%d %%H:%%i') "
                "FROM study_requests r JOIN users u ON u.id = r.user_id "
                "LEFT JOIN departments d ON d.id = u.department_id "
                "WHERE r.group_id = %u ORDER BY r.created_at, r.user_id", gid);
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
                json_kv_str(&b, "created_at", row[3]);
                buf_putc(&b, '}');
            }
            mysql_free_result(qr);
        }
        first = 1;   /* 아래 멤버 목록에서 다시 쓴다 */
    }
    buf_putc(&b, ']');

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
int study_add_member(Response *res, unsigned gid, unsigned uid, const char *role)
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
    if (!study_department_ok(gid, uid)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 403, "department_not_allowed", "이 스터디의 대상 학과가 아니어서 참여할 수 없습니다.");
        return 0;
    }
    if (study_confirmed(gid)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 409, "confirmed", "인원이 모두 모여 확정된 그룹입니다.");
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
    if (!db_exec_buf(&sql)) {
        buf_free(&sql);
        db_rollback();
        res_error(res, 500, "db_error", "멤버를 추가할 수 없습니다.");
        return 0;
    }
    /* 이 사람의 참여 신청은 이제 필요 없다 (권유 수락·방장의 직접 추가로 들어온 경우에도) */
    buf_reset(&sql);
    db_sqlf(&sql, "DELETE FROM study_requests WHERE group_id = %u AND user_id = %u", gid, uid);
    db_exec_buf(&sql);
    /* 마지막 자리가 찼으면 확정된 그룹으로 */
    if (count + 1 >= max) {
        buf_reset(&sql);
        db_sqlf(&sql, "UPDATE study_groups SET confirmed_at = NOW() WHERE id = %u", gid);
        db_exec_buf(&sql);
    }
    buf_free(&sql);
    if (!db_commit()) {
        db_rollback();
        res_error(res, 500, "db_error", "멤버를 추가할 수 없습니다.");
        return 0;
    }
    if (count + 1 >= max)
        study_announce_confirmed(gid);   /* 알림은 커밋한 뒤에 (트랜잭션 잠금을 오래 쥐지 않게) */
    return 1;
}

/* POST /api/studies/{id}/join - 참여 신청. 바로 멤버가 되지 않고 방장에게 알림이 간다 (방장이 수락하면 멤버)
 * DELETE 같은 경로 - 내 신청 취소 */
static void handle_study_join(Request *req, Response *res, unsigned gid, int cancel)
{
    CurrentUser u;
    Buf sql;
    long long owner, n, max, count;
    char name[304], title[512], link[32];

    if (!auth_require(req, res, &u))
        return;
    owner = study_owner(gid);
    if (owner < 0) {
        res_error(res, 404, "not_found", "스터디가 없습니다.");
        return;
    }
    buf_init(&sql);
    if (cancel) {
        db_sqlf(&sql, "DELETE FROM study_requests WHERE group_id = %u AND user_id = %u", gid, u.id);
        db_exec_buf(&sql);
        buf_free(&sql);
        res_json(res, 200, "{\"ok\":true}");
        return;
    }

    db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u AND user_id = %u", gid, u.id);
    n = db_scalar(sql.data, 0);
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT max_members FROM study_groups WHERE id = %u", gid);
    max = db_scalar(sql.data, 0);
    buf_reset(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u", gid);
    count = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (n) {
        res_error(res, 409, "already_member", "이미 이 스터디의 멤버입니다.");
        return;
    }
    if (study_confirmed(gid) || count >= max) {
        res_error(res, 409, "confirmed", "인원이 모두 모여 확정된 그룹입니다.");
        return;
    }
    if (!study_department_ok(gid, u.id)) {
        res_error(res, 403, "department_not_allowed", "이 스터디의 대상 학과가 아니어서 참여할 수 없습니다.");
        return;
    }

    buf_init(&sql);
    db_sqlf(&sql, "INSERT IGNORE INTO study_requests (group_id, user_id) VALUES (%u, %u)", gid, u.id);
    db_lock();   /* db_affected 가 다른 스레드의 값이 되지 않게 */
    n = db_exec_buf(&sql) ? (long long)db_affected() : -1;
    db_unlock();
    buf_free(&sql);
    if (n < 0) {
        res_error(res, 500, "db_error", "참여 신청을 저장할 수 없습니다.");
        return;
    }
    if (n == 0) {
        res_error(res, 409, "already_requested", "이미 참여를 신청했습니다. 방장의 수락을 기다려 주세요.");
        return;
    }

    study_name(gid, name, sizeof name);
    snprintf(title, sizeof title, "%s 님이 「%s」 스터디 참여를 신청했습니다", u.nickname, name);
    snprintf(link, sizeof link, "#/studies/%u", gid);
    notify_add((unsigned)owner, u.id, "study_request", title,
               "스터디 화면의 참여 신청 목록에서 수락하거나 거절할 수 있습니다.", link);
    res_json(res, 201, "{\"ok\":true,\"requested\":true}");
}

/* POST /api/studies/{id}/requests/{uid} - 참여 신청 수락 (방장·관리자)
 * DELETE 같은 경로 - 거절. 어느 쪽이든 신청한 사람에게 알린다 */
static void handle_study_request(Request *req, Response *res, unsigned gid, unsigned uid, int accept)
{
    CurrentUser u;
    Buf sql;
    long long n;
    int ok;
    char name[304], title[400], link[32];

    if (!auth_require(req, res, &u))
        return;
    ok = study_manage_ok(gid, &u);
    if (ok < 0) { res_error(res, 404, "not_found", "스터디가 없습니다."); return; }
    if (!ok)    { res_error(res, 403, "not_allowed", "방장만 참여 신청을 처리할 수 있습니다."); return; }

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM study_requests WHERE group_id = %u AND user_id = %u", gid, uid);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    if (!n) {
        res_error(res, 404, "no_request", "참여 신청이 없습니다. 이미 처리했거나 신청을 취소했습니다.");
        return;
    }

    study_name(gid, name, sizeof name);
    snprintf(link, sizeof link, "#/studies/%u", gid);
    if (accept) {
        if (!study_add_member(res, gid, uid, "member"))   /* 정원·학과·확정 검사, 신청 삭제, 확정 처리까지 */
            return;
        snprintf(title, sizeof title, "「%s」 스터디 참여가 수락되었습니다", name);
        notify_add(uid, u.id, "study_accept", title, "이제 스터디 멤버입니다. 채팅방에 들어가 인사해 보세요.", link);
        res_json(res, 201, "{\"ok\":true}");
    } else {
        buf_init(&sql);
        db_sqlf(&sql, "DELETE FROM study_requests WHERE group_id = %u AND user_id = %u", gid, uid);
        db_exec_buf(&sql);
        buf_free(&sql);
        snprintf(title, sizeof title, "「%s」 스터디 참여 신청이 거절되었습니다", name);
        notify_add(uid, u.id, "study_reject", title, "", link);
        res_json(res, 200, "{\"ok\":true}");
    }
}

/* GET /api/studies/{id}/candidates?q=이름 또는 학과 (방장·관리자)
 * 아직 멤버가 아니고 대상 학과에 맞는 학생을 표시 이름이나 학과 이름으로 찾는다. 학번으로는 찾지 않는다.
 * q 가 비어 있으면 추가할 수 있는 학생을 이름순으로 보여 준다. */
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
    str_trim(q);   /* 빈 검색어면 아래 패턴이 '%%' 가 되어 모두 고른다 */

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
            /* 대상 학과가 정해진 스터디면 그 학과 학생만 */
            "  AND (NOT EXISTS (SELECT 1 FROM study_departments sd WHERE sd.group_id = %u) "
            "       OR u.department_id IN (SELECT sd.department_id FROM study_departments sd WHERE sd.group_id = %u)) "
            "ORDER BY u.nickname LIMIT %d",
            pat.data, pat.data, gid, gid, gid, CANDIDATE_MAX);
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

    if (study_add_member(res, gid, uid, role))
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
    /* 확정된 그룹은 멤버가 바뀌지 않는다 (관리자만 예외: 문제가 생긴 회원을 정리할 수 있게) */
    if (!u.is_admin && study_confirmed(gid)) {
        res_error(res, 409, "confirmed", "확정된 그룹은 나가거나 내보낼 수 없습니다.");
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
    if (req_is(req, "POST", "/api/studies")) { handle_study_save(req, res, 0); return 1; }

    if (path_two_ids(req->path, "/members/", &gid, &uid)) {
        if (!strcmp(m, "PUT"))    { handle_study_role(req, res, gid, uid);   return 1; }
        if (!strcmp(m, "DELETE")) { handle_study_remove(req, res, gid, uid); return 1; }
        return 0;
    }
    if (path_two_ids(req->path, "/requests/", &gid, &uid)) {
        if (!strcmp(m, "POST"))   { handle_study_request(req, res, gid, uid, 1); return 1; }
        if (!strcmp(m, "DELETE")) { handle_study_request(req, res, gid, uid, 0); return 1; }
        return 0;
    }
    if ((!strcmp(m, "POST") || !strcmp(m, "DELETE")) && path_id(req->path, "/api/studies/", "/join", &gid)) {
        handle_study_join(req, res, gid, !strcmp(m, "DELETE"));
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
    if (!strcmp(m, "PUT") && path_id(req->path, "/api/studies/", NULL, &gid)) {
        handle_study_save(req, res, gid);
        return 1;
    }
    if (!strcmp(m, "DELETE") && path_id(req->path, "/api/studies/", NULL, &gid)) {
        handle_study_delete(req, res, gid);
        return 1;
    }
    return 0;
}
