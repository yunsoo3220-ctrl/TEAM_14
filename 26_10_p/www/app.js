/* SKU14 공모전 게시판 - 프런트엔드 (의존성 없음)
 * 서버의 /api/* 를 호출하고 해시 라우팅으로 화면을 바꾼다.
 *
 * 구조 개요
 *   - 단일 페이지 앱(SPA): index.html 한 장을 띄워 두고, 주소의 # 뒤 부분(#/posts, #/posts/12 ...)이
 *     바뀔 때마다 route() 가 알맞은 view 함수를 불러 #view 영역만 새로 그린다.
 *     # 뒤는 서버로 전송되지 않으므로 화면을 바꿔도 페이지를 다시 받지 않는다.
 *   - 화면 조각은 index.html 의 <template id="tpl-..."> 를 복제(tpl)해 채운다.
 *   - 사용자 입력이나 서버 데이터는 textContent 로만 넣는다(el 함수). innerHTML 을 쓰지 않으므로
 *     게시물 제목에 <script> 같은 것이 들어 있어도 글자로만 보인다 (XSS 방지).
 *   - 프레임워크(React 등) 없이 표준 DOM API 만 쓴다.
 */

'use strict';   /* 엄격 모드: 선언 안 한 변수 사용 같은 실수를 오류로 잡아 준다 */

/* 앱 전체가 공유하는 상태. 페이지를 새로 고치면 boot() 가 다시 채운다. */
const state = {
  me: null,         /* 로그인한 사용자 (/api/me 의 user), 비로그인이면 null */
  aiEnabled: false, /* 서버에 ANTHROPIC_API_KEY 가 있을 때만 AI 메뉴·추천을 보인다 */
  colleges: [],     /* [{id, name, departments:[{id,name}]}] */
  tags: [],         /* [{category, label, tags:[{id,name}]}] 관심 키워드 */
};

/* 서버의 kind 코드 → 화면 표시 이름 */
const KIND_LABEL = { contest: '공모전', hackathon: '해커톤', etc: '기타' };

/* ══════════════════════════════════════════════════════════ 유틸 */

/* 서버 API 호출 도우미.
 *   - 같은 출처 쿠키(sid)를 함께 보낸다 (credentials: 'same-origin')
 *   - 본문이 있으면 JSON 헤더를 붙인다
 *   - 성공(2xx)이면 파싱한 JSON 을, 실패면 서버의 오류 메시지를 담은 Error 를 던진다
 *     (err.code 에 서버의 기계용 코드, 예: 'login_required')
 * 사용 예: const r = await api('/api/posts');  await api('/api/login', {method:'POST', body: JSON.stringify(...)}) */
async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: 'same-origin',
    headers: options.body ? { 'Content-Type': 'application/json' } : {},
    ...options,   /* 전개 구문: 호출자가 준 method, body 등을 덮어 넣는다 */
  });

  let data = null;
  try { data = await res.json(); } catch { /* 본문 없는 응답 */ }

  if (!res.ok) {
    /* 서버 오류 형식: {"error":{"code":"...","message":"..."}} (src/http.c 의 res_error) */
    const msg = data && data.error ? data.error.message : `요청 실패 (${res.status})`;
    const err = new Error(msg);
    err.code = data && data.error ? data.error.code : String(res.status);
    throw err;
  }
  return data;
}

/* 화면 아래에 잠깐 떴다 사라지는 알림 (3.2초). 연달아 부르면 타이머를 새로 건다. */
let toastTimer = null;
function toast(message) {
  const el = document.getElementById('toast');
  el.textContent = message;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3200);
}

/* <template id="..."> 의 내용을 복제해 돌려준다 (DocumentFragment) */
const tpl = id => document.getElementById(id).content.cloneNode(true);
/* 요소를 만드는 짧은 도우미: el('p', 'muted', '내용') → <p class="muted">내용</p>
 * 글자는 textContent 로 넣으므로 HTML 로 해석되지 않는다 (안전). */
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
};

/* 본문 영역(#view)을 node 로 바꾸고 맨 위로 스크롤 */
function render(node) {
  document.getElementById('view').replaceChildren(node);
  window.scrollTo(0, 0);
}

/* 본문에 안내 문장 하나만 보여 준다 (오류, 권한 없음 등) */
function renderMessage(text) {
  render(el('p', 'empty', text));
}

/* <form> 의 입력값을 {name: 값} 객체로. 문자열은 앞뒤 공백을 뗀다.
 * (같은 name 이 여러 개면 마지막 값만 남는다 - 체크박스 목록은 따로 모은다) */
function formValues(form) {
  const out = {};
  for (const [k, v] of new FormData(form)) out[k] = typeof v === 'string' ? v.trim() : v;
  return out;
}

/* 게시판 표시 이름. 서버의 make_display_name() 과 같은 규칙이라 미리보기에 쓴다.
 * 실제로 저장되는 값은 언제나 서버가 만든 것이다.
 *   손동권 + 010-9948-9687 -> 손*권_9948 */
function displayName(name, phone) {
  /* [...문자열] 은 글자 단위로 쪼갠다 (한글·이모지도 한 글자씩) */
  const chars = [...(name || '').trim()];
  const digits = (phone || '').replace(/\D/g, '');   /* 숫자가 아닌 것(\D)을 모두 지움 */

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

/* 'YYYY-MM-DD' 마감일까지 남은 날 수 (그날 23:59:59 기준). 날짜가 이상하면 null.
 * 86400000 = 하루의 밀리초 (24 x 60 x 60 x 1000) */
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
  if (byAi) t.title = 'AI 가 본문에서 찾은 마감일';   /* 마우스를 올리면 보이는 설명 */
  return t;
}

/* 모델은 영문을 소문자로 다룬다. 짧은 약어(ai, sns)는 대문자로 보여준다. */
const fmtTerm = t => (/^[a-z0-9]{2,4}$/.test(t) ? t.toUpperCase() : t);

/* 태그 배열 → <div class="tag-list"><span>#태그</span>...</div> */
function tagList(tags, cls = 'tag-list') {
  const box = el('div', cls);
  for (const t of tags || []) box.append(el('span', 'ttag', `#${fmtTerm(t)}`));
  return box;
}

/* 관련도 점수를 원형 게이지로 보여준다.
 * CSS 변수 --p 에 0~100 을 넣으면 style.css 의 conic-gradient 가 그만큼 원을 채운다. */
function scoreRing(score) {
  const ring = el('span', 'score-ring');
  ring.style.setProperty('--p', Math.max(0, Math.min(100, score)));
  ring.append(el('b', null, String(score)));
  ring.title = `내 학과 관련도 ${score}점`;
  return ring;
}

