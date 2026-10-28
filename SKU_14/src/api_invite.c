/* 참여 권유 (팀원 모집글 · 그룹 스터디에 회원을 메일로 초대)
 *
 *   POST /api/invitations                 {kind:"recruit"|"study", id, user_id} (모집글 작성자·스터디 방장)
 *   POST /api/notifications/{id}/accept   알림함의 권유 알림에서 [참여하기] (권유받은 회원)
 *
 * [참여하기] 는 대상에 따라 다르게 동작한다.
 *   그룹 스터디   → 바로 멤버로 들어간다 (역할은 member, 방장이 나중에 멘토·멘티로 정할 수 있다).
 *   팀원 모집글   → 팀원(recruit_members)이 되고, 글쓴이가 볼 수 있게 "참여하고 싶다" 는 댓글도 남긴다.
 * 어느 쪽이든 권유한 사람(작성자·방장)의 알림함에 수락 알림이 가고, 같은 권유는 한 번만 수락할 수 있다.
 *
 * 흐름: 작성자/방장이 자기 글의 "팀원 찾기" 화면(GET /api/members/matches?kind=&id=)에서
 *       잘 맞는 회원을 보고 [참여 권유] 를 누르면, 그 회원의 알림함(api_notify.c)에 알림이 들어가고
 *       인증한 학교 메일로도 안내 메일이 간다. 메일은 덤이다: 메일 서버가 없거나 보내기에 실패해도
 *       알림함에는 들어갔으므로 권유는 성공으로 본다 (응답의 mailed 로 메일 여부를 알려 준다).
 * 같은 모임에서 같은 회원에게는 한 번만 보낼 수 있다 (invitations 의 유일 키).
 * 받는 사람의 메일 주소는 화면에 내보내지 않는다. 서버가 DB 에서 꺼내 바로 보낸다.
 * 스팸을 막으려고 한 사람이 24시간 동안 보낼 수 있는 권유는 INVITE_DAILY_MAX 건이다.
 *
 * 관련 테이블:
 *   invitations  (대상 종류, 대상 id, 받는 회원, 수락 시각) - 보낸 권유 기록
 */
#include "api.h"
#include "db.h"
#include "mailer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INVITE_DAILY_MAX 30   /* 한 사람이 24시간 동안 보낼 수 있는 권유 수 */

/* ------------------------------------------------------------ 대상 (api.h 참고) */

int invite_target(const char *kind, unsigned id, InviteTarget *t)
{
    Buf sql;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    int found = 0;

    memset(t, 0, sizeof *t);
    if (!kind || !id)
        return 0;

    buf_init(&sql);
    if (!strcmp(kind, "recruit")) {
        db_sqlf(&sql, "SELECT author_id, title, status = 'open', 'recruit' FROM recruits WHERE id = %u", id);
        str_copy(t->kind, sizeof t->kind, "recruit");
    } else if (!strcmp(kind, "study")) {
        /* 스터디: 정원이 남아 있어야 권유할 수 있다 */
        db_sqlf(&sql, "SELECT g.owner_id, g.name, "
                      "       (SELECT COUNT(*) FROM study_members m WHERE m.group_id = g.id) < g.max_members, 'study' "
                      "FROM study_groups g WHERE g.id = %u", id);
        str_copy(t->kind, sizeof t->kind, "study");
    } else {
        buf_free(&sql);
        return 0;
    }
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (row) {
        t->id = id;
        t->owner_id = (unsigned)strtoul(row[0], NULL, 10);
        str_copy(t->title, sizeof t->title, row[1]);
        t->open = row[2][0] == '1';
        str_copy(t->subkind, sizeof t->subkind, row[3]);   /* recruit / study */
        found = 1;
    }
    if (qr)
        mysql_free_result(qr);
    return found;
}

int invite_member_already(const InviteTarget *t, unsigned user_id)
{
    Buf sql;
    long long n;

    if (user_id == t->owner_id)
        return 1;
    buf_init(&sql);
    if (strcmp(t->kind, "study") == 0)
        db_sqlf(&sql, "SELECT COUNT(*) FROM study_members WHERE group_id = %u AND user_id = %u", t->id, user_id);
    else
        db_sqlf(&sql, "SELECT COUNT(*) FROM recruit_members WHERE recruit_id = %u AND user_id = %u", t->id, user_id);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

int invite_sent(const InviteTarget *t, unsigned user_id)
{
    Buf sql;
    long long n;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM invitations WHERE target_kind = '%s' AND target_id = %u AND invitee_id = %u",
            t->kind, t->id, user_id);
    n = db_scalar(sql.data, 0);
    buf_free(&sql);
    return n > 0;
}

/* 대상 종류의 화면 이름과 주소(#/...) 앞부분 */
static const char *target_label(const InviteTarget *t)
{
    if (!strcmp(t->subkind, "study")) return "그룹 스터디";
    return "팀원 모집";
}

