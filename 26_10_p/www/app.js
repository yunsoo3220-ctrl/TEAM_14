/* SKU14 공모전 게시판 - 프런트엔드 (의존성 없음)
 * 서버의 /api/* 를 호출하고 해시 라우팅으로 화면을 바꾼다. */

'use strict';

const state = {
  me: null,
  aiEnabled: false, /* 서버에 ANTHROPIC_API_KEY 가 있을 때만 AI 메뉴·추천을 보인다 */
  colleges: [],     /* [{id, name, departments:[{id,name}]}] */
  tags: [],         /* [{category, label, tags:[{id,name}]}] 관심 키워드 */
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

/* 게시판 표시 이름. 서버의 make_display_name() 과 같은 규칙이라 미리보기에 쓴다.
 * 실제로 저장되는 값은 언제나 서버가 만든 것이다.
 *   손동권 + 010-9948-9687 -> 손*권_9948 */
function displayName(name, phone) {
  const chars = [...(name || '').trim()];
  const digits = (phone || '').replace(/\D/g, '');

  let middle = '';
  if (digits.length === 11) middle = digits.slice(3, 7);
  else if (digits.length === 10) middle = digits.slice(3, 6);
  else return '';

  if (!chars.length) return '';
  if (chars.length === 1) return `${chars[0]}_${middle}`;

  const masked = chars.length === 2
    ? `${chars[0]}*`
    : chars[0] + '*'.repeat(chars.length - 2) + chars[chars.length - 1];

  return `${masked}_${middle}`;
}

function daysLeft(dateStr) {
  const d = new Date(`${dateStr}T23:59:59`);
  if (Number.isNaN(d.getTime())) return null;
  return Math.ceil((d - Date.now()) / 86400000);
}

/* 마감일 배지. 지났으면 '마감', 7일 이내면 강조. */
function ddayTag(deadline, byAi) {
  if (!deadline) return null;
  const left = daysLeft(deadline);
  let t;
  if (left === null || left < 0) t = el('span', 'tag plain', '마감');
  else t = el('span', left <= 7 ? 'tag warn' : 'tag plain', left === 0 ? '오늘 마감' : `D-${left}`);
  if (byAi) t.title = 'AI 가 본문에서 찾은 마감일';
  return t;
}

/* 모델은 영문을 소문자로 다룬다. 짧은 약어(ai, sns)는 대문자로 보여준다. */
const fmtTerm = t => (/^[a-z0-9]{2,4}$/.test(t) ? t.toUpperCase() : t);

function tagList(tags, cls = 'tag-list') {
  const box = el('div', cls);
  for (const t of tags || []) box.append(el('span', 'ttag', `#${fmtTerm(t)}`));
  return box;
}

/* 관련도 점수를 원형 게이지로 보여준다. */
function scoreRing(score) {
  const ring = el('span', 'score-ring');
  ring.style.setProperty('--p', Math.max(0, Math.min(100, score)));
  ring.append(el('b', null, String(score)));
  ring.title = `내 학과 관련도 ${score}점`;
  return ring;
}

function skeleton(n, cls = 'sk-item') {
  return Array.from({ length: n }, () => {
    const s = el('div', `skeleton ${cls}`);
    s.append(el('i'), el('i'), el('i'));
    return s;
  });
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
  document.getElementById('nav-me').hidden = !me;
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
  top.append(el('span', `tag kind-${p.kind}`, KIND_LABEL[p.kind] || p.kind));
  const dd = ddayTag(p.deadline, p.deadline_by_ai);
  if (dd) top.append(dd);
  if (p.can_comment) top.append(el('span', 'tag ok', '참여 가능'));

  a.append(top, el('h3', null, p.title));
  if (p.summary) a.append(el('p', 'item-summary', p.summary));
  else if (p.excerpt) a.append(el('p', 'item-summary excerpt', p.excerpt));
  if (p.tags && p.tags.length) a.append(tagList(p.tags.slice(0, 4)));

  const meta = el('div', 'item-meta');
  const bits = [];
  if (p.host) bits.push(p.host);
  if (p.deadline) bits.push(`마감 ${p.deadline}`);
  if (p.need_people > 0) bits.push(`${p.need_people}명 모집`);
  bits.push(`댓글 ${p.comment_count}`);
  bits.push(p.created_at.slice(0, 10));
  for (const b of bits) meta.append(el('span', null, b));
  a.append(meta);

  a.append(el('div', 'item-depts', p.departments
    ? `대상 학과 · ${p.departments}`
    : '대상 학과 미지정 · 로그인하면 누구나 댓글 가능'));

  return a;
}

/* ══════════════════════════════════════════════════ AI 추천 */

function recoCard(r) {
  const a = el('a', 'reco-card');
  a.href = `#/posts/${r.id}`;

  const head = el('div', 'reco-card-head');
  const tags = el('div', 'item-top');
  tags.append(el('span', `tag kind-${r.kind}`, KIND_LABEL[r.kind] || r.kind));
  const dd = ddayTag(r.deadline);
  if (dd) tags.append(dd);
  head.append(tags, scoreRing(r.score));

  const why = el('p', 'reco-why');
  why.append(el('span', 'spark', '✦'), r.reason);

  a.append(head, el('h3', null, r.title), why);
  if (r.tags && r.tags.length) a.append(tagList(r.tags.slice(0, 3)));
  return a;
}

/* 자체 모델 결과 카드: 관련도 + 겹친 키워드 */
function mlCard(r) {
  const a = el('a', 'reco-card');
  a.href = `#/posts/${r.id}`;

  const head = el('div', 'reco-card-head');
  const tags = el('div', 'item-top');
  tags.append(el('span', `tag kind-${r.kind}`, KIND_LABEL[r.kind] || r.kind));
  const dd = ddayTag(r.deadline);
  if (dd) tags.append(dd);
  head.append(tags, scoreRing(r.score));

  a.append(head, el('h3', null, r.title));
  if (r.terms && r.terms.length) {
    const why = el('div', 'reco-terms');
    why.append(el('span', 'spark', '✦'), el('span', 'muted', '겹친 키워드'));
    for (const t of r.terms) why.append(el('span', 'ttag hit', fmtTerm(t)));
    a.append(why);
  }
  return a;
}

/* 목록 위에 붙는 추천 영역. Claude 분석이 켜져 있으면 그 결과를, 아니면 자체 모델을 쓴다. */
async function mountReco(box) {
  const me = state.me;
  box.hidden = false;

  if (!me) {
    box.className = 'reco reco-cta';
    const t = el('div');
    t.append(el('h2', null, '학과를 고르면 관련 공모전을 정리해 드립니다'),
             el('p', null, '자체 AI 모델이 게시물 내용을 분석해 학과별 관련도를 매깁니다. 로그인 없이도 볼 수 있습니다.'));
    const go = el('a', 'btn', '학과별 추천 보기');
    go.href = '#/reco';
    box.replaceChildren(el('span', 'ai-badge lg', 'AI'), t, go);
    return;
  }
  if (me.is_admin) { box.hidden = true; return; }

  if (!state.aiEnabled) return mountLocalReco(box, me);

  const head = el('div', 'reco-head');
  const titleBox = el('div');
  titleBox.append(el('h2', null, `${me.department_name} 맞춤 추천`),
                  el('p', 'muted', 'AI 가 본문을 읽고 내 전공과 관련도가 높다고 판단한 공모전'));
  const more = el('a', 'more', '전체 보기 →');
  more.href = '#/reco';
  head.append(el('span', 'ai-badge', 'AI'), titleBox, more);

  const grid = el('div', 'reco-grid');
  grid.append(...skeleton(3, 'sk-reco'));
  box.replaceChildren(head, grid);

  try {
    const r = await api('/api/recommendations?limit=3');
    if (r.posts.length) {
      grid.replaceChildren(...r.posts.map(recoCard));
    } else {
      grid.replaceChildren(el('p', 'empty small', r.analyzed_count
        ? '아직 내 학과와 관련도가 높은 공모전이 없습니다.'
        : 'AI 분석이 아직 끝나지 않았습니다. 잠시 뒤 다시 확인해 주세요.'));
    }
  } catch {
    box.hidden = true;
  }
}

async function mountLocalReco(box, me) {
  const head = el('div', 'reco-head');
  const titleBox = el('div');
  titleBox.append(el('h2', null, `${me.department_name} 관련 공모전`),
                  el('p', 'muted', '자체 AI 모델이 게시물 내용을 내 전공·관심 키워드와 비교했습니다'));
  const more = el('a', 'more', '전체 보기 →');
  more.href = `#/reco/${me.department_id}`;
  head.append(el('span', 'ai-badge', 'AI'), titleBox, more);

  const grid = el('div', 'reco-grid');
  grid.append(...skeleton(3, 'sk-reco'));
  box.replaceChildren(head, grid);

  try {
    const r = await api(`/api/related?department_id=${me.department_id}&limit=3`);
    grid.replaceChildren(...(r.posts.length
      ? r.posts.map(mlCard)
      : [el('p', 'empty small', '아직 내 학과와 관련 있는 공모전이 없습니다.')]));
  } catch {
    box.hidden = true;
  }
}

/* 관련도 구간. 같은 구간끼리 묶어 보여준다. */
const TIERS = [
  { min: 70, label: '매우 관련', note: '전공 분야와 직접 맞닿는 공모전' },
  { min: 50, label: '관련',      note: '전공 지식을 살릴 수 있는 공모전' },
  { min: 0,  label: '참고',      note: '일부 주제가 겹치는 공모전' },
];

/* #/reco/{학과번호} - 학과를 골라 자체 모델의 추천을 구간별로 본다. */
async function viewReco(deptParam) {
  markNav('reco');
  setPageTitle('학과별 추천');

  const node = tpl('tpl-reco');
  const root = node.firstElementChild;
  const sel = root.querySelector('[data-reco-dept]');
  const groups = root.querySelector('[data-reco-groups]');
  const modelLine = root.querySelector('[data-model-line]');

  for (const c of state.colleges) {
    const g = document.createElement('optgroup');
    g.label = c.name;
    for (const d of c.departments) g.append(new Option(d.name, d.id));
    sel.append(g);
  }

  const mine = state.me && state.me.department_id;
  const dept = Number(deptParam) || mine || (state.colleges[0] && state.colleges[0].departments[0].id);
  sel.value = String(dept);
  sel.onchange = () => { location.hash = `#/reco/${sel.value}`; };

  render(node);
  groups.replaceChildren(...skeleton(3, 'sk-reco'));

  try {
    const r = await api(`/api/related?department_id=${dept}&limit=60`);
    const m = r.model;
    modelLine.textContent =
      `${m.embedding_dim ? '딥러닝 임베딩 + TF-IDF' : 'TF-IDF'} · ` +
      `게시물 ${m.posts}건 · 학과 ${m.departments}개 · 학습 데이터 ${m.labels}건 · ${m.trained_at} 학습`;
    root.querySelector('[data-reco-title]').textContent =
      `${r.department.name}${dept === mine ? ' (내 학과)' : ''} 관련 공모전`;
    if (r.personal)
      root.querySelector('[data-reco-sub]').textContent =
        '내 학과에 내 관심 키워드·자기소개까지 더해 계산한 개인 맞춤 추천입니다.';

    if (!r.posts.length) {
      groups.replaceChildren(el('p', 'empty', '이 학과와 관련 있다고 판단한 공모전이 아직 없습니다.'));
      return;
    }

    groups.replaceChildren(...TIERS.map((t, i) => {
      const upper = i ? TIERS[i - 1].min : 101;
      const items = r.posts.filter(p => p.score >= t.min && p.score < upper);
      if (!items.length) return null;
      const sec = el('section', 'tier');
      const h = el('div', 'tier-head');
      h.append(el('h3', null, t.label), el('span', 'tier-count', String(items.length)),
               el('span', 'muted small', t.note));
      const grid = el('div', 'reco-grid');
      grid.append(...items.map(mlCard));
      sec.append(h, grid);
      return sec;
    }).filter(Boolean));
  } catch (e) {
    groups.replaceChildren(el('p', 'empty', e.message));
  }
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

    listEl.replaceChildren(...skeleton(4));
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
  if (!forceMine) mountReco(root.querySelector('[data-reco]'));

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

  const ai = post.ai;
  const kindEl = root.querySelector('[data-kind]');
  kindEl.textContent = KIND_LABEL[post.kind] || post.kind;
  kindEl.classList.add(`kind-${post.kind}`);
  root.querySelector('[data-title]').textContent = post.title;

  /* 주최·마감이 비어 있으면 AI 가 본문에서 찾은 값을 표시하고 출처를 밝힌다. */
  const fillMeta = (sel, own, guess) => {
    const dd = root.querySelector(sel);
    if (own) { dd.textContent = own; return; }
    if (guess) {
      dd.append(guess, ' ');
      const b = el('span', 'ai-mini', 'AI');
      b.title = 'AI 가 본문에서 찾은 값';
      dd.append(b);
      return;
    }
    dd.textContent = '-';
  };
  fillMeta('[data-host]', post.host, ai && ai.host);
  fillMeta('[data-deadline]', post.deadline, ai && ai.deadline);
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

  if (ai) paintAiCard(root.querySelector('[data-ai]'), ai);
  paintMlCard(root.querySelector('[data-ml]'), post.related_departments || []);

  /* 이 공모전으로 올라온 팀원 모집 글 */
  {
    const recs = post.recruits || [];
    const box = root.querySelector('[data-recruits]');
    root.querySelector('[data-recruit-count]').textContent = `(${recs.length})`;
    root.querySelector('[data-recruit-new]').href = state.me ? `#/recruits/new/${post.id}` : '#/login';
    box.replaceChildren(...(recs.length ? recs.map(r => {
      const a = el('a', 'mini-row');
      a.href = `#/recruits/${r.id}`;
      a.append(el('span', r.status === 'open' ? 'tag ok' : 'tag plain', r.status === 'open' ? `모집 중 · ${r.need_people}명` : '마감'),
               el('strong', null, r.title),
               el('span', 'muted small', `${r.author_nickname}${r.author_department ? ' · ' + r.author_department : ''} · 댓글 ${r.comment_count}`));
      return a;
    }) : [el('p', 'muted small', '아직 이 공모전으로 팀원을 찾는 글이 없습니다.')]));
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

/* 자체 모델이 고른 관련 학과. 학과를 누르면 그 학과의 추천 목록으로 간다. */
function paintMlCard(card, depts) {
  if (!depts.length) return;
  card.hidden = false;
  const myDept = state.me && state.me.department_id;

  card.querySelector('[data-ml-depts]').replaceChildren(...depts.map(d => {
    const a = el('a', 'ml-dept' + (d.id === myDept ? ' me' : ''));
    a.href = `#/reco/${d.id}`;
    const head = el('div', 'ai-dept-head');
    const name = el('strong', null, d.name);
    if (d.id === myDept) name.append(el('span', 'tag ok', '내 학과'));
    head.append(name, el('b', 'ai-score', String(d.score)));
    const bar = el('div', 'bar');
    const fill = el('i');
    fill.style.width = `${d.score}%`;
    bar.append(fill);
    a.append(head, bar);
    if (typeof d.prob === 'number') a.append(el('span', 'muted small', `적합 확률 ${fmtProb(d.prob)}`));
    if (d.terms.length) a.append(tagList(d.terms, 'tag-list tight'));
    return a;
  }));
}

function paintAiCard(card, ai) {
  card.hidden = false;
  card.querySelector('[data-ai-meta]').textContent = `${ai.analyzed_at} 분석`;
  card.querySelector('[data-ai-summary]').textContent = ai.summary;
  card.querySelector('[data-ai-tags]').replaceWith(tagList(ai.tags));

  const box = card.querySelector('[data-ai-depts]');
  if (!ai.departments.length) {
    box.replaceChildren(el('p', 'muted small', '특별히 관련도가 높은 학과가 없습니다.'));
    return;
  }

  const myDept = state.me && state.me.department_id;
  box.replaceChildren(...ai.departments.map(d => {
    const row = el('div', 'ai-dept' + (d.id === myDept ? ' me' : ''));
    const head = el('div', 'ai-dept-head');
    const name = el('strong', null, d.name);
    if (d.id === myDept) name.append(el('span', 'tag ok', '내 학과'));
    head.append(name, el('span', 'muted small', d.college_name), el('b', 'ai-score', `${d.score}`));

    const bar = el('div', 'bar');
    const fill = el('i');
    fill.style.width = `${d.score}%`;
    bar.append(fill);

    row.append(head, bar, el('p', 'ai-reason', d.reason));
    return row;
  }));
}

/* ══════════════════════════════════════════════════ 로그인 / 가입 */

const SCHOOL_EMAIL = /^[a-z0-9._-]+@skuniv\.ac\.kr$/;

function viewAuth(register = false) {
  markNav('login');
  setPageTitle(register ? '회원가입' : '로그인');

  const node = tpl('tpl-auth');
  const root = node.firstElementChild;
  root.querySelector('[data-login-card]').hidden = register;
  root.querySelector('[data-register-card]').hidden = !register;

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

  /* 표시 이름 미리보기. 서버가 만드는 값과 같은 규칙을 쓴다. */
  const reg = root.querySelector('[data-register]');
  const preview = reg.querySelector('[data-preview]');
  const refreshPreview = () => {
    const v = formValues(reg);
    preview.textContent = displayName(v.name, v.phone) || '···';
  };
  reg.querySelector('[name="name"]').oninput = refreshPreview;
  reg.querySelector('[name="phone"]').oninput = refreshPreview;

  /* 관심 키워드 (선택, 0~8개) - 고르면 팀원 추천과 개인 맞춤 공모전 추천에 쓰인다 */
  const getTags = interestPicker(reg.querySelector('[data-interest-picker]'), [],
                                 { countEl: reg.querySelector('[data-pick-count]') });

  /* 학교 이메일 인증 */
  const emailEl = reg.querySelector('[name="email"]');
  const codeEl = reg.querySelector('[name="code"]');
  const codeRow = reg.querySelector('[data-code-row]');
  const sendBtn = reg.querySelector('[data-send-code]');
  const timerEl = reg.querySelector('[data-timer]');
  const codeMsg = reg.querySelector('[data-code-msg]');
  const verifyBtn = reg.querySelector('[data-verify-code]');
  let sentTo = '', tick = null, verified = false;

  const say = (text, kind = 'muted') => { codeMsg.textContent = text; codeMsg.className = `auth-msg small ${kind}`; };

  function countdown(expiresIn, resendIn) {
    const t0 = Date.now();
    clearInterval(tick);
    const step = () => {
      if (!document.body.contains(timerEl)) return clearInterval(tick);   /* 다른 화면으로 나감 */
      const passed = (Date.now() - t0) / 1000;
      const left = Math.max(0, Math.ceil(expiresIn - passed));
      const again = Math.max(0, Math.ceil(resendIn - passed));
      timerEl.textContent = left ? `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}` : '만료';
      timerEl.classList.toggle('over', !left);
      sendBtn.disabled = again > 0;
      sendBtn.textContent = again > 0 ? `다시 받기 (${again})` : '다시 받기';
      if (!left && !again) clearInterval(tick);
    };
    step();
    tick = setInterval(step, 1000);
  }

  sendBtn.onclick = async () => {
    const email = emailEl.value.trim().toLowerCase();
    emailEl.value = email;
    if (!SCHOOL_EMAIL.test(email)) {
      say('@skuniv.ac.kr 로 끝나는 학교 이메일을 입력하세요.', 'blocked');
      return emailEl.focus();
    }
    sendBtn.disabled = true;
    say('인증 코드를 보내는 중…');
    try {
      const r = await api('/api/register/send-code', { method: 'POST', body: JSON.stringify({ email }) });
      sentTo = email;
      codeRow.hidden = false;
      codeEl.value = '';
      verifyBtn.disabled = false;
      codeEl.focus();
      say(r.dev_mode
        ? '개발 모드: 메일 서버가 설정되지 않아 코드를 서버 로그(logs\\server.err.log)에 남겼습니다.'
        : `${email} 로 6자리 코드를 보냈습니다. 메일함(스팸함 포함)을 확인하세요.`, r.dev_mode ? 'warn' : 'ok');
      countdown(r.expires_in, r.resend_in);
    } catch (e) {
      say(e.message, 'blocked');
      sendBtn.disabled = false;
    }
  };
  emailEl.oninput = () => {                         /* 주소를 바꾸면 다시 받아야 한다 */
    if (sentTo && emailEl.value.trim().toLowerCase() !== sentTo) {
      sentTo = '';
      codeRow.hidden = true;
      clearInterval(tick);
      sendBtn.disabled = false;
      sendBtn.textContent = '인증 코드 받기';
      say('주소가 바뀌었습니다. 인증 코드를 다시 받으세요.');
    }
  };
  codeEl.oninput = () => { codeEl.value = codeEl.value.replace(/\D/g, '').slice(0, 6); };

  /* [확인] - 서버에서 코드를 맞춰 보고, 맞으면 이메일을 잠그고 인증 완료로 표시한다. */
  async function verifyCode() {
    if (verified) return;
    const code = codeEl.value.trim();
    if (!/^\d{6}$/.test(code)) {
      say('메일로 받은 6자리 숫자를 입력하세요.', 'blocked');
      return codeEl.focus();
    }
    verifyBtn.disabled = true;
    try {
      await api('/api/register/verify-code', { method: 'POST', body: JSON.stringify({ email: sentTo, code }) });
      verified = true;
      clearInterval(tick);
      emailEl.readOnly = codeEl.readOnly = true;
      sendBtn.disabled = true;
      sendBtn.textContent = '인증됨';
      verifyBtn.textContent = '완료';
      timerEl.textContent = '✓';
      timerEl.classList.remove('over');
      timerEl.classList.add('done');
      reg.querySelector('[data-email-box]').classList.add('verified');
      say('이메일 인증이 완료되었습니다. 아래 정보를 입력하고 가입하세요.', 'ok');
      reg.querySelector('[name="student_no"]').focus();
    } catch (e) {
      say(e.message, 'blocked');
      verifyBtn.disabled = false;
      codeEl.select();
    }
  }
  verifyBtn.onclick = verifyCode;
  codeEl.onkeydown = ev => {                        /* 코드 칸에서 Enter = [확인] (가입 제출 아님) */
    if (ev.key === 'Enter') { ev.preventDefault(); verifyCode(); }
  };

  reg.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(ev.target);
    v.email = (v.email || '').trim().toLowerCase();
    if (!SCHOOL_EMAIL.test(v.email)) return toast('학교 이메일(@skuniv.ac.kr)을 입력하세요.');
    if (!sentTo) return toast('먼저 인증 코드 받기를 누르세요.');
    if (!verified) return toast('인증 코드를 입력하고 [확인]을 눌러 이메일 인증을 마치세요.');
    delete v.code;
    for (const [k, label] of [['student_no', '학번'], ['name', '이름'], ['phone', '전화번호'],
                              ['password', '비밀번호'], ['department_id', '소속 학과']])
      if (!v[k]) return toast(`${label}을(를) 입력하세요.`);
    v.department_id = Number(v.department_id);
    v.interest_ids = getTags();
    const btn = reg.querySelector('button[type="submit"]');
    btn.disabled = true;
    try {
      const out = await api('/api/register', { method: 'POST', body: JSON.stringify(v) });
      clearInterval(tick);
      toast(`가입했습니다. 표시 이름은 ${out.nickname} 입니다. 로그인하세요.`);
      location.hash = '#/login';
      setTimeout(() => {
        const id = document.querySelector('[data-login] [name="student_no"]');
        if (id) { id.value = v.student_no; document.querySelector('[data-login] [name="password"]').focus(); }
      }, 0);
    } catch (e) {
      toast(e.message);
      btn.disabled = false;
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

const fmtProb = p => (p >= 0.995 ? '99%+' : p < 0.005 ? '<1%' : `${Math.round(p * 100)}%`);

/* 관리자 글쓰기: 입력하는 동안 학과별 적합 확률을 실시간으로 보여 준다.
 * 서버는 재학습 없이 지금 모델로 계산한다 (POST /api/ml/predict). */
function mountPredict(box, form, checksEl, prefix) {
  const list = box.querySelector('[data-predict-list]');
  const meta = box.querySelector('[data-predict-meta]');
  const apply = box.querySelector('[data-predict-apply]');
  const titleEl = form.querySelector('[name="title"]');
  const bodyEl = form.querySelector('[name="body"]');
  const intro = meta.textContent;
  let timer = null, seq = 0, last = [];

  const checkbox = id => checksEl.querySelector(`input[name="${prefix}dept"][value="${id}"]`);

  function paint(depts) {
    list.replaceChildren(...depts.slice(0, 8).map(d => {
      const row = el('button', 'predict-row');
      row.type = 'button';
      const cb = checkbox(d.id);
      row.classList.toggle('on', !!(cb && cb.checked));
      row.title = '눌러서 대상 학과로 체크/해제';
      const head = el('div', 'predict-row-head');
      head.append(el('strong', null, d.name), el('span', 'muted small', d.college_name),
                  el('b', 'predict-prob', fmtProb(d.prob)));
      const bar = el('div', 'bar');
      const fill = el('i');
      fill.style.width = `${Math.max(1, d.prob * 100)}%`;
      bar.append(fill);
      row.append(head, bar);
      if (d.terms.length) row.append(el('span', 'predict-terms', d.terms.map(fmtTerm).join(' · ')));
      row.onclick = () => {
        if (!cb) return;
        cb.checked = !cb.checked;
        row.classList.toggle('on', cb.checked);
      };
      return row;
    }));
  }

  async function run() {
    const title = titleEl.value.trim(), body = bodyEl.value.trim();
    if ((title + body).length < 6) return clear();
    const my = ++seq, t0 = performance.now();
    box.classList.add('busy');
    try {
      const r = await api('/api/ml/predict', { method: 'POST', body: JSON.stringify({ title, body }) });
      if (my !== seq) return;                     // 더 최근 입력의 결과만 그린다
      last = r.departments;
      paint(last);
      const c = r.model.calibration;
      meta.textContent =
        `${r.model.embedding_dim ? '딥러닝 임베딩 + TF-IDF' : 'TF-IDF'} · 게시물 ${r.model.posts}건으로 학습 · ` +
        `정답 ${c.positives}쌍으로 확률 보정 · ${Math.round(performance.now() - t0)}ms`;
      apply.hidden = !last.some(d => d.prob >= 0.3);
    } catch (e) {
      if (my === seq) meta.textContent = e.message;
    } finally {
      if (my === seq) box.classList.remove('busy');
    }
  }

  function clear() {
    seq++;
    last = [];
    list.replaceChildren();
    meta.textContent = intro;
    apply.hidden = true;
    box.classList.remove('busy');
  }

  const schedule = () => { clearTimeout(timer); timer = setTimeout(run, 450); };
  titleEl.addEventListener('input', schedule);
  bodyEl.addEventListener('input', schedule);
  checksEl.addEventListener('change', () => last.length && paint(last));
  apply.onclick = () => {
    for (const d of last) {
      const cb = checkbox(d.id);
      if (cb && d.prob >= 0.3) cb.checked = true;
    }
    paint(last);
  };
  return { clear };
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
  noticesEl.replaceChildren(...skeleton(2));

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
      await Promise.all([reloadNotices(), reloadSummary(), reloadAi()]);
    } catch (e) {
      crawlMsg.textContent = e.message;
    } finally {
      crawlBtn.disabled = false;
    }
  };

  /* AI 분석 상태. 작업이 도는 동안 2초마다 다시 읽는다. */
  const aiBox = root.querySelector('[data-ai-status]');
  const aiRun = root.querySelector('[data-ai-run]');
  const aiForce = root.querySelector('[data-ai-force]');
  let aiTimer = null;

  function paintAiStatus(s) {
    aiBox.replaceChildren();
    if (!s.enabled) {
      aiBox.append(el('p', 'reason blocked',
        'ANTHROPIC_API_KEY 가 설정되지 않아 AI 기능이 꺼져 있습니다. 서버를 키와 함께 다시 시작하세요.'));
      aiRun.disabled = aiForce.disabled = true;
      return;
    }
    const pct = s.post_count ? Math.round((s.analyzed_count / s.post_count) * 100) : 0;
    const head = el('div', 'row spread');
    head.append(el('span', null, `분석 완료 ${s.analyzed_count} / ${s.post_count}건`),
                el('span', 'muted small', s.model));
    const bar = el('div', 'bar lg');
    const fill = el('i');
    fill.style.width = `${pct}%`;
    bar.append(fill);
    aiBox.append(head, bar);

    let line;
    if (s.running) line = `분석 중… 이번 작업 ${s.done + s.failed} / ${s.total}건`;
    else if (s.finished_at) line = `마지막 작업 ${s.finished_at} · 성공 ${s.done}, 실패 ${s.failed}`;
    else line = '서버가 켜진 뒤 아직 분석 작업을 하지 않았습니다.';
    aiBox.append(el('p', 'muted small', line));
    if (s.last_error) aiBox.append(el('p', 'reason blocked', `최근 오류: ${s.last_error}`));

    aiRun.disabled = aiForce.disabled = s.running;
    clearTimeout(aiTimer);
    if (s.running) aiTimer = setTimeout(reloadAi, 2000);
  }

  async function reloadAi() {
    if (!document.body.contains(aiBox)) return;   /* 다른 화면으로 나갔으면 멈춘다 */
    try { paintAiStatus(await api('/api/ai/status')); }
    catch (e) { aiBox.replaceChildren(el('p', 'empty', e.message)); }
  }

  async function startAi(force) {
    try {
      const r = await api('/api/ai/analyze', { method: 'POST', body: JSON.stringify({ force }) });
      toast(r.started ? 'AI 분석을 시작했습니다.' : '이미 분석이 진행 중입니다.');
      paintAiStatus(r.status);
    } catch (e) {
      toast(e.message);
    }
  }
  aiRun.onclick = () => startAi(false);
  aiForce.onclick = () => {
    if (confirm('모든 게시물을 다시 분석합니다. API 사용량이 발생합니다. 계속할까요?')) startAi(true);
  };

  const form = root.querySelector('[data-new-post]');
  const checksEl = root.querySelector('[data-dept-checks]');
  deptChecks(checksEl, 'new-');
  const predict = mountPredict(root.querySelector('[data-predict]'), form, checksEl, 'new-');

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
      toast('게시했습니다. 추천 모델이 이 글까지 포함해 다시 학습합니다.');
      form.reset();
      deptChecks(checksEl, 'new-');
      predict.clear();
      await Promise.all([reloadSummary(), reloadAi()]);
    } catch (e) {
      toast(e.message);
    } finally {
      btn.disabled = false;
    }
  };

  await Promise.all([reloadSummary(), reloadNotices(), reloadAi()]);
}

/* ══════════════════════════════════════════ 관심 키워드 선택기 */

/* 분야·역할·성향으로 묶은 키워드 칩. 고른 번호 배열을 돌려주는 함수를 돌려준다. */
function interestPicker(container, selected = [], { max = 8, countEl = null, min = 0 } = {}) {
  const chosen = new Set(selected.map(Number));

  const refresh = () => {
    for (const b of container.querySelectorAll('.pick'))
      b.classList.toggle('on', chosen.has(Number(b.dataset.id)));
    if (countEl) {
      countEl.textContent = `${chosen.size} / ${max}개 선택${min && chosen.size < min ? ` · ${min}개 이상` : ''}`;
      countEl.classList.toggle('warn', chosen.size >= max);
    }
  };

  container.replaceChildren(...state.tags.map(g => {
    const box = el('div', 'pick-group');
    box.append(el('strong', null, g.label));
    const row = el('div', 'picks');
    for (const t of g.tags) {
      const b = el('button', 'pick', t.name);
      b.type = 'button';
      b.dataset.id = t.id;
      b.onclick = () => {
        const id = Number(t.id);
        if (chosen.has(id)) chosen.delete(id);
        else if (chosen.size >= max) { toast(`키워드는 ${max}개까지 고를 수 있습니다.`); return; }
        else chosen.add(id);
        refresh();
      };
      row.append(b);
    }
    box.append(row);
    return box;
  }));
  refresh();
  return () => [...chosen];
}

function requireLogin(message) {
  const box = el('div', 'empty');
  box.append(message, el('br'));
  const a = el('a', 'btn small', '로그인 · 가입');
  a.href = '#/login';
  box.append(a);
  render(box);
}

/* ══════════════════════════════════════════════ 팀원 모집 게시판 */

/* 마음에 들어요 버튼. 누른 기록은 AI 추천(관련 공모전·팀원 찾기)에도 반영된다. */
function likeButton(r) {
  const b = el('button', 'like-btn');
  b.type = 'button';
  const paint = () => {
    b.classList.toggle('on', !!r.liked);
    b.replaceChildren(el('span', 'heart', r.liked ? '♥' : '♡'), el('span', null, String(r.like_count)));
    b.title = r.liked ? '마음에 들어요 취소' : '마음에 들어요 · AI 추천에 반영됩니다';
    b.setAttribute('aria-pressed', r.liked ? 'true' : 'false');
  };
  const mine = state.me && state.me.id === r.author_id;
  if (mine) {
    b.disabled = true;
    b.title = '내 모집글';
  }
  b.onclick = async ev => {
    ev.preventDefault();            // 카드(링크) 안에 있어도 이동하지 않게
    ev.stopPropagation();
    if (!state.me) return toast('로그인하면 마음에 들어요를 누를 수 있습니다.');
    b.disabled = true;
    try {
      const out = await api(`/api/recruits/${r.id}/like`, { method: r.liked ? 'DELETE' : 'POST' });
      r.liked = out.liked;
      r.like_count = out.like_count;
      paint();
      if (r.liked) toast('마음에 들어요! 비슷한 공모전·팀원 추천에 반영됩니다.');
    } catch (e) { toast(e.message); }
    b.disabled = false;
  };
  paint();
  return b;
}

function recruitCard(r) {
  const a = el('a', 'recruit-card' + (r.status === 'closed' ? ' closed' : ''));
  a.href = `#/recruits/${r.id}`;

  const top = el('div', 'reco-card-head');
  const tags = el('div', 'item-top');
  tags.append(el('span', r.status === 'open' ? 'tag ok' : 'tag plain',
                 r.status === 'open' ? `모집 중 · ${r.need_people}명` : '모집 마감'));
  const dd = r.status === 'open' ? ddayTag(r.deadline) : null;
  if (dd) tags.append(dd);
  top.append(tags);
  if (typeof r.match === 'number' && r.match > 0) top.append(scoreRing(r.match));

  a.append(top, el('h3', null, r.title));
  if (r.post_title) a.append(el('p', 'recruit-post', `🏆 ${r.post_title}`));
  a.append(el('p', 'item-summary excerpt', r.excerpt));
  if (r.tags.length) {
    const want = el('div', 'tag-list');
    for (const t of r.tags) want.append(el('span', 'ttag want', t));
    a.append(want);
  }
  const meta = el('div', 'item-meta');
  meta.append(el('span', null, r.author_nickname + (r.author_department ? ` · ${r.author_department}` : '')),
              el('span', null, `댓글 ${r.comment_count}`),
              el('span', null, r.created_at.slice(0, 10)));
  const foot = el('div', 'recruit-foot');
  foot.append(meta, likeButton(r));
  a.append(foot);
  return a;
}

async function viewRecruits() {
  markNav('recruits');
  setPageTitle('팀원 모집');

  const node = tpl('tpl-recruits');
  const root = node.firstElementChild;
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const f = {
    tag: root.querySelector('[data-f="tag"]'),
    status: root.querySelector('[data-f="status"]'),
    sort: root.querySelector('[data-f="sort"]'),
  };
  for (const g of state.tags) {
    const og = document.createElement('optgroup');
    og.label = g.label;
    for (const t of g.tags) og.append(new Option(t.name, t.id));
    f.tag.append(og);
  }
  const canMatch = state.me && !state.me.is_admin;
  if (!canMatch) f.sort.querySelector('[value="match"]').disabled = true;
  else f.sort.value = 'match';
  if (!state.me) root.querySelector('[data-new]').href = '#/login';

  async function reload() {
    const qs = new URLSearchParams({ status: f.status.value, sort: f.sort.value });
    if (f.tag.value) qs.set('tag', f.tag.value);
    listEl.replaceChildren(...skeleton(4, 'sk-reco'));
    try {
      const r = await api(`/api/recruits?${qs}`);
      countEl.textContent = `(${r.recruits.length})`;
      listEl.replaceChildren(...(r.recruits.length
        ? r.recruits.map(recruitCard)
        : [el('p', 'empty', '조건에 맞는 모집글이 없습니다. 첫 모집글을 써 보세요.')]));
    } catch (e) {
      listEl.replaceChildren(el('p', 'empty', e.message));
    }
  }
  for (const x of Object.values(f)) x.onchange = reload;
  render(node);
  reload();
}

async function viewRecruitNew(postId) {
  markNav('recruits');
  setPageTitle('모집글 쓰기');
  if (!state.me) return requireLogin('모집글을 쓰려면 로그인하세요.');

  const node = tpl('tpl-recruit-new');
  const root = node.firstElementChild;
  const form = root.querySelector('[data-form]');
  const postSel = form.querySelector('[name="post_id"]');
  const getTags = interestPicker(root.querySelector('[data-tag-picker]'), [],
                                 { countEl: root.querySelector('[data-pick-count]') });
  render(node);

  try {
    const { posts } = await api('/api/posts');
    for (const p of posts) postSel.append(new Option(p.title, p.id));
    if (postId) postSel.value = String(postId);
  } catch { /* 공모전 목록 없이도 쓸 수 있다 */ }

  form.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(form);
    const btn = form.querySelector('button[type="submit"]');
    btn.disabled = true;
    try {
      const out = await api('/api/recruits', {
        method: 'POST',
        body: JSON.stringify({
          title: v.title, body: v.body, post_id: Number(v.post_id) || 0,
          need_people: Number(v.need_people || 1), deadline: v.deadline || null,
          tag_ids: getTags(),
        }),
      });
      toast('모집글을 올렸습니다.');
      location.hash = `#/recruits/${out.id}`;
    } catch (e) {
      toast(e.message);
      btn.disabled = false;
    }
  };
}

