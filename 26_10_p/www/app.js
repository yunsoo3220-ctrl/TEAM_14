/* SKU14 공모전 게시판 - 프런트엔드 (의존성 없음)
 * 서버의 /api/* 를 호출하고 해시 라우팅으로 화면을 바꾼다. */

'use strict';

const state = {
  me: null,
  colleges: [],     /* [{id, name, departments:[{id,name}]}] */
};

const KIND_LABEL = { contest: '공모전', hackathon: '해커톤', etc: '기타' };

/* ══════════════════════════════════════════════════════════ 유틸 */

async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: 'same-origin',
    headers: options.body ? { 'Content-Type': 'application/json' } : {},
    ...options,
  });

  let data = null;
  try { data = await res.json(); } catch { /* 본문 없는 응답 */ }

  if (!res.ok) {
    const msg = data && data.error ? data.error.message : `요청 실패 (${res.status})`;
    const err = new Error(msg);
    err.code = data && data.error ? data.error.code : String(res.status);
    throw err;
  }
  return data;
}

let toastTimer = null;
function toast(message) {
  const el = document.getElementById('toast');
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3200);
}

const tpl = id => document.getElementById(id).content.cloneNode(true);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
};

function render(node) {
  document.getElementById('view').replaceChildren(node);
  window.scrollTo(0, 0);
}

function renderMessage(text) {
  render(el('p', 'empty', text));
}

function formValues(form) {
  const out = {};
  for (const [k, v] of new FormData(form)) out[k] = typeof v === 'string' ? v.trim() : v;
  return out;
}

function daysLeft(dateStr) {
  const d = new Date(`${dateStr}T23:59:59`);
  if (Number.isNaN(d.getTime())) return null;
  return Math.ceil((d - Date.now()) / 86400000);
}

/* ═════════════════════════════════════════════ 사이드바 / 상단 / 히어로 */

function setPageTitle(text) {
  document.getElementById('page-title').textContent = text;
}

function markNav(name) {
  for (const a of document.querySelectorAll('[data-nav]'))
    a.classList.toggle('on', a.dataset.nav === name);
}

function paintChrome() {
  const me = state.me;

  document.getElementById('nav-admin').hidden = !(me && me.is_admin);
  document.getElementById('nav-login-label').firstChild.nodeValue =
    me ? '로그아웃' : '로그인 · 가입';

  /* 상단 오른쪽 */
  const who = document.getElementById('who');
  who.replaceChildren();

  if (!me) {
    const a = el('a', null, '로그인 / 가입');
    a.href = '#/login';
    who.append(a);
  } else {
    const avatar = el('span', 'avatar', me.nickname.slice(0, 1));
    const box = el('span', 'whoami');
    box.append(
      el('b', null, `${me.nickname} 님`),
      el('small', null, me.is_admin ? '관리자' : (me.department_name || '소속 없음')),
    );
    const out = el('button', 'ghost small', '로그아웃');
    out.onclick = logout;
    who.append(avatar, box, out);
  }

  paintHero();
}

function paintHero() {
  const hero = document.getElementById('hero');
  const me = state.me;
  hero.replaceChildren();

  if (!me) {
    hero.append(el('h2', null, '서경대 공모전, 한눈에.'));
    const sub = el('p', 'sub');
    sub.append(
      el('span', null, '학교 공지의 공모전·해커톤을 모아 보여줍니다'),
      el('span', null, '목록은 로그인 없이 볼 수 있습니다'),
    );
    const act = el('div', 'act');
    const a = el('a', 'btn-light', '로그인하고 내 학과 공모전 보기');
    a.href = '#/login';
    act.append(a);
    hero.append(sub, act);
    return;
  }

  const h = el('h2');
  h.append('안녕하세요. ', el('u', null, `${me.nickname}님`), '!');
  hero.append(h);

  const sub = el('p', 'sub');
  if (me.is_admin) {
    sub.append(el('span', null, '관리자'), el('span', null, `학번 ${me.student_no}`));
  } else {
    if (me.college_name) sub.append(el('span', null, me.college_name));
    sub.append(
      el('span', null, me.department_name || '소속 없음'),
      el('span', null, `학번 ${me.student_no}`),
    );
  }
  hero.append(sub);
}