static const char *target_path(const InviteTarget *t)
{
    if (!strcmp(t->subkind, "study")) return "studies";
    return "recruits";
}

/* ------------------------------------------------------------ 권유 보내기 */

/* POST /api/invitations {kind, id, user_id}
 * 기록을 먼저 넣어(유일 키로 두 번 누르기를 막고) 메일을 보낸다. 메일이 실패하면 기록을 지워 다시 누를 수 있게 한다. */
static void handle_invite(Request *req, Response *res)
{
    CurrentUser u;
    Json *in;
    InviteTarget t;
    unsigned uid, id;
    char kind[12];
    Buf sql, text, subject;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char email[128] = "", nickname[64] = "", site[256], err[256], link[64];
    int ok, mailed = 0;

    if (!auth_require(req, res, &u))
        return;
    in = body_object(req, res);
    if (!in)
        return;
    str_copy(kind, sizeof kind, json_str(in, "kind", ""));
    id = (unsigned)json_int(in, "id", 0);
    uid = (unsigned)json_int(in, "user_id", 0);
    json_free(in);

    if (!invite_target(kind, id, &t)) {
        res_error(res, 404, "not_found", "권유할 모집글이나 스터디가 없습니다.");
        return;
    }
    if (t.owner_id != u.id) {
        res_error(res, 403, "not_allowed", "글쓴이(방장)만 참여를 권유할 수 있습니다.");
        return;
    }
    if (!t.open) {
        res_error(res, 400, "closed", strcmp(t.kind, "recruit") ? "정원이 찼습니다." : "마감한 모집글입니다.");
        return;
    }
    if (!uid || invite_member_already(&t, uid)) {
        res_error(res, 400, "bad_user", "이미 함께하고 있는 회원입니다.");
        return;
    }
    if (invite_sent(&t, uid)) {
        res_error(res, 409, "already_invited", "이미 참여를 권유한 회원입니다.");
        return;
    }

    /* 받는 사람: 학생이어야 한다 (메일 주소가 없어도 알림함으로는 간다) */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT IFNULL(email, ''), nickname FROM users WHERE id = %u AND role = 'student'", uid);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    if (row) {
        str_copy(email, sizeof email, row[0]);
        str_copy(nickname, sizeof nickname, row[1]);
    }
    if (qr)
        mysql_free_result(qr);
    if (!row) {
        res_error(res, 404, "bad_user", "회원이 없습니다.");
        return;
    }

    /* 하루 한도 */
    buf_init(&sql);
    db_sqlf(&sql, "SELECT COUNT(*) FROM invitations WHERE inviter_id = %u AND created_at > NOW() - INTERVAL 1 DAY", u.id);
    ok = db_scalar(sql.data, 0) < INVITE_DAILY_MAX;
    buf_free(&sql);
    if (!ok) {
        res_error(res, 429, "too_many", "참여 권유는 하루 30건까지 보낼 수 있습니다.");
        return;
    }

    /* 기록 먼저 (동시에 두 번 눌러도 유일 키 때문에 하나만 들어간다) */
    buf_init(&sql);
    db_sqlf(&sql, "INSERT INTO invitations (target_kind, target_id, inviter_id, invitee_id) VALUES ('%s', %u, %u, %u)",
            t.kind, t.id, u.id, uid);
    ok = db_exec_buf(&sql);
    buf_free(&sql);
    if (!ok) {
        if (db_errno() == DB_ERR_DUP_ENTRY)
            res_error(res, 409, "already_invited", "이미 참여를 권유한 회원입니다.");
        else
            res_error(res, 500, "db_error", "권유를 저장할 수 없습니다.");
        return;
    }

    /* 알림함: 이것이 권유의 본체다. 실패하면 기록을 되돌려 다시 누를 수 있게 한다. */
    snprintf(link, sizeof link, "#/%s/%u", target_path(&t), t.id);
    buf_init(&subject);
    buf_printf(&subject, "%s 님이 %s 참여를 권유했습니다", u.nickname, target_label(&t));
    buf_init(&text);
    buf_printf(&text, "%s%s'%s' 에 함께하자는 권유입니다. 눌러서 내용을 확인해 보세요.",
               u.department_name, u.department_name[0] ? " · " : "", t.title);
    ok = notify_add(uid, u.id, "invite", subject.data, text.data, link);
    buf_free(&subject);
    buf_free(&text);
    if (!ok) {
        buf_init(&sql);
        db_sqlf(&sql, "DELETE FROM invitations WHERE target_kind = '%s' AND target_id = %u AND invitee_id = %u",
                t.kind, t.id, uid);
        db_exec_buf(&sql);
        buf_free(&sql);
        res_error(res, 500, "db_error", "알림을 저장할 수 없습니다.");
        return;
    }

    /* 메일 (덤): 메일 주소가 있고 메일 서버가 설정돼 있을 때만 */
    if (email[0] && (mail_enabled() || mail_dev_mode())) {
        api_site_url(req, site, sizeof site);
        buf_init(&subject);
        buf_printf(&subject, "[SKU14] %s 님이 %s 참여를 권유했습니다", u.nickname, target_label(&t));
        buf_init(&text);
        buf_printf(&text,
                   "안녕하세요, %s 님.\n\n"
                   "%s 님%s%s%s이 SKU14 에서 아래 %s에 함께하자고 권유했습니다.\n\n"
                   "  %s\n\n"
                   "자세히 보기: %s/%s\n\n"
                   "관심이 있다면 링크에서 내용을 확인하고 댓글이나 [참여하기] 로 답해 주세요.\n"
                   "SKU14 에 로그인하면 알림함에서도 이 권유를 볼 수 있습니다.\n\n"
                   "-- \n"
                   "이 메일은 SKU14 회원이 '참여 권유' 버튼을 눌러 보냈습니다.\n"
                   "같은 모임에서는 한 번만 보낼 수 있으며, 회원님의 메일 주소는 상대에게 알려지지 않습니다.\n",
                   nickname, u.nickname,
                   u.department_name[0] ? "(" : "", u.department_name, u.department_name[0] ? ")" : "",
                   target_label(&t), t.title, site, link);
        mailed = mail_send(email, subject.data, text.data, err, sizeof err);
        if (!mailed)
            log_warn("참여 권유 메일 실패 (회원 %u, 알림함에는 들어감): %s", uid, err);
        buf_free(&subject);
        buf_free(&text);
    }

    log_info("참여 권유: 회원 %u -> %u (%s %u, 메일 %s)", u.id, uid, t.subkind, t.id, mailed ? "보냄" : "안 보냄");
    res_json(res, 201, mailed ? "{\"ok\":true,\"mailed\":true}" : "{\"ok\":true,\"mailed\":false}");
}