async function viewRecruit(id) {
  markNav('recruits');
  setPageTitle('모집글');

  let data;
  try { data = await api(`/api/recruits/${id}`); }
  catch (e) { return renderMessage(e.message); }

  const { recruit: r, body, comments, can_edit } = data;
  const node = tpl('tpl-recruit');
  const root = node.firstElementChild;

  const top = root.querySelector('[data-tags-top]');
  top.append(el('span', r.status === 'open' ? 'tag ok' : 'tag plain', r.status === 'open' ? '모집 중' : '모집 마감'));
  const dd = r.status === 'open' ? ddayTag(r.deadline) : null;
  if (dd) top.append(dd);
  root.querySelector('[data-title]').textContent = r.title;
  root.querySelector('[data-author]').textContent =
    r.author_nickname + (r.author_department ? ` · ${r.author_department}` : '');
  root.querySelector('[data-need]').textContent = `${r.need_people}명`;
  root.querySelector('[data-deadline]').textContent = r.deadline || '정하지 않음';
  root.querySelector('[data-created]').textContent = r.created_at;
  root.querySelector('[data-body]').textContent = body;

  const postP = root.querySelector('[data-post]');
  if (r.post_id) {
    const a = el('a', null, `🏆 ${r.post_title}`);
    a.href = `#/posts/${r.post_id}`;
    postP.append(el('span', 'muted', '함께 나갈 공모전 '), a);
  } else postP.remove();

  const want = root.querySelector('[data-want]');
  for (const t of r.tags) want.append(el('span', 'ttag want', t));
  const likeRow = el('div', 'like-row');
  likeRow.append(likeButton(r), el('span', 'muted small',
    state.me && state.me.id === r.author_id
      ? '내 모집글'
      : '마음에 들면 눌러 두세요. AI 가 비슷한 공모전과 잘 맞는 팀원을 더 잘 찾습니다.'));
  want.after(likeRow);

  if (typeof r.match === 'number' && r.match > 0) {
    const box = root.querySelector('[data-match]');
    box.hidden = false;
    box.append(scoreRing(r.match));
    const t = el('div');
    t.append(el('strong', null, '나와 맞는 정도'),
             el('p', 'muted small', r.terms.length
               ? `내 관심사와 겹치는 말: ${r.terms.map(fmtTerm).join(', ')}`
               : '겹치는 관심사가 많지 않습니다.'));
    box.append(t);
  }

  if (can_edit) {
    const ownerRow = root.querySelector('[data-owner]');
    ownerRow.hidden = false;
    const tg = ownerRow.querySelector('[data-toggle]');
    tg.textContent = r.status === 'open' ? '모집 마감하기' : '다시 모집하기';
    tg.onclick = async () => {
      try {
        await api(`/api/recruits/${id}/status`, { method: 'PUT',
          body: JSON.stringify({ status: r.status === 'open' ? 'closed' : 'open' }) });
        viewRecruit(id);
      } catch (e) { toast(e.message); }
    };
    ownerRow.querySelector('[data-delete]').onclick = async () => {
      if (!confirm('이 모집글을 지울까요? 댓글도 함께 지워집니다.')) return;
      try {
        await api(`/api/recruits/${id}`, { method: 'DELETE' });
        toast('지웠습니다.');
        location.hash = '#/recruits';
      } catch (e) { toast(e.message); }
    };
  }

  const listEl = root.querySelector('[data-comments]');
  const countEl = root.querySelector('[data-count]');
  const paint = items => {
    countEl.textContent = `(${items.length})`;
    listEl.replaceChildren(...(items.length
      ? items.map(c => commentRow(c, async cid => {
          try {
            await api(`/api/recruit-comments/${cid}`, { method: 'DELETE' });
            paint((await api(`/api/recruits/${id}`)).comments);
          } catch (e) { toast(e.message); }
        }))
      : [el('p', 'empty', '아직 댓글이 없습니다. 참여 의사를 남겨 보세요.')]));
  };
  paint(comments);

  const form = root.querySelector('[data-comment-form]');
  const ta = form.querySelector('textarea');
  const reason = root.querySelector('[data-reason]');
  if (!state.me) {
    ta.disabled = true;
    form.querySelector('button').disabled = true;
    reason.textContent = '로그인 후 댓글을 쓸 수 있습니다.';
    reason.classList.add('blocked');
  } else {
    reason.textContent = `${state.me.nickname} 이름으로 씁니다.`;
  }
  form.onsubmit = async ev => {
    ev.preventDefault();
    const text = ta.value.trim();
    if (!text) return;
    try {
      await api(`/api/recruits/${id}/comments`, { method: 'POST', body: JSON.stringify({ body: text }) });
      ta.value = '';
      paint((await api(`/api/recruits/${id}`)).comments);
    } catch (e) { toast(e.message); }
  };

  render(node);
}