async function logout() {
  await api('/api/logout', { method: 'POST' });
  state.me = null;
  paintChrome();
  /* 해시가 바뀌면 hashchange 가 route() 를 부른다. 이미 목록이면 직접 부른다. */
  if (location.hash === '#/posts' || location.hash === '') route();
  else location.hash = '#/posts';
}

/* ══════════════════════════════════════════════════════ 타일 */

function tile(cls, value, label, note) {
  const n = el('div', `tile ${cls}`);
  n.append(el('b', null, value), el('span', null, label));
  if (note) n.append(el('small', null, note));
  return n;
}

function linkTile(href, cls, value, label, note) {
  const n = tile(cls, value, label, note);
  const a = document.createElement('a');
  a.href = href;
  a.className = n.className;
  a.replaceChildren(...n.childNodes);
  return a;
}

/* ══════════════════════════════════════════════════ 게시판 목록 */

function postItem(p) {
  const a = el('a', 'item');
  a.href = `#/posts/${p.id}`;

  const top = el('div', 'item-top');
  top.append(el('span', 'tag', KIND_LABEL[p.kind] || p.kind));

  if (p.deadline) {
    const left = daysLeft(p.deadline);
    if (left !== null && left >= 0)
      top.append(el('span', left <= 7 ? 'tag warn' : 'tag plain',
                    left === 0 ? '오늘 마감' : `D-${left}`));
    else
      top.append(el('span', 'tag plain', '마감'));
  }
  if (p.can_comment) top.append(el('span', 'tag ok', '참여 가능'));

  const meta = el('div', 'item-meta');
  const bits = [];
  if (p.host) bits.push(p.host);
  if (p.deadline) bits.push(`마감 ${p.deadline}`);
  if (p.need_people > 0) bits.push(`${p.need_people}명 모집`);
  bits.push(`댓글 ${p.comment_count}`);
  bits.push(p.created_at);
  for (const b of bits) meta.append(el('span', null, b));

  a.append(top, el('h3', null, p.title), meta);
  a.append(el('div', 'item-depts', p.departments
    ? `대상: ${p.departments}`
    : '대상 학과 미지정 — 로그인하면 누구나 댓글 가능'));

  return a;
}

async function viewList(forceMine) {
  markNav(forceMine ? 'mine' : 'posts');
  setPageTitle(forceMine ? '내 학과 대상 공모전' : '공모전 목록');

  const node = tpl('tpl-list');
  const root = node.firstElementChild;
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const tilesEl = root.querySelector('[data-tiles]');
  const deptSel = root.querySelector('[data-f="department"]');

  for (const c of state.colleges) {
    const group = document.createElement('optgroup');
    group.label = c.name;
    for (const d of c.departments) group.append(new Option(d.name, d.id));
    deptSel.append(group);
  }

  const filters = {
    department: deptSel,
    kind: root.querySelector('[data-f="kind"]'),
    mine: root.querySelector('[data-f="mine"]'),
  };
  if (!state.me) filters.mine.disabled = true;
  if (forceMine && state.me) filters.mine.checked = true;

  async function reload() {
    const qs = new URLSearchParams();
    if (filters.department.value) qs.set('department_id', filters.department.value);
    if (filters.kind.value) qs.set('kind', filters.kind.value);
    if (filters.mine.checked) qs.set('mine', '1');

    listEl.replaceChildren(el('p', 'empty', '불러오는 중…'));
    try {
      const { posts } = await api(`/api/posts?${qs}`);
      countEl.textContent = `(${posts.length})`;
      listEl.replaceChildren(...(posts.length
        ? posts.map(postItem)
        : [el('p', 'empty', '조건에 맞는 공모전이 없습니다.')]));
    } catch (e) {
      listEl.replaceChildren(el('p', 'empty', e.message));
    }
  }

  for (const f of Object.values(filters)) f.onchange = reload;

  render(node);
  reload();

  /* 타일은 필터와 무관한 전체 집계라 따로 한 번 불러온다. */
  try {
    const { posts } = await api('/api/posts');
    const soon = posts.filter(p => {
      const d = p.deadline ? daysLeft(p.deadline) : null;
      return d !== null && d >= 0 && d <= 7;
    }).length;
    const forMe = posts.filter(p => p.can_comment).length;

    tilesEl.replaceChildren(
      tile('c1', posts.length, '전체 공모전', '학교 공지에서 수집'),
      state.me
        ? linkTile('#/mine', 'c2', forMe, '내가 참여 가능', state.me.department_name || '관리자')
        : tile('c2', '—', '내가 참여 가능', '로그인이 필요합니다'),
      tile('c3', soon, '마감 7일 이내', soon ? '서두르세요' : '임박한 공모전 없음'),
      tile('c4', posts.filter(p => p.kind === 'hackathon').length, '해커톤', '개발·아이디어'),
    );
  } catch { /* 타일은 없어도 목록은 보여준다 */ }
}