/* 데이터를 기다리는 동안 보여 줄 회색 자리표시자(스켈레톤) n 개 */
function skeleton(n, cls = 'sk-item') {
  return Array.from({ length: n }, () => {
    const s = el('div', `skeleton ${cls}`);
    s.append(el('i'), el('i'), el('i'));
    return s;
  });
}

/* ═════════════════════════════════════════════ 사이드바 / 상단 / 히어로 */

/* 상단 제목 바꾸기 */
function setPageTitle(text) {
  document.getElementById('page-title').textContent = text;
}

/* 사이드바에서 현재 메뉴만 강조 (data-nav 값이 name 인 링크에 .on) */
function markNav(name) {
  for (const a of document.querySelectorAll('[data-nav]'))
    a.classList.toggle('on', a.dataset.nav === name);
}

/* 로그인 상태에 따라 화면 테두리(사이드바 메뉴, 상단 사용자 정보, 히어로)를 다시 그린다.
 * 로그인/로그아웃 직후와 처음 시작할 때 부른다. */
function paintChrome() {
  const me = state.me;

  /* 관리자 메뉴는 관리자에게만, 내 프로필은 로그인한 사람에게만 */
  document.getElementById('nav-admin').hidden = !(me && me.is_admin);
  document.getElementById('nav-me').hidden = !me;
  /* label 안의 첫 텍스트 노드만 바꾼다 (뒤의 <small>Sign in</small> 은 그대로 두기 위해) */
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
    const avatar = el('span', 'avatar', me.nickname.slice(0, 1));   /* 표시 이름 첫 글자 동그라미 */
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

/* 상단 큰 인사 영역(히어로). 비로그인이면 서비스 소개, 로그인이면 인사와 소속 */
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

/* 로그아웃: 서버 세션 삭제 → 상태 비우기 → 목록 화면으로 */
async function logout() {
  await api('/api/logout', { method: 'POST' });
  state.me = null;
  paintChrome();
  /* 해시가 바뀌면 hashchange 가 route() 를 부른다. 이미 목록이면 직접 부른다. */
  if (location.hash === '#/posts' || location.hash === '') route();
  else location.hash = '#/posts';
}

/* ══════════════════════════════════════════════════════ 타일 */

/* 숫자 요약 타일: 큰 숫자(value) + 이름(label) + 작은 설명(note) */
function tile(cls, value, label, note) {
  const n = el('div', `tile ${cls}`);
  n.append(el('b', null, value), el('span', null, label));
  if (note) n.append(el('small', null, note));
  return n;
}

/* 누를 수 있는 타일: tile 로 만든 내용을 <a> 로 옮겨 담는다 */
function linkTile(href, cls, value, label, note) {
  const n = tile(cls, value, label, note);
  const a = document.createElement('a');
  a.href = href;
  a.className = n.className;
  a.replaceChildren(...n.childNodes);
  return a;
}

/* ══════════════════════════════════════════════════ 게시판 목록 */

/* 게시물 목록의 한 줄(카드). 누르면 #/posts/{id} 상세로 간다.
 * 구성: [종류·D-day·참여 가능 배지] 제목 / 요약(없으면 본문 앞부분) / 태그 / 메타 / 대상 학과 */
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

  /* 메타 정보는 있는 것만 모아 점(·)으로 구분해 보여 준다 (구분 기호는 CSS 가 붙임) */
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

/* Claude 분석 결과 추천 카드: 점수 게이지 + AI 가 쓴 추천 이유 */
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
  why.append(el('span', 'spark', '✦'), r.reason);   /* append 에 문자열을 주면 텍스트 노드가 된다 */

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
  /* 근거 키워드 (src/ml.c 의 explain 결과) */
  if (r.terms && r.terms.length) {
    const why = el('div', 'reco-terms');
    why.append(el('span', 'spark', '✦'), el('span', 'muted', '겹친 키워드'));
    for (const t of r.terms) why.append(el('span', 'ttag hit', fmtTerm(t)));
    a.append(why);
  }
  return a;
}

/* 목록 위에 붙는 추천 영역. Claude 분석이 켜져 있으면 그 결과를, 아니면 자체 모델을 쓴다.
 *   비로그인 → 안내 배너 / 관리자 → 숨김 / 학생 → 내 학과 추천 3개 */
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

  /* 먼저 스켈레톤을 보여 주고, 응답이 오면 바꾼다 */
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
    box.hidden = true;   /* 추천은 부가 기능이라 실패하면 조용히 숨긴다 */
  }
}

/* Claude 가 꺼져 있을 때: 자체 모델(/api/related)로 내 학과 추천 3개 */
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

/* 관련도 구간. 같은 구간끼리 묶어 보여준다. (위에서부터 차례로 min 이상인 첫 구간에 들어간다) */
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

  /* 학과 선택 상자: 단과대학별 <optgroup> 으로 묶는다 */
  for (const c of state.colleges) {
    const g = document.createElement('optgroup');
    g.label = c.name;
    for (const d of c.departments) g.append(new Option(d.name, d.id));
    sel.append(g);
  }

  /* 보여 줄 학과: 주소의 번호 > 내 학과 > 첫 번째 학과 */
  const mine = state.me && state.me.department_id;
  const dept = Number(deptParam) || mine || (state.colleges[0] && state.colleges[0].departments[0].id);
  sel.value = String(dept);
  sel.onchange = () => { location.hash = `#/reco/${sel.value}`; };   /* 고르면 주소가 바뀌고 route() 가 다시 그림 */

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

    /* 구간마다 [min, 윗 구간의 min) 범위의 글을 모아 섹션으로. 빈 구간은 null → filter 로 제거 */
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