/* ══════════════════════════════════════════ 팀원 찾기 · 내 프로필 */

function personCard(m) {
  const c = el('article', 'person-card');
  const head = el('div', 'person-head');
  const av = el('span', 'avatar lg', m.nickname.slice(0, 1));
  const who = el('div', 'person-who');
  who.append(el('strong', null, m.nickname),
             el('span', 'muted small', [m.college_name, m.department_name].filter(Boolean).join(' · ')));
  head.append(av, who, scoreRing(m.score));
  c.append(head);

  const tags = el('div', 'tag-list');
  const shared = new Set(m.shared_tags);
  for (const t of m.tags) tags.append(el('span', shared.has(t) ? 'ttag hit' : 'ttag', t));
  c.append(tags);

  const social = (m.terms || []).filter(t => t.includes('♥'));
  if (social.length) c.append(el('p', 'person-like', social.join(' · ')));
  if (m.shared_tags.length)
    c.append(el('p', 'person-why', `공통 키워드 ${m.shared_tags.length}개 · ${m.shared_tags.join(', ')}`));
  if (m.bio) c.append(el('p', 'person-bio', m.bio));
  if (m.open_recruits) {
    const a = el('a', 'small', `이 회원의 모집글 ${m.open_recruits}건 보기 →`);
    a.href = '#/recruits';
    c.append(a);
  }
  return c;
}