/* ══════════════════════════════════════════════════ 게시물 상세 */

function commentRow(c, onDelete) {
  const wrap = el('div', 'comment');
  const head = el('div', 'comment-head');

  head.append(el('strong', null, c.author_nickname));
  if (c.author_department) head.append(el('span', null, c.author_department));
  head.append(el('span', null, c.created_at));

  if (state.me && (state.me.id === c.author_id || state.me.is_admin)) {
    const del = el('button', 'ghost small', '삭제');
    del.onclick = () => onDelete(c.id);
    head.append(del);
  }

  wrap.append(head, el('p', 'comment-body', c.body));
  return wrap;
}

async function viewDetail(id) {
  markNav('posts');
  setPageTitle('공모전 상세');

  let data;
  try {
    data = await api(`/api/posts/${id}`);
  } catch (e) {
    renderMessage(e.message);
    return;
  }

  const node = tpl('tpl-detail');
  const root = node.firstElementChild;
  const { post, comments, permission } = data;
  const reloadDetail = () => viewDetail(id);

  root.querySelector('[data-kind]').textContent = KIND_LABEL[post.kind] || post.kind;
  root.querySelector('[data-title]').textContent = post.title;
  root.querySelector('[data-host]').textContent = post.host || '-';
  root.querySelector('[data-deadline]').textContent = post.deadline || '-';
  root.querySelector('[data-need]').textContent =
    post.need_people > 0 ? `${post.need_people}명` : '미정';
  root.querySelector('[data-created]').textContent = post.created_at;
  root.querySelector('[data-body]').textContent = post.body;

  const src = root.querySelector('[data-source]');
  if (post.source_url) {
    const a = el('a', null, '원본 공지 보기 →');
    a.href = post.source_url;
    a.target = '_blank';
    a.rel = 'noopener';
    src.append(a);
  } else {
    src.remove();
  }

  /* 대상 학과 */
  const chips = root.querySelector('[data-depts]');
  if (post.departments.length) {
    chips.replaceChildren(...post.departments.map(d => {
      const s = el('span', 'chip' + (state.me && state.me.department_id === d.id ? ' me' : ''),
                   d.name);
      s.title = d.college_name;
      return s;
    }));
  } else {
    chips.replaceChildren(el('span', 'chip', '전체 — 대상 학과 미지정'));
    root.querySelector('.depts .hint').textContent =
      '대상 학과를 지정하지 않아 로그인한 누구나 댓글을 쓸 수 있습니다.';
  }

  render(node);

  if (state.me && state.me.is_admin)
    mountDeptEditor(root.querySelector('.depts'), post, reloadDetail);

  /* 댓글 */
  const listEl = root.querySelector('[data-comments]');
  const countEl = root.querySelector('[data-count]');

  function paintComments(items) {
    countEl.textContent = `(${items.length})`;
    listEl.replaceChildren(...(items.length
      ? items.map(c => commentRow(c, deleteComment))
      : [el('p', 'empty', '첫 댓글을 남겨보세요.')]));
  }

  async function deleteComment(cid) {
    try {
      await api(`/api/comments/${cid}`, { method: 'DELETE' });
      paintComments((await api(`/api/posts/${id}`)).comments);
    } catch (e) {
      toast(e.message);
    }
  }

  paintComments(comments);

  const form = root.querySelector('[data-comment-form]');
  const reason = root.querySelector('[data-reason]');
  const textarea = form.querySelector('textarea');
  const submit = form.querySelector('button');

  if (!permission.can_comment) {
    textarea.disabled = true;
    submit.disabled = true;
    reason.textContent = permission.reason;
    reason.classList.add('blocked');
    if (!state.me) {
      const a = el('a', null, ' 로그인하기');
      a.href = '#/login';
      reason.append(a);
    }
  } else {
    reason.textContent = state.me.is_admin
      ? '관리자는 모든 글에 댓글을 쓸 수 있습니다.'
      : `${state.me.department_name} 소속으로 댓글을 씁니다.`;
  }

  form.onsubmit = async ev => {
    ev.preventDefault();
    const body = textarea.value.trim();
    if (!body) return;
    submit.disabled = true;
    try {
      await api(`/api/posts/${id}/comments`, {
        method: 'POST',
        body: JSON.stringify({ body }),
      });
      textarea.value = '';
      paintComments((await api(`/api/posts/${id}`)).comments);
    } catch (e) {
      toast(e.message);
    } finally {
      submit.disabled = false;
    }
  };
}