/* #/posts (forceMine = false) 또는 #/mine (true) - 공모전 목록 */
async function viewList(forceMine) {
  markNav(forceMine ? 'mine' : 'posts');
  setPageTitle(forceMine ? '내 학과 대상 공모전' : '공모전 목록');

  const node = tpl('tpl-list');
  const root = node.firstElementChild;
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const tilesEl = root.querySelector('[data-tiles]');
  const deptSel = root.querySelector('[data-f="department"]');

  /* 학과 필터 선택 상자 채우기 (단과대학별 묶음) */
  for (const c of state.colleges) {
    const group = document.createElement('optgroup');
    group.label = c.name;
    for (const d of c.departments) group.append(new Option(d.name, d.id));
    deptSel.append(group);
  }

  /* 필터 세 가지: 학과, 종류, "내 학과가 참여 가능한 것만" */
  const filters = {
    department: deptSel,
    kind: root.querySelector('[data-f="kind"]'),
    mine: root.querySelector('[data-f="mine"]'),
  };
  if (!state.me) filters.mine.disabled = true;         /* 비로그인은 "내 학과" 를 알 수 없다 */
  if (forceMine && state.me) filters.mine.checked = true;

  /* 현재 필터로 목록을 다시 불러온다 */
  async function reload() {
    /* URLSearchParams: 쿼리 문자열을 안전하게 만든다 (한글 등은 자동 인코딩) */
    const qs = new URLSearchParams();
    if (filters.department.value) qs.set('department_id', filters.department.value);
    if (filters.kind.value) qs.set('kind', filters.kind.value);
    if (filters.mine.checked) qs.set('mine', '1');

    listEl.replaceChildren(...skeleton(4));
    try {
      const { posts } = await api(`/api/posts?${qs}`);   /* 구조 분해: 응답의 posts 만 꺼낸다 */
      countEl.textContent = `(${posts.length})`;
      listEl.replaceChildren(...(posts.length
        ? posts.map(postItem)
        : [el('p', 'empty', '조건에 맞는 공모전이 없습니다.')]));
    } catch (e) {
      listEl.replaceChildren(el('p', 'empty', e.message));
    }
  }

  for (const f of Object.values(filters)) f.onchange = reload;   /* 필터가 바뀔 때마다 다시 불러오기 */

  render(node);
  reload();
  if (!forceMine) mountReco(root.querySelector('[data-reco]'));

  /* 타일은 필터와 무관한 전체 집계라 따로 한 번 불러온다. */
  try {
    const { posts } = await api('/api/posts');
    const soon = posts.filter(p => {                     /* 마감 0~7일 남은 공모전 수 */
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

/* 댓글 한 줄. 내 댓글이거나 관리자면 [삭제] 버튼을 붙인다.
 * (버튼을 숨기는 것은 편의일 뿐이고, 실제 권한 검사는 서버가 한다) */
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

/* #/posts/{id} - 게시물 상세 + 댓글 */
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
  const reloadDetail = () => viewDetail(id);   /* 관리자가 학과를 고친 뒤 화면을 새로 그릴 때 */

  const ai = post.ai;   /* Claude 분석 결과 (없으면 null) */
  const kindEl = root.querySelector('[data-kind]');
  kindEl.textContent = KIND_LABEL[post.kind] || post.kind;
  kindEl.classList.add(`kind-${post.kind}`);
  root.querySelector('[data-title]').textContent = post.title;

  /* 주최·마감이 비어 있으면 AI 가 본문에서 찾은 값을 표시하고 출처를 밝힌다. */
  const fillMeta = (sel, own, guess) => {
    const dd = root.querySelector(sel);
    if (own) { dd.textContent = own; return; }    /* 관리자가 직접 입력한 값 우선 */
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
  /* 본문도 textContent 로 넣는다 (CSS 의 white-space 로 줄바꿈을 살린다) */
  root.querySelector('[data-body]').textContent = post.body;

  /* 원본 공지 링크: 새 탭으로 열고, rel=noopener 로 새 탭이 이 페이지를 조작하지 못하게 한다 */
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
      /* 내 학과는 .me 로 강조 */
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
    /* 모집글 쓰기 링크: 로그인했으면 이 공모전이 미리 연결된 글쓰기 화면으로 */
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

  /* 삭제 후 상세를 다시 받아 댓글만 새로 그린다 (화면 전체를 다시 그리면 스크롤이 맨 위로 간다) */
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

  /* 서버가 알려 준 권한(permission)에 따라 입력창을 막거나 안내한다 */
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
    ev.preventDefault();          /* 폼의 기본 동작(페이지 이동)을 막고 fetch 로 보낸다 */
    const body = textarea.value.trim();
    if (!body) return;
    submit.disabled = true;       /* 두 번 눌러 중복 등록되는 것 방지 */
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
      submit.disabled = false;    /* 성공·실패와 상관없이 다시 누를 수 있게 */
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
    /* 막대 그래프: 바깥 .bar 안의 <i> 너비를 점수% 로 */
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

/* Claude 분석 카드: 요약, 태그, 학과별 점수와 이유 */
function paintAiCard(card, ai) {
  card.hidden = false;
  card.querySelector('[data-ai-meta]').textContent = `${ai.analyzed_at} 분석`;
  card.querySelector('[data-ai-summary]').textContent = ai.summary;
  card.querySelector('[data-ai-tags]').replaceWith(tagList(ai.tags));   /* 자리표시자 요소를 통째로 교체 */

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

/* 학교 이메일 형식 (서버의 school_email 과 같은 규칙: 소문자·숫자·. _ - + @skuniv.ac.kr) */
const SCHOOL_EMAIL = /^[a-z0-9._-]+@skuniv\.ac\.kr$/;

/* #/login (register = false) 또는 #/register (true).
 * 한 템플릿 안에 로그인 카드와 가입 카드가 모두 있고, 하나만 보이게 한다. */
function viewAuth(register = false) {
  markNav('login');
  setPageTitle(register ? '회원가입' : '로그인');

  const node = tpl('tpl-auth');
  const root = node.firstElementChild;
  root.querySelector('[data-login-card]').hidden = register;
  root.querySelector('[data-register-card]').hidden = !register;

  /* 가입 폼의 학과 선택 상자 */
  const sel = root.querySelector('[name="department_id"]');
  sel.append(new Option('학과를 선택하세요', ''));
  for (const c of state.colleges) {
    const g = document.createElement('optgroup');
    g.label = c.name;
    for (const d of c.departments) g.append(new Option(d.name, d.id));
    sel.append(g);
  }

  /* 로그인 제출 */
  root.querySelector('[data-login]').onsubmit = async ev => {
    ev.preventDefault();
    try {
      const out = await api('/api/login', {
        method: 'POST',
        body: JSON.stringify(formValues(ev.target)),
      });
      state.me = out.user;      /* 서버가 쿠키를 설정했으므로 이후 요청은 로그인 상태 */
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
  reg.querySelector('[name="name"]').oninput = refreshPreview;    /* 한 글자 입력할 때마다 */
  reg.querySelector('[name="phone"]').oninput = refreshPreview;

  /* 관심 키워드 (선택, 0~8개) - 고르면 팀원 추천과 개인 맞춤 공모전 추천에 쓰인다 */
  const getTags = interestPicker(reg.querySelector('[data-interest-picker]'), [],
                                 { countEl: reg.querySelector('[data-pick-count]') });

  /* 학교 이메일 인증
   * 흐름: [인증 코드 받기] → 메일로 6자리 → 코드 입력 후 [확인] → 이메일 잠금 → 나머지 입력 → [가입] */
  const emailEl = reg.querySelector('[name="email"]');
  const codeEl = reg.querySelector('[name="code"]');
  const codeRow = reg.querySelector('[data-code-row]');
  const sendBtn = reg.querySelector('[data-send-code]');
  const timerEl = reg.querySelector('[data-timer]');
  const codeMsg = reg.querySelector('[data-code-msg]');
  const verifyBtn = reg.querySelector('[data-verify-code]');
  /* sentTo: 코드를 보낸 주소, tick: 카운트다운 타이머 id, verified: 인증 완료 여부 */
  let sentTo = '', tick = null, verified = false;

  /* 인증 안내 문구. kind 는 색 (muted 회색 / ok 초록 / warn 노랑 / blocked 빨강) */
  const say = (text, kind = 'muted') => { codeMsg.textContent = text; codeMsg.className = `auth-msg small ${kind}`; };

  /* 남은 유효 시간(분:초)과 재발송 대기 시간을 1초마다 갱신 */
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
      if (!left && !again) clearInterval(tick);   /* 둘 다 끝나면 더 셀 필요 없음 */
    };
    step();
    tick = setInterval(step, 1000);
  }

  /* [인증 코드 받기] */
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
  /* 코드 칸: 숫자만 남기고 6자리로 자른다 */
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
      emailEl.readOnly = codeEl.readOnly = true;   /* 인증한 주소를 바꾸지 못하게 */
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
      say(e.message, 'blocked');   /* 서버가 남은 기회를 알려 준다 */
      verifyBtn.disabled = false;
      codeEl.select();
    }
  }
  verifyBtn.onclick = verifyCode;
  codeEl.onkeydown = ev => {                        /* 코드 칸에서 Enter = [확인] (가입 제출 아님) */
    if (ev.key === 'Enter') { ev.preventDefault(); verifyCode(); }
  };

  /* [가입] 제출: 화면에서 한 번 더 확인한 뒤 서버로 (최종 검사는 서버가 한다) */
  reg.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(ev.target);
    v.email = (v.email || '').trim().toLowerCase();
    if (!SCHOOL_EMAIL.test(v.email)) return toast('학교 이메일(@skuniv.ac.kr)을 입력하세요.');
    if (!sentTo) return toast('먼저 인증 코드 받기를 누르세요.');
    if (!verified) return toast('인증 코드를 입력하고 [확인]을 눌러 이메일 인증을 마치세요.');
    delete v.code;   /* 이미 [확인] 으로 인증했으므로 코드는 보내지 않는다 */
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
      /* 로그인 화면이 그려진 직후(다음 이벤트 루프)에 학번을 채우고 비밀번호 칸으로 커서를 옮긴다 */
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

/* 대학별로 묶은 학과 체크박스. name 접두사로 여러 폼에서 재사용한다.
 * (한 화면에 체크박스 묶음이 여러 개 있어도 prefix 로 서로 섞이지 않게 구분) */
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
      label.append(input, el('span', null, d.name));   /* <label> 로 감싸 글자를 눌러도 체크된다 */
      opts.append(label);
    }

    box.append(el('strong', null, c.name), opts);
    return box;
  }));
}