async function viewPeople() {
  markNav('people');
  setPageTitle('팀원 찾기');
  if (!state.me) return requireLogin('나와 맞는 회원을 보려면 로그인하세요.');
  if (state.me.is_admin) return renderMessage('팀원 추천은 학생 계정에서 볼 수 있습니다.');

  const node = tpl('tpl-people');
  const root = node.firstElementChild;
  const list = root.querySelector('[data-list]');
  const mine = root.querySelector('[data-my-tags]');
  list.append(...skeleton(6, 'sk-reco'));
  render(node);

  try {
    const r = await api('/api/members/matches?limit=24');
    mine.replaceChildren(el('span', 'muted small', '내 키워드'),
      ...(r.my_tags.length ? r.my_tags.map(t => el('span', 'ttag hit', t))
                           : [el('span', 'small', '아직 고르지 않았습니다')]));
    if (!r.my_tags.length) {
      list.replaceChildren(el('p', 'empty', '내 프로필에서 관심 키워드를 고르면 잘 맞는 회원을 찾아 드립니다.'));
      return;
    }
    list.replaceChildren(...(r.members.length
      ? r.members.map(personCard)
      : [el('p', 'empty', '아직 비슷한 회원이 없습니다.')]));
  } catch (e) {
    list.replaceChildren(el('p', 'empty', e.message));
  }
}