/* ══════════════════════════════════════════════════ 로그인 / 가입 */

function viewAuth() {
  markNav('login');
  setPageTitle('로그인 · 회원가입');

  const node = tpl('tpl-auth');
  const root = node.firstElementChild;

  const sel = root.querySelector('[name="department_id"]');
  sel.append(new Option('학과를 선택하세요', ''));
  for (const c of state.colleges) {
    const g = document.createElement('optgroup');
    g.label = c.name;
    for (const d of c.departments) g.append(new Option(d.name, d.id));
    sel.append(g);
  }

  root.querySelector('[data-login]').onsubmit = async ev => {
    ev.preventDefault();
    try {
      const out = await api('/api/login', {
        method: 'POST',
        body: JSON.stringify(formValues(ev.target)),
      });
      state.me = out.user;
      paintChrome();
      toast(`${out.user.nickname}님 반갑습니다.`);
      location.hash = '#/posts';
    } catch (e) {
      toast(e.message);
    }
  };

  root.querySelector('[data-register]').onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(ev.target);
    v.department_id = Number(v.department_id);
    try {
      await api('/api/register', { method: 'POST', body: JSON.stringify(v) });
      toast('가입했습니다. 이제 로그인하세요.');
      ev.target.reset();
    } catch (e) {
      toast(e.message);
    }
  };

  render(node);
}

/* ══════════════════════════════════════════════════════ 관리자 */

/* 대학별로 묶은 학과 체크박스. name 접두사로 여러 폼에서 재사용한다. */
function deptChecks(container, prefix) {
  container.replaceChildren(...state.colleges.map(c => {
    const box = el('div', 'dept-college');
    const opts = el('div', 'opts');

    for (const d of c.departments) {
      const label = document.createElement('label');
      const input = document.createElement('input');
      input.type = 'checkbox';
      input.value = d.id;
      input.name = `${prefix}dept`;
      label.append(input, el('span', null, d.name));
      opts.append(label);
    }

    box.append(el('strong', null, c.name), opts);
    return box;
  }));
}

function checkedDepts(container, prefix) {
  return [...container.querySelectorAll(`input[name="${prefix}dept"]:checked`)]
    .map(i => Number(i.value));
}

/* 게시물 상세에 붙는 관리자용 대상 학과 편집기.
 * 체크를 모두 풀고 저장하면 제한이 풀려 누구나 댓글을 쓸 수 있는 글이 된다. */