/* ------------------------------------------------------------ 알림함에서 수락 */

/* 알림의 link("#/recruits/3", "#/studies/4")로 대상을 찾는다. 있으면 1.
 * "#/mentoring/5" 는 멘토링 메뉴를 없애기 전에 보낸 알림이다 (그 모임은 이제 그룹 스터디다).
 * link 는 권유를 보낼 때 서버가 만든 값이다 (handle_invite). */
static int target_from_link(const char *link, InviteTarget *t)
{
    static const struct { const char *prefix, *kind; } map[] = {
        { "#/recruits/", "recruit" }, { "#/studies/", "study" }, { "#/mentoring/", "study" },
    };
    size_t i;

    for (i = 0; link && i < sizeof map / sizeof map[0]; i++) {
        size_t n = strlen(map[i].prefix);
        char *end;
        unsigned long id;
        if (strncmp(link, map[i].prefix, n) != 0)
            continue;
        id = strtoul(link + n, &end, 10);
        if (*end != '\0' || id == 0 || id > 0xFFFFFFFFul)
            return 0;
        return invite_target(map[i].kind, (unsigned)id, t);
    }
    return 0;
}

/* 받은 권유의 수락 여부. 반환: 1 = 수락함, 0 = 아직, -1 = 권유 기록 없음 */
static int invite_accepted(const InviteTarget *t, unsigned user_id)
{
    Buf sql;
    long long v;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT accepted_at IS NOT NULL FROM invitations "
                  "WHERE target_kind = '%s' AND target_id = %u AND invitee_id = %u", t->kind, t->id, user_id);
    v = db_scalar(sql.data, -1);
    buf_free(&sql);
    return (int)v;
}

const char *invite_state(const char *link, unsigned user_id)
{
    InviteTarget t;
    int acc;

    if (!target_from_link(link, &t))
        return "gone";
    acc = invite_accepted(&t, user_id);
    if (acc < 0)
        return "gone";
    if (acc > 0)
        return "accepted";
    if (invite_member_already(&t, user_id))
        return "member";
    if (!t.open)
        return "closed";
    return "open";
}

/* POST /api/notifications/{id}/accept
 * 먼저 accepted_at 을 "아직이면" 채워(두 번 눌러도 하나만 통과) 놓고 참여 처리를 한다.
 * 참여 처리가 실패하면 accepted_at 을 되돌려 다시 누를 수 있게 한다. */