/* 체크된 학과 id 배열 */
function checkedDepts(container, prefix) {
  return [...container.querySelectorAll(`input[name="${prefix}dept"]:checked`)]
    .map(i => Number(i.value));
}

/* 게시물 상세에 붙는 관리자용 대상 학과 편집기.
 * 체크를 모두 풀고 저장하면 제한이 풀려 누구나 댓글을 쓸 수 있는 글이 된다. */
function mountDeptEditor(container, post, onSaved) {
  const prefix = `p${post.id}-`;
  const details = el('details', 'dept-editor');   /* <details>: 눌러서 펼치고 접는 상자 */
  details.append(el('summary', null, '대상 학과 지정 (관리자)'));

  const grid = el('div', 'dept-grid');
  details.append(grid);
  deptChecks(grid, prefix);

  /* 현재 지정된 학과를 미리 체크 (Set 으로 빠르게 포함 여부 확인) */
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

/* 수집한 공지 한 줄 (관리자 화면). 대기 중이면 학과 체크박스와 [게시물로 등록]/[숨기기] 버튼 */
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

/* 확률 0~1 → 화면 표시 ('99%+', '<1%', '37%') */
const fmtProb = p => (p >= 0.995 ? '99%+' : p < 0.005 ? '<1%' : `${Math.round(p * 100)}%`);

/* 관리자 글쓰기: 입력하는 동안 학과별 적합 확률을 실시간으로 보여 준다.
 * 서버는 재학습 없이 지금 모델로 계산한다 (POST /api/ml/predict).
 *   box      예측 결과를 그릴 상자
 *   form     제목·본문 입력이 있는 폼
 *   checksEl 대상 학과 체크박스 묶음 (예측 행을 누르면 여기 체크가 바뀐다)
 * 돌려주는 값: { clear } - 글을 등록한 뒤 예측 영역을 비울 때 쓴다 */
function mountPredict(box, form, checksEl, prefix) {
  const list = box.querySelector('[data-predict-list]');
  const meta = box.querySelector('[data-predict-meta]');
  const apply = box.querySelector('[data-predict-apply]');
  const titleEl = form.querySelector('[name="title"]');
  const bodyEl = form.querySelector('[name="body"]');
  const intro = meta.textContent;   /* 처음 안내 문구 (비울 때 되돌림) */
  /* timer: 입력 멈춤 감지용, seq: 요청 순번(늦게 온 옛 응답 무시용), last: 마지막 예측 결과 */
  let timer = null, seq = 0, last = [];

  const checkbox = id => checksEl.querySelector(`input[name="${prefix}dept"][value="${id}"]`);

  /* 상위 8개 학과를 막대로. 행을 누르면 그 학과 체크박스를 켜고 끈다 */
  function paint(depts) {
    list.replaceChildren(...depts.slice(0, 8).map(d => {
      const row = el('button', 'predict-row');
      row.type = 'button';    /* type 을 안 주면 폼 안의 버튼은 submit 이 되어 글이 등록돼 버린다 */
      const cb = checkbox(d.id);
      row.classList.toggle('on', !!(cb && cb.checked));
      row.title = '눌러서 대상 학과로 체크/해제';
      const head = el('div', 'predict-row-head');
      head.append(el('strong', null, d.name), el('span', 'muted small', d.college_name),
                  el('b', 'predict-prob', fmtProb(d.prob)));
      const bar = el('div', 'bar');
      const fill = el('i');
      fill.style.width = `${Math.max(1, d.prob * 100)}%`;   /* 0% 라도 1% 는 보이게 */
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

  /* 서버에 예측 요청. 응답이 늦게 와서 순서가 뒤바뀌어도 가장 최근 요청의 결과만 그린다 */
  async function run() {
    const title = titleEl.value.trim(), body = bodyEl.value.trim();
    if ((title + body).length < 6) return clear();   /* 너무 짧으면 의미 있는 예측이 안 된다 */
    const my = ++seq, t0 = performance.now();          /* 이번 요청 번호와 시작 시각 */
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
      apply.hidden = !last.some(d => d.prob >= 0.3);   /* 30% 넘는 학과가 있을 때만 [추천 학과 체크] 버튼 */
    } catch (e) {
      if (my === seq) meta.textContent = e.message;
    } finally {
      if (my === seq) box.classList.remove('busy');
    }
  }

  /* 예측 영역 비우기. seq 를 올려 진행 중인 요청의 응답도 무시되게 한다 */
  function clear() {
    seq++;
    last = [];
    list.replaceChildren();
    meta.textContent = intro;
    apply.hidden = true;
    box.classList.remove('busy');
  }

  /* 디바운스: 입력이 0.45초 멈췄을 때만 요청한다 (한 글자마다 서버에 보내지 않게) */
  const schedule = () => { clearTimeout(timer); timer = setTimeout(run, 450); };
  titleEl.addEventListener('input', schedule);
  bodyEl.addEventListener('input', schedule);
  /* 체크박스를 직접 바꾸면 예측 행의 강조도 다시 그린다 */
  checksEl.addEventListener('change', () => last.length && paint(last));
  /* [추천 학과 체크]: 확률 30% 이상인 학과를 한꺼번에 체크 */
  apply.onclick = () => {
    for (const d of last) {
      const cb = checkbox(d.id);
      if (cb && d.prob >= 0.3) cb.checked = true;
    }
    paint(last);
  };
  return { clear };
}

/* #/admin - 관리자 화면 */
async function viewAdmin() {
  markNav('admin');
  setPageTitle('관리자');

  /* 화면에서 막는 것은 편의일 뿐, 관리자 API 는 서버가 다시 권한을 검사한다 */
  if (!state.me || !state.me.is_admin) {
    renderMessage('관리자만 볼 수 있습니다.');
    return;
  }

  const node = tpl('tpl-admin');
  const root = node.firstElementChild;
  render(node);

  /* ── 상단 요약 타일 */
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

  /* ── 수집한 공지 목록 ([전체 보기] 체크 해제 시 대기 중인 것만) */
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

  /* 공지 → 게시물 등록. 끝나면 목록과 요약을 동시에(Promise.all) 새로 고친다 */
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

  /* ── [지금 수집] 버튼: 서버가 학교 홈페이지를 읽는 동안(수 초~수십 초) 버튼을 막아 둔다 */
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

  /* 진행률 막대와 상태 문장을 그린다. 실행 중이면 2초 뒤 다시 읽도록 예약 (폴링) */
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

  /* 분석 시작. force = true 면 이미 분석한 글까지 전부 다시 (API 비용이 들므로 확인을 받는다) */
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

  /* ── 새 게시물 쓰기 폼 (+ 실시간 학과 예측) */
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
      /* 빈 선택 항목은 null 로 보내 DB 에 NULL 로 저장되게 한다 */
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
      form.reset();                  /* 입력칸 비우기 */
      deptChecks(checksEl, 'new-');  /* 체크박스도 새로 (모두 해제) */
      predict.clear();
      await Promise.all([reloadSummary(), reloadAi()]);
    } catch (e) {
      toast(e.message);
    } finally {
      btn.disabled = false;
    }
  };

  /* 처음 화면을 열 때 세 영역을 동시에 불러온다 */
  await Promise.all([reloadSummary(), reloadNotices(), reloadAi()]);
}

/* ══════════════════════════════════════════ 관심 키워드 선택기 */

/* 분야·역할·성향으로 묶은 키워드 칩. 고른 번호 배열을 돌려주는 함수를 돌려준다.
 *   container 칩을 그릴 요소, selected 처음에 골라 둘 id 들
 *   max 최대 개수, countEl "3 / 8개 선택" 을 보여 줄 요소, min 최소 개수(안내용)
 * 사용 예: const getTags = interestPicker(box, [1,4]);  ...  getTags() → [1, 4, 7]
 * (클로저: 돌려준 함수가 chosen 집합을 계속 기억한다) */
function interestPicker(container, selected = [], { max = 8, countEl = null, min = 0 } = {}) {
  const chosen = new Set(selected.map(Number));

  /* 고른 상태(.on)와 개수 표시를 갱신 */
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
      b.type = 'button';            /* 폼 안에서 눌러도 제출되지 않게 */
      b.dataset.id = t.id;          /* data-id 속성 */
      b.onclick = () => {
        const id = Number(t.id);
        if (chosen.has(id)) chosen.delete(id);                                  /* 이미 골랐으면 해제 */
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

/* 로그인이 필요한 화면 대신 보여 줄 안내 + 로그인 버튼 */
function requireLogin(message) {
  const box = el('div', 'empty');
  box.append(message, el('br'));
  const a = el('a', 'btn small', '로그인 · 가입');
  a.href = '#/login';
  box.append(a);
  render(box);
}

/* ══════════════════════════════════════════════ 팀원 모집 게시판 */

/* 마음에 들어요 버튼. 누른 기록은 AI 추천(관련 공모전·팀원 찾기)에도 반영된다.
 * r 객체(liked, like_count)를 직접 고쳐 상태를 유지하므로, 같은 r 을 쓰는 다른 곳과도 일치한다. */
function likeButton(r) {
  const b = el('button', 'like-btn');
  b.type = 'button';
  const paint = () => {
    b.classList.toggle('on', !!r.liked);
    b.replaceChildren(el('span', 'heart', r.liked ? '♥' : '♡'), el('span', null, String(r.like_count)));
    b.title = r.liked ? '마음에 들어요 취소' : '마음에 들어요 · AI 추천에 반영됩니다';
    b.setAttribute('aria-pressed', r.liked ? 'true' : 'false');   /* 화면 낭독기에 눌림 상태 알림 */
  };
  const mine = state.me && state.me.id === r.author_id;
  if (mine) {
    b.disabled = true;      /* 내 글에는 못 누른다 (서버도 막는다) */
    b.title = '내 모집글';
  }
  b.onclick = async ev => {
    ev.preventDefault();            // 카드(링크) 안에 있어도 이동하지 않게
    ev.stopPropagation();
    if (!state.me) return toast('로그인하면 마음에 들어요를 누를 수 있습니다.');
    b.disabled = true;
    try {
      /* 지금 눌린 상태면 DELETE(취소), 아니면 POST(누르기) */
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

/* 모집글 목록 카드. 로그인한 학생에게는 나와 맞는 정도(match) 게이지를 붙인다 */
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
  if (r.post_title) a.append(el('p', 'recruit-post', `🏆 ${r.post_title}`));   /* 연결된 공모전 */
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

/* #/recruits - 모집글 목록 (키워드·상태 필터, 최신순/나와 맞는 순 정렬) */
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
  /* 키워드 필터 선택 상자 (분류별 묶음) */
  for (const g of state.tags) {
    const og = document.createElement('optgroup');
    og.label = g.label;
    for (const t of g.tags) og.append(new Option(t.name, t.id));
    f.tag.append(og);
  }
  /* "나와 맞는 순" 은 로그인한 학생만 (점수를 매길 기준이 있어야 하므로). 가능하면 기본값으로 */
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

/* #/recruits/new 또는 #/recruits/new/{공모전 id} - 모집글 쓰기 (공모전을 미리 골라 둘 수 있다) */
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

  /* "함께 나갈 공모전" 선택 상자 채우기 */
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
          title: v.title, body: v.body, post_id: Number(v.post_id) || 0,   /* 0 = 연결 없음 */
          need_people: Number(v.need_people || 1), deadline: v.deadline || null,
          tag_ids: getTags(),
        }),
      });
      toast('모집글을 올렸습니다.');
      location.hash = `#/recruits/${out.id}`;   /* 방금 쓴 글로 이동 */
    } catch (e) {
      toast(e.message);
      btn.disabled = false;
    }
  };
}