function mountDeptEditor(container, post, onSaved) {
  const prefix = `p${post.id}-`;
  const details = el('details', 'dept-editor');
  details.append(el('summary', null, '대상 학과 지정 (관리자)'));

  const grid = el('div', 'dept-grid');
  details.append(grid);
  deptChecks(grid, prefix);

  const chosen = new Set(post.departments.map(d => d.id));
  for (const input of grid.querySelectorAll(`input[name="${prefix}dept"]`))
    input.checked = chosen.has(Number(input.value));

  const row = el('div', 'row');
  const save = el('button', 'small', '저장');
  const clear = el('button', 'ghost small', '전체 해제');
  clear.onclick = () => {
    for (const i of grid.querySelectorAll(`input[name="${prefix}dept"]`)) i.checked = false;
  };

  save.onclick = async () => {
    save.disabled = true;
    try {
      await api(`/api/posts/${post.id}/departments`, {
        method: 'PUT',
        body: JSON.stringify({ department_ids: checkedDepts(grid, prefix) }),
      });
      toast('대상 학과를 저장했습니다.');
      onSaved();
    } catch (e) {
      toast(e.message);
      save.disabled = false;
    }
  };

  row.append(save, clear, el('span', 'muted', '체크를 모두 풀면 누구나 댓글을 쓸 수 있습니다.'));
  details.append(row);
  container.append(details);
}

function noticeRow(n, onPublish, onIgnore) {
  const wrap = el('div', 'notice');
  const top = el('div', 'notice-top');

  top.append(el('span', 'tag', n.keyword), el('span', 'muted', n.published_at));
  if (n.status !== 'pending')
    top.append(el('span', n.status === 'published' ? 'tag ok' : 'tag plain',
                  n.status === 'published' ? '게시됨' : '숨김'));

  const h = el('h4');
  const link = el('a', null, n.title);
  link.href = n.url;
  link.target = '_blank';
  link.rel = 'noopener';
  h.append(link);

  wrap.append(top, h);

  if (n.status === 'published') {
    const p = el('p', 'hint');
    const a = el('a', null, '등록한 게시물 보기 →');
    a.href = `#/posts/${n.post_id}`;
    p.append(a);
    wrap.append(p);
    return wrap;
  }
  if (n.status === 'ignored') return wrap;

  const fs = el('fieldset', 'deptbox');
  fs.append(el('legend', null, '대상 학과 체크'));
  const grid = el('div', 'dept-grid');
  fs.append(grid);
  deptChecks(grid, `n${n.id}-`);

  const row = el('div', 'row');
  const pub = el('button', 'small', '게시물로 등록');
  pub.onclick = () => onPublish(n, checkedDepts(grid, `n${n.id}-`), pub);
  const ign = el('button', 'ghost small', '숨기기');
  ign.onclick = () => onIgnore(n);

  row.append(pub, ign);
  wrap.append(fs, row);
  return wrap;
}