static void handle_accept(Request *req, Response *res, unsigned nid)
{
    CurrentUser u;
    InviteTarget t;
    Buf sql, title, body;
    MYSQL_RES *qr;
    MYSQL_ROW row;
    char kind[24] = "", link[200] = "";
    unsigned long long changed;
    int ok, found;

    if (!auth_require(req, res, &u))
        return;

    buf_init(&sql);
    db_sqlf(&sql, "SELECT kind, link FROM notifications WHERE id = %u AND user_id = %u", nid, u.id);
    qr = db_query_buf(&sql);
    buf_free(&sql);
    row = qr ? mysql_fetch_row(qr) : NULL;
    found = row != NULL;
    if (row) {
        str_copy(kind, sizeof kind, row[0]);
        str_copy(link, sizeof link, row[1]);
    }
    if (qr)
        mysql_free_result(qr);
    if (!found) {
        res_error(res, 404, "not_found", "알림이 없습니다.");
        return;
    }
    if (strcmp(kind, "invite") != 0) {
        res_error(res, 400, "not_invite", "참여 권유 알림이 아닙니다.");
        return;
    }
    if (!target_from_link(link, &t) || invite_accepted(&t, u.id) < 0) {
        res_error(res, 404, "gone", "권유받은 글이 지워졌습니다.");
        return;
    }
    if (strcmp(t.kind, "recruit") == 0 && !t.open) {
        res_error(res, 400, "closed", "모집을 마감한 글입니다.");
        return;
    }

    /* 수락 표시 (아직일 때만) */
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE invitations SET accepted_at = NOW() WHERE target_kind = '%s' AND target_id = %u "
                  "AND invitee_id = %u AND accepted_at IS NULL", t.kind, t.id, u.id);
    db_lock();   /* db_affected 가 다른 스레드의 값이 되지 않게 */
    ok = db_exec_buf(&sql);
    changed = ok ? db_affected() : 0;
    db_unlock();
    buf_free(&sql);
    if (!ok) {
        res_error(res, 500, "db_error", "권유를 수락할 수 없습니다.");
        return;
    }
    if (!changed) {
        res_error(res, 409, "already_accepted", "이미 수락한 권유입니다.");
        return;
    }

    /* 참여 처리 */
    if (!strcmp(t.kind, "study")) {
        /* 이미 멤버면(스스로 들어온 경우) 수락 표시만 남긴다 */
        ok = invite_member_already(&t, u.id) || study_add_member(res, t.id, u.id, "member");
    } else {
        buf_init(&sql);
        db_sqlf(&sql, "INSERT INTO recruit_comments (recruit_id, author_id, body) VALUES (%u, %u, %Q)",
                t.id, u.id, "✋ 참여 권유를 받고 함께하고 싶어 참여합니다! 연락 방법을 알려 주세요.");
        ok = db_exec_buf(&sql) && recruit_team_add(t.id, u.id);
        buf_free(&sql);
        if (!ok)
            res_error(res, 500, "db_error", "팀에 들어갈 수 없습니다.");
    }
    if (!ok) {   /* 응답은 위에서 채웠다 (예: 정원이 참). 다시 누를 수 있게 되돌린다 */
        buf_init(&sql);
        db_sqlf(&sql, "UPDATE invitations SET accepted_at = NULL WHERE target_kind = '%s' AND target_id = %u "
                      "AND invitee_id = %u", t.kind, t.id, u.id);
        db_exec_buf(&sql);
        buf_free(&sql);
        return;
    }

    /* 이 알림은 읽은 것으로, 권유한 사람에게는 수락 알림 */
    buf_init(&sql);
    db_sqlf(&sql, "UPDATE notifications SET read_at = IFNULL(read_at, NOW()) WHERE id = %u", nid);
    db_exec_buf(&sql);
    buf_free(&sql);

    buf_init(&title);
    buf_printf(&title, "%s 님이 %s 참여 권유를 수락했습니다", u.nickname, target_label(&t));
    buf_init(&body);
    buf_printf(&body, strcmp(t.kind, "study") == 0 ? "'%s' 의 멤버가 되었습니다. 역할(멘토·멘티)을 정해 보세요."
                                                   : "'%s' 의 팀원이 되었습니다. 팀 채팅방에서 인사해 보세요.", t.title);
    notify_add(t.owner_id, u.id, "invite_accepted", title.data, body.data, link);
    buf_free(&title);
    buf_free(&body);

    log_info("참여 권유 수락: 회원 %u (%s %u)", u.id, t.subkind, t.id);
    buf_init(&body);
    buf_puts(&body, "{\"ok\":true,");
    json_kv_str(&body, "kind", t.subkind);
    buf_putc(&body, ',');
    json_kv_str(&body, "link", link);
    buf_putc(&body, '}');
    res_json_buf(res, 200, &body);
    buf_free(&body);
}

/* ------------------------------------------------------------ 라우터 */

int route_invite(Request *req, Response *res)
{
    unsigned id;

    if (req_is(req, "POST", "/api/invitations")) { handle_invite(req, res); return 1; }
    if (!strcmp(req->method, "POST") && path_id(req->path, "/api/notifications/", "/accept", &id)) {
        handle_accept(req, res, id);
        return 1;
    }
    return 0;
}