/* #/recruits/{id} - 모집글 상세 + 댓글 (작성자에게는 마감/삭제 버튼) */
async function viewRecruit(id) {
  markNav('recruits');
  setPageTitle('모집글');

  let data;
  try { data = await api(`/api/recruits/${id}`); }
  catch (e) { return renderMessage(e.message); }

  /* recruit: r 처럼 구조 분해하면서 이름을 바꿀 수 있다 */
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

  /* 함께 나갈 공모전 링크 (없으면 그 줄을 지운다) */
  const postP = root.querySelector('[data-post]');
  if (r.post_id) {
    const a = el('a', null, `🏆 ${r.post_title}`);
    a.href = `#/posts/${r.post_id}`;
    postP.append(el('span', 'muted', '함께 나갈 공모전 '), a);
  } else postP.remove();

  /* 찾는 키워드와 ♥ 버튼 */
  const want = root.querySelector('[data-want]');
  for (const t of r.tags) want.append(el('span', 'ttag want', t));
  const likeRow = el('div', 'like-row');
  likeRow.append(likeButton(r), el('span', 'muted small',
    state.me && state.me.id === r.author_id
      ? '내 모집글'
      : '마음에 들면 눌러 두세요. AI 가 비슷한 공모전과 잘 맞는 팀원을 더 잘 찾습니다.'));
  want.after(likeRow);   /* want 요소 바로 뒤에 끼워 넣는다 */

  /* 나와 맞는 정도 (로그인한 학생에게만 서버가 match 를 준다) */
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

  /* 작성자·관리자 전용: 모집 마감/재개, 삭제 */
  if (can_edit) {
    const ownerRow = root.querySelector('[data-owner]');
    ownerRow.hidden = false;
    const tg = ownerRow.querySelector('[data-toggle]');
    tg.textContent = r.status === 'open' ? '모집 마감하기' : '다시 모집하기';
    tg.onclick = async () => {
      try {
        await api(`/api/recruits/${id}/status`, { method: 'PUT',
          body: JSON.stringify({ status: r.status === 'open' ? 'closed' : 'open' }) });
        viewRecruit(id);   /* 상태가 바뀌었으니 화면을 다시 그린다 */
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

  /* 댓글 목록 (게시물 상세의 commentRow 를 재사용, 삭제 API 만 다르다) */
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

  /* 댓글 입력 (모집글은 학과 제한 없이 로그인만 하면 쓸 수 있다) */
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

/* ══════════════════════════════════════════════════ 그룹 스터디 */

const ROLE_LABEL = { mentor: '멘토', mentee: '멘티', member: '멤버' };

/* 스터디 목록 카드 */
function studyCard(s) {
  const full = s.member_count >= s.max_members;
  const a = el('a', 'recruit-card' + (full && !s.my_role ? ' closed' : ''));
  a.href = `#/studies/${s.id}`;

  const tags = el('div', 'item-top');
  tags.append(el('span', full ? 'tag plain' : 'tag ok', `${s.member_count} / ${s.max_members}명`));
  if (s.mentor_count) tags.append(el('span', 'tag mentor', `멘토 ${s.mentor_count}`));
  if (s.mentee_count) tags.append(el('span', 'tag mentee', `멘티 ${s.mentee_count}`));
  if (s.my_role) tags.append(el('span', 'tag owner',
    state.me && state.me.id === s.owner_id ? '내가 방장' : `참여 중 · ${ROLE_LABEL[s.my_role]}`));

  a.append(tags, el('h3', null, s.name));
  if (s.excerpt) a.append(el('p', 'item-summary excerpt', s.excerpt));
  const meta = el('div', 'item-meta');
  meta.append(el('span', null, `방장 ${s.owner_nickname}`), el('span', null, s.created_at.slice(0, 10)));
  const foot = el('div', 'recruit-foot');
  foot.append(meta);
  a.append(foot);
  return a;
}

/* #/studies - 스터디 목록 (전체 / 내가 속한 것) */
async function viewStudies() {
  markNav('studies');
  setPageTitle('그룹 스터디');

  const node = tpl('tpl-studies');
  const root = node.firstElementChild;
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const mineSel = root.querySelector('[data-f="mine"]');
  if (!state.me) {
    root.querySelector('[data-new]').href = '#/login';
    mineSel.querySelector('[value="1"]').disabled = true;
  }

  async function reload() {
    listEl.replaceChildren(...skeleton(4, 'sk-reco'));
    try {
      const r = await api(`/api/studies${mineSel.value ? '?mine=1' : ''}`);
      countEl.textContent = `(${r.studies.length})`;
      listEl.replaceChildren(...(r.studies.length
        ? r.studies.map(studyCard)
        : [el('p', 'empty', mineSel.value ? '아직 참여한 스터디가 없습니다.' : '아직 스터디가 없습니다. 첫 스터디를 만들어 보세요.')]));
    } catch (e) {
      listEl.replaceChildren(el('p', 'empty', e.message));
    }
  }
  mineSel.onchange = reload;
  render(node);
  reload();
}

/* #/studies/new - 스터디 만들기 */
function viewStudyNew() {
  markNav('studies');
  setPageTitle('스터디 만들기');
  if (!state.me) return requireLogin('스터디를 만들려면 로그인하세요.');

  const node = tpl('tpl-study-new');
  const form = node.querySelector('[data-form]');
  form.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(form);
    const btn = form.querySelector('button[type="submit"]');
    btn.disabled = true;
    try {
      const out = await api('/api/studies', {
        method: 'POST',
        body: JSON.stringify({ name: v.name, description: v.description, max_members: Number(v.max_members || 6) }),
      });
      toast('스터디를 만들었습니다. 멤버를 추가해 보세요.');
      location.hash = `#/studies/${out.id}`;
    } catch (e) {
      toast(e.message);
      btn.disabled = false;
    }
  };
  render(node);
}

/* 멤버 한 줄. 방장에게는 역할 선택과 [내보내기] 를 붙인다. */
function studyMemberRow(m, study, canManage, reload) {
  const row = el('div', 'study-member');
  const who = el('div', 'person-who');
  const name = el('strong', null, m.nickname);
  who.append(name, el('span', 'muted small', [m.department, `${m.joined_at} 참여`].filter(Boolean).join(' · ')));
  row.append(el('span', 'avatar', m.nickname.slice(0, 1)), who);

  const tools = el('div', 'study-member-tools');
  if (m.is_owner) tools.append(el('span', 'tag owner', '방장'));

  if (canManage) {
    /* 역할 바꾸기 (방장 자신도 멘토로 정할 수 있다) */
    const sel = el('select');
    for (const [v, label] of Object.entries(ROLE_LABEL)) sel.append(new Option(label, v));
    sel.value = m.role;
    sel.setAttribute('aria-label', `${m.nickname} 역할`);
    sel.onchange = async () => {
      try {
        await api(`/api/studies/${study.id}/members/${m.user_id}`, {
          method: 'PUT', body: JSON.stringify({ role: sel.value }) });
        toast(`${m.nickname} 님을 ${ROLE_LABEL[sel.value]}(으)로 정했습니다.`);
        reload();
      } catch (e) { toast(e.message); sel.value = m.role; }
    };
    tools.append(sel);
    if (!m.is_owner) {
      const out = el('button', 'ghost small danger', '내보내기');
      out.onclick = async () => {
        if (!confirm(`${m.nickname} 님을 스터디에서 내보낼까요?`)) return;
        try {
          await api(`/api/studies/${study.id}/members/${m.user_id}`, { method: 'DELETE' });
          reload();
        } catch (e) { toast(e.message); }
      };
      tools.append(out);
    }
  } else if (m.role !== 'member') {
    tools.append(el('span', `tag ${m.role}`, ROLE_LABEL[m.role]));
  }
  row.append(tools);
  return row;
}

/* #/studies/{id} - 스터디 상세: 멤버(멘토·멘티·멤버 묶음), 참여/나가기, 방장의 멤버 추가 */
async function viewStudy(id) {
  markNav('studies');
  setPageTitle('그룹 스터디');

  let data;
  try { data = await api(`/api/studies/${id}`); }
  catch (e) { return renderMessage(e.message); }

  const { study: s, description, members, can_manage } = data;
  const reload = () => viewStudy(id);
  const node = tpl('tpl-study');
  const root = node.firstElementChild;
  const full = s.member_count >= s.max_members;

  const top = root.querySelector('[data-tags-top]');
  top.append(el('span', full ? 'tag plain' : 'tag ok', full ? '정원 마감' : '참여 가능'));
  if (s.my_role) top.append(el('span', 'tag owner', `참여 중 · ${ROLE_LABEL[s.my_role]}`));
  root.querySelector('[data-name]').textContent = s.name;
  root.querySelector('[data-owner]').textContent = s.owner_nickname;
  root.querySelector('[data-count]').textContent = `${s.member_count} / ${s.max_members}명`;
  root.querySelector('[data-mm]').textContent = `멘토 ${s.mentor_count} · 멘티 ${s.mentee_count}`;
  root.querySelector('[data-created]').textContent = s.created_at;
  root.querySelector('[data-description]').textContent = description || '소개가 없습니다.';

  /* 참여하기 / 나가기 / 삭제 */
  const actions = root.querySelector('[data-actions]');
  const isOwner = state.me && state.me.id === s.owner_id;
  if (!state.me) {
    const a = el('a', 'btn small', '로그인하고 참여하기');
    a.href = '#/login';
    actions.append(a);
  } else if (!s.my_role && !state.me.is_admin) {
    const join = el('button', 'small', full ? '정원이 찼습니다' : '참여하기');
    join.disabled = full;
    join.onclick = async () => {
      try {
        await api(`/api/studies/${id}/join`, { method: 'POST' });
        toast('스터디에 참여했습니다.');
        reload();
      } catch (e) { toast(e.message); }
    };
    actions.append(join);
  } else if (s.my_role && !isOwner) {
    const leave = el('button', 'ghost small', '스터디 나가기');
    leave.onclick = async () => {
      if (!confirm('이 스터디에서 나갈까요?')) return;
      try {
        await api(`/api/studies/${id}/members/${state.me.id}`, { method: 'DELETE' });
        toast('스터디에서 나왔습니다.');
        reload();
      } catch (e) { toast(e.message); }
    };
    actions.append(leave);
  }
  if (can_manage) {
    const del = el('button', 'ghost small danger', '스터디 삭제');
    del.onclick = async () => {
      if (!confirm('이 스터디를 지울까요? 멤버 목록도 함께 지워집니다.')) return;
      try {
        await api(`/api/studies/${id}`, { method: 'DELETE' });
        toast('지웠습니다.');
        location.hash = '#/studies';
      } catch (e) { toast(e.message); }
    };
    actions.append(del);
  }
  actions.hidden = !actions.childElementCount;

  /* 멤버: 멘토 → 멘티 → 멤버 묶음 (빈 묶음은 생략) */
  const memEl = root.querySelector('[data-members]');
  for (const role of ['mentor', 'mentee', 'member']) {
    const list = members.filter(m => m.role === role);
    if (!list.length) continue;
    const group = el('div');
    group.append(el('h3', 'study-group-title', `${ROLE_LABEL[role]} (${list.length})`),
                 ...list.map(m => studyMemberRow(m, s, can_manage, reload)));
    memEl.append(group);
  }

  /* 방장: 회원 찾아서 추가 */
  if (can_manage) {
    const box = root.querySelector('[data-add]');
    box.hidden = false;
    const form = box.querySelector('[data-search]');
    const results = box.querySelector('[data-results]');
    if (full) results.append(el('p', 'empty small', '정원이 찼습니다. 멤버를 더 넣으려면 누군가 나가야 합니다.'));
    form.onsubmit = async ev => {
      ev.preventDefault();
      const { q, role } = formValues(form);
      if (!q) return;
      try {
        const { users } = await api(`/api/studies/${id}/candidates?q=${encodeURIComponent(q)}`);
        results.replaceChildren(...(users.length ? users.map(u => {
          const r = el('div', 'mini-row');
          r.append(el('strong', null, u.nickname), el('span', 'muted', u.department || ''));
          const add = el('button', 'small', `${ROLE_LABEL[role]}(으)로 추가`);
          add.onclick = async () => {
            add.disabled = true;
            try {
              await api(`/api/studies/${id}/members`, {
                method: 'POST', body: JSON.stringify({ user_id: u.id, role }) });
              toast(`${u.nickname} 님을 추가했습니다.`);
              reload();
            } catch (e) { toast(e.message); add.disabled = false; }
          };
          r.append(add);
          return r;
        }) : [el('p', 'empty small', '찾는 회원이 없거나 이미 멤버입니다.')]));
      } catch (e) { toast(e.message); }
    };
  }

  render(node);
}

/* ══════════════════════════════════════════ 팀원 찾기 · 내 프로필 */

/* 잘 맞는 회원 카드: 아바타, 소속, 점수 게이지, 키워드(겹치는 것 강조), ♥ 관계, 자기소개 */
function personCard(m) {
  const c = el('article', 'person-card');
  const head = el('div', 'person-head');
  const av = el('span', 'avatar lg', m.nickname.slice(0, 1));
  const who = el('div', 'person-who');
  who.append(el('strong', null, m.nickname),
             el('span', 'muted small', [m.college_name, m.department_name].filter(Boolean).join(' · ')));
  head.append(av, who, scoreRing(m.score));
  c.append(head);

  /* 나와 겹치는 키워드는 .hit 로 강조 */
  const tags = el('div', 'tag-list');
  const shared = new Set(m.shared_tags);
  for (const t of m.tags) tags.append(el('span', shared.has(t) ? 'ttag hit' : 'ttag', t));
  c.append(tags);

  /* 서버(ml.c 의 explain_social)가 terms 맨 앞에 넣은 "♥" 관계 문구만 골라 보여 준다 */
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

/* #/people - 나와 성향이 비슷한 회원 (학생만) */
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
    /* 키워드가 없으면 비교할 근거가 없으므로 프로필 작성을 안내 */
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

/* #/me - 내 관심 키워드와 자기소개 편집 */
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
      /* 서버 모델은 다음 요청 때 데이터 변화를 감지해 자동으로 다시 학습한다 */
      toast('저장했습니다. 추천에 바로 반영됩니다.');
    } catch (e) { toast(e.message); }
  };
}

/* ══════════════════════════════════════════════════ AI 구조 소개 */

/* #/ai - 추천 모델 설명 페이지. 설명 글은 템플릿에 있고, 여기서는 현재 모델 상태 숫자만 채운다 */
async function viewAi() {
  markNav('ai');
  setPageTitle('AI 구조');
  const node = tpl('tpl-ai');
  const root = node.firstElementChild;
  const set = (k, v) => { root.querySelector(`[data-s="${k}"]`).textContent = v; };   /* data-s="키" 자리에 값 */
  render(node);

  try {
    const m = await api('/api/ml/info');
    set('mode', m.embedding_dim ? '딥러닝 임베딩 + TF-IDF' : 'TF-IDF 만 (임베딩 서비스 꺼짐)');
    /* 모델 이름에서 마지막 '/' 뒤만 (예: intfloat/multilingual-e5-small → multilingual-e5-small) */
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

/* 주소의 # 부분을 보고 알맞은 화면을 그린다.
 *   '#/posts/12' → slice(2) → 'posts/12' → split('/') → ['posts', '12']
 * 처음 접속(해시 없음)이면 '#/posts' 로 본다. 순서가 중요하다: 구체적인 경로를 먼저 검사한다. */
function route() {
  closeSidebar();   /* 모바일에서 메뉴를 누른 뒤 사이드바가 열린 채로 남지 않게 */
  const parts = (location.hash || '#/posts').slice(2).split('/');

  /* 히어로는 목록 화면에만 보인다. 상세·로그인·관리자 화면은 바로 본문부터. */
  const isList = !parts[0] || parts[0] === 'mine' || (parts[0] === 'posts' && !parts[1]);
  document.getElementById('hero').hidden = !isList;

  if (parts[0] === 'login') {
    if (state.me) return logout();   /* 사이드바의 "로그아웃" 메뉴가 #/login 을 가리킨다 */
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
  if (parts[0] === 'studies' && parts[1] === 'new') return viewStudyNew();
  if (parts[0] === 'studies' && parts[1]) return viewStudy(Number(parts[1]));
  if (parts[0] === 'studies') return viewStudies();
  if (parts[0] === 'people') return viewPeople();
  if (parts[0] === 'ai') return viewAi();
  if (parts[0] === 'me') return viewMe();
  if (parts[0] === 'posts' && parts[1]) return viewDetail(Number(parts[1]));
  return viewList(false);   /* 그 밖의 모든 주소는 목록으로 */
}

/* ════════════════════════════════════════════ 좁은 화면 사이드바 */

/* 모바일: 햄버거 버튼으로 사이드바를 열고, 뒤의 반투명 막(scrim)을 누르면 닫는다 */
function openSidebar() {
  document.getElementById('sidebar').classList.add('open');
  document.getElementById('scrim').hidden = false;
}

function closeSidebar() {
  document.getElementById('sidebar').classList.remove('open');
  document.getElementById('scrim').hidden = true;
}

/* ══════════════════════════════════════════════════════ 시작 */

/* 앱 시작: 기초 데이터 세 가지를 동시에 받아 state 를 채운 뒤 첫 화면을 그린다 */
async function boot() {
  document.getElementById('hamburger').onclick = openSidebar;
  document.getElementById('scrim').onclick = closeSidebar;

  try {
    /* Promise.all: 세 요청을 동시에 보내고 모두 끝날 때까지 기다린다 (차례로 보내는 것보다 빠름) */
    const [me, depts, tags] = await Promise.all([
      api('/api/me'),
      api('/api/departments'),
      api('/api/tags'),
    ]);
    state.tags = tags.groups;
    state.me = me.user;
    state.aiEnabled = !!me.ai_enabled;   /* !! 는 어떤 값이든 true/false 로 바꾼다 */
    state.colleges = depts.colleges;
  } catch (e) {
    renderMessage(`서버에 연결할 수 없습니다: ${e.message}`);
    return;
  }

  paintChrome();
  window.addEventListener('hashchange', route);   /* 이후 주소의 # 가 바뀔 때마다 route() */
  route();                                         /* 지금 주소로 첫 화면 */
}

boot();