async function viewAdmin() {
  markNav('admin');
  setPageTitle('관리자');

  if (!state.me || !state.me.is_admin) {
    renderMessage('관리자만 볼 수 있습니다.');
    return;
  }

  const node = tpl('tpl-admin');
  const root = node.firstElementChild;
  render(node);

  const summaryEl = root.querySelector('[data-summary]');
  async function reloadSummary() {
    try {
      const s = await api('/api/admin/summary');
      summaryEl.replaceChildren(
        tile('c1', s.post_count, '게시물'),
        tile('c3', s.comment_count, '댓글'),
        tile('c4', s.user_count, '학생'),
        tile('c2', s.notice_pending, '대기 공지',
             s.crawl_fetched_at ? `수집 ${s.crawl_fetched_at}` : '아직 수집 전'),
      );
    } catch (e) {
      summaryEl.replaceChildren(el('p', 'empty', e.message));
    }
  }

  const noticesEl = root.querySelector('[data-notices]');
  const showAll = root.querySelector('[data-show-all]');

  async function reloadNotices() {
    noticesEl.replaceChildren(el('p', 'empty', '불러오는 중…'));
    try {
      const { notices } = await api(`/api/notices${showAll.checked ? '' : '?status=pending'}`);
      noticesEl.replaceChildren(...(notices.length
        ? notices.map(n => noticeRow(n, publish, ignore))
        : [el('p', 'empty', '대기 중인 공지가 없습니다.')]));
    } catch (e) {
      noticesEl.replaceChildren(el('p', 'empty', e.message));
    }
  }

  async function publish(n, departmentIds, button) {
    button.disabled = true;
    try {
      await api(`/api/notices/${n.id}/publish`, {
        method: 'POST',
        body: JSON.stringify({ department_ids: departmentIds }),
      });
      toast('게시물로 등록했습니다.');
      await Promise.all([reloadNotices(), reloadSummary()]);
    } catch (e) {
      toast(e.message);
      button.disabled = false;
    }
  }

  async function ignore(n) {
    try {
      await api(`/api/notices/${n.id}/ignore`, { method: 'POST' });
      await Promise.all([reloadNotices(), reloadSummary()]);
    } catch (e) {
      toast(e.message);
    }
  }

  showAll.onchange = reloadNotices;

  const crawlBtn = root.querySelector('[data-crawl]');
  const crawlMsg = root.querySelector('[data-crawl-msg]');
  crawlBtn.onclick = async () => {
    crawlBtn.disabled = true;
    crawlMsg.textContent = '학교 홈페이지에서 가져오는 중…';
    try {
      const r = await api('/api/notices/crawl', { method: 'POST' });
      crawlMsg.textContent =
        `최근 ${r.window_days}일: ${r.fetched}건 확인, 신규 ${r.inserted}건, 게시 ${r.published}건`;
      await Promise.all([reloadNotices(), reloadSummary()]);
    } catch (e) {
      crawlMsg.textContent = e.message;
    } finally {
      crawlBtn.disabled = false;
    }
  };

  const form = root.querySelector('[data-new-post]');
  const checksEl = root.querySelector('[data-dept-checks]');
  deptChecks(checksEl, 'new-');

  form.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(form);
    const btn = form.querySelector('button[type="submit"]');
    btn.disabled = true;
    try {
      await api('/api/posts', {
        method: 'POST',
        body: JSON.stringify({
          title: v.title,
          body: v.body,
          kind: v.kind,
          host: v.host || null,
          source_url: v.source_url || null,
          deadline: v.deadline || null,
          need_people: Number(v.need_people || 0),
          department_ids: checkedDepts(checksEl, 'new-'),
        }),
      });
      toast('게시했습니다.');
      form.reset();
      deptChecks(checksEl, 'new-');
      await reloadSummary();
    } catch (e) {
      toast(e.message);
    } finally {
      btn.disabled = false;
    }
  };

  await Promise.all([reloadSummary(), reloadNotices()]);
}

/* ══════════════════════════════════════════════════════ 라우팅 */

function route() {
  closeSidebar();
  const parts = (location.hash || '#/posts').slice(2).split('/');

  if (parts[0] === 'login') {
    if (state.me) return logout();
    return viewAuth();
  }
  if (parts[0] === 'admin') return viewAdmin();
  if (parts[0] === 'mine') return viewList(true);
  if (parts[0] === 'posts' && parts[1]) return viewDetail(Number(parts[1]));
  return viewList(false);
}

/* ════════════════════════════════════════════ 좁은 화면 사이드바 */

function openSidebar() {
  document.getElementById('sidebar').classList.add('open');
  document.getElementById('scrim').hidden = false;
}

function closeSidebar() {
  document.getElementById('sidebar').classList.remove('open');
  document.getElementById('scrim').hidden = true;
}

/* ══════════════════════════════════════════════════════ 시작 */

async function boot() {
  document.getElementById('hamburger').onclick = openSidebar;
  document.getElementById('scrim').onclick = closeSidebar;

  try {
    const [me, depts] = await Promise.all([
      api('/api/me'),
      api('/api/departments'),
    ]);
    state.me = me.user;
    state.colleges = depts.colleges;
  } catch (e) {
    renderMessage(`서버에 연결할 수 없습니다: ${e.message}`);
    return;
  }

  paintChrome();
  window.addEventListener('hashchange', route);
  route();
}

boot();