async function viewMe() {
  markNav('me');
  setPageTitle('내 프로필');
  if (!state.me) return requireLogin('로그인하세요.');

  const node = tpl('tpl-me');
  const root = node.firstElementChild;
  const form = root.querySelector('[data-form]');
  root.querySelector('[data-who]').textContent =
    `${state.me.nickname} · ${state.me.department_name || '관리자'}`;

  let prof = { bio: '', interest_ids: [] };
  try { prof = await api('/api/profile'); } catch { /* 빈 프로필로 시작 */ }
  const getTags = interestPicker(root.querySelector('[data-interest-picker]'), prof.interest_ids,
                                 { countEl: root.querySelector('[data-pick-count]'), min: 1 });
  form.querySelector('[name="bio"]').value = prof.bio || '';
  render(node);

  form.onsubmit = async ev => {
    ev.preventDefault();
    const ids = getTags();
    if (!ids.length) return toast('관심 키워드를 하나 이상 고르세요.');
    try {
      await api('/api/profile', { method: 'PUT',
        body: JSON.stringify({ interest_ids: ids, bio: form.querySelector('[name="bio"]').value }) });
      toast('저장했습니다. 추천에 바로 반영됩니다.');
    } catch (e) { toast(e.message); }
  };
}

/* ══════════════════════════════════════════════════ AI 구조 소개 */

async function viewAi() {
  markNav('ai');
  setPageTitle('AI 구조');
  const node = tpl('tpl-ai');
  const root = node.firstElementChild;
  const set = (k, v) => { root.querySelector(`[data-s="${k}"]`).textContent = v; };
  render(node);

  try {
    const m = await api('/api/ml/info');
    set('mode', m.embedding_dim ? '딥러닝 임베딩 + TF-IDF' : 'TF-IDF 만 (임베딩 서비스 꺼짐)');
    set('emb', m.embedding_dim ? `${m.embedding_model.replace(/^.*\//, '')} · ${m.embedding_dim}차원` : '사용 안 함');
    set('posts', `${m.posts}건`);
    set('depts', `${m.departments}개`);
    set('labels', `${m.labels}건`);
    set('trained', m.trained_at || '-');
    root.querySelector('[data-stats]').classList.toggle('off', !m.embedding_dim);
  } catch (e) {
    set('mode', '상태를 불러올 수 없음');
  }
}

/* ══════════════════════════════════════════════════════ 라우팅 */

function route() {
  closeSidebar();
  const parts = (location.hash || '#/posts').slice(2).split('/');

  /* 히어로는 목록 화면에만 보인다. 상세·로그인·관리자 화면은 바로 본문부터. */
  const isList = !parts[0] || parts[0] === 'mine' || (parts[0] === 'posts' && !parts[1]);
  document.getElementById('hero').hidden = !isList;

  if (parts[0] === 'login') {
    if (state.me) return logout();
    return viewAuth();
  }
  if (parts[0] === 'register') {
    if (state.me) { location.hash = '#/posts'; return; }
    return viewAuth(true);
  }
  if (parts[0] === 'admin') return viewAdmin();
  if (parts[0] === 'mine') return viewList(true);
  if (parts[0] === 'reco') return viewReco(parts[1]);
  if (parts[0] === 'recruits' && parts[1] === 'new') return viewRecruitNew(parts[2]);
  if (parts[0] === 'recruits' && parts[1]) return viewRecruit(Number(parts[1]));
  if (parts[0] === 'recruits') return viewRecruits();
  if (parts[0] === 'people') return viewPeople();
  if (parts[0] === 'ai') return viewAi();
  if (parts[0] === 'me') return viewMe();
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
    const [me, depts, tags] = await Promise.all([
      api('/api/me'),
      api('/api/departments'),
      api('/api/tags'),
    ]);
    state.tags = tags.groups;
    state.me = me.user;
    state.aiEnabled = !!me.ai_enabled;
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
