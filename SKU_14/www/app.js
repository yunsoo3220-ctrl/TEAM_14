/* 서경스터디 공모전 게시판 - 프런트엔드 (의존성 없음)
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
  unread: 0,        /* 안 읽은 알림 수 (상단 🔔 배지) */
  chatUnread: 0,    /* 채팅방 전체의 안 읽은 메시지 수 (사이드바 배지) */
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

/* 확인 창. 브라우저의 confirm() 은 브라우저·설정에 따라 막혀 있으면 묻지도 않고 "취소" 로 끝나므로
 * (예: 확인 창 차단, 일부 앱 안의 브라우저) 화면 안에 직접 그린다.
 * 사용: if (!(await askConfirm('지울까요?'))) return;   확인 = true, 취소·바깥 누르기·Esc = false */
function askConfirm(message, okLabel = '확인') {
  return new Promise(resolve => {
    const back = el('div', 'ask-back');
    const box = el('div', 'ask-box');
    box.setAttribute('role', 'alertdialog');
    box.setAttribute('aria-modal', 'true');
    const text = el('p', 'ask-text', message);
    const ok = el('button', null, okLabel);
    const no = el('button', 'ghost', '취소');
    const row = el('div', 'row ask-buttons');
    row.append(no, ok);
    box.append(text, row);
    back.append(box);
    const before = document.activeElement;
    const close = answer => {
      document.removeEventListener('keydown', onKey, true);
      back.remove();
      if (before && before.focus) before.focus();
      resolve(answer);
    };
    const onKey = ev => {
      if (ev.key === 'Escape') { ev.preventDefault(); close(false); }
      else if (ev.key === 'Tab') { ev.preventDefault(); (document.activeElement === ok ? no : ok).focus(); }   /* 두 버튼 안에서만 */
    };
    ok.onclick = () => close(true);
    no.onclick = () => close(false);
    back.onclick = ev => { if (ev.target === back) close(false); };
    document.addEventListener('keydown', onKey, true);
    document.body.append(back);
    ok.focus();
  });
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

/* 사이드바·상단바에서 현재 메뉴만 강조 (data-nav 값이 name 인 링크에 .on) */
function markNav(name) {
  for (const a of document.querySelectorAll('[data-nav]'))
    a.classList.toggle('on', a.dataset.nav === name);
}

/* 화면 위의 탭 줄 (예: 전체 / 내 학과 대상 / 학과별 추천). items = [[주소, 키, 이름], ...], current = 지금 탭의 키 */
function subTabs(nav, items, current) {
  for (const [hash, key, label] of items) {
    const a = el('a', key === current ? 'on' : null, label);
    a.href = hash;
    if (key === current) a.setAttribute('aria-current', 'page');
    nav.append(a);
  }
}

/* 사이드바를 줄이면서 화면 안의 탭으로 옮긴 메뉴들 (예전의 "내 학과 대상", "학과별 추천", "팀원 모집") */
const POST_TABS    = [['#/posts', 'all', '전체 공모전'], ['#/mine', 'mine', '내 학과 대상'], ['#/reco', 'reco', '학과별 추천']];
const STUDY_TABS   = [['#/studies', 'all', '전체 스터디'], ['#/studies/major', 'major', '내 학과 대상'],
                      ['#/studies/reco', 'reco', '학과별 추천']];
const BOOK_TABS    = [['#/books', 'all', '전체 책'], ['#/books/major', 'major', '내 학과 대상'], ['#/books/reco', 'reco', '학과별 추천']];
const RECRUIT_TABS = [['#/recruits', 'mine', '내가 쓴 모집글'], ['#/recruits/joined', 'joined', '참여 중인 팀'],
                      ['#/recruits/all', 'all', '모집글 둘러보기']];

/* 학과 번호 → 이름 (state.colleges 에서 찾는다, 없으면 '') */
function deptName(id) {
  for (const c of state.colleges)
    for (const d of c.departments) if (d.id === Number(id)) return d.name;
  return '';
}

/* 학과별 추천에서 볼 학과: 주소의 번호 > 내 학과 > 첫 번째 학과 */
function recoDept(param) {
  return Number(param) || (state.me && state.me.department_id)
    || (state.colleges[0] && state.colleges[0].departments[0].id);
}

/* 로그인 상태에 따라 화면 테두리(사이드바 메뉴, 상단 사용자 정보, 히어로)를 다시 그린다.
 * 로그인/로그아웃 직후와 처음 시작할 때 부른다. */
function paintChrome() {
  const me = state.me;

  /* 관리자 메뉴는 관리자에게만, 내 모집 관리·채팅방은 로그인한 사람에게만 */
  document.getElementById('nav-admin').hidden = !(me && me.is_admin);
  document.getElementById('nav-recruit').hidden = !me;
  document.getElementById('nav-chat').hidden = !me;

  /* 상단 오른쪽 */
  const who = document.getElementById('who');
  who.replaceChildren();

  if (!me) {
    const a = el('a', null, '로그인 / 가입');
    a.href = '#/login';
    who.append(a);
  } else {
    /* 알림함 🔔 (안 읽은 알림이 있으면 빨간 숫자). 사이드바에는 알림함 메뉴가 없고 여기서만 연다 */
    const bell = el('a', 'bell');
    bell.href = '#/notifications';
    bell.dataset.nav = 'notifications';   /* 알림함을 보는 동안 markNav 가 강조 */
    bell.title = '알림함';
    bell.setAttribute('aria-label', '알림함');
    bell.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 9a6 6 0 0112 0c0 5 2 6.5 2 6.5H4S6 14 6 9z"/><path d="M10 19a2 2 0 004 0"/></svg>';   /* 고정 아이콘이라 innerHTML 을 써도 안전 */
    bell.append(Object.assign(el('span', 'bell-badge'), { hidden: true }));
    who.append(bell);
    /* 내 이름을 누르면 내 프로필 (사이드바의 내 프로필 메뉴를 여기로 옮겼다) */
    const meLink = el('a', 'me-link');
    meLink.href = '#/me';
    meLink.dataset.nav = 'me';
    meLink.title = '내 프로필';
    const avatar = el('span', 'avatar', me.nickname.slice(0, 1));   /* 표시 이름 첫 글자 동그라미 */
    const box = el('span', 'whoami');
    box.append(
      el('b', null, `${me.nickname} 님`),
      el('small', null, me.is_admin ? '관리자' : (me.department_name || '소속 없음')),
    );
    meLink.append(avatar, box);
    const out = el('button', 'ghost small', '로그아웃');
    out.onclick = logout;
    who.append(meLink, out);
  }

  paintHero();
  paintUnread();
  paintSuspend();
}

/* 정지된 회원이면 화면 위에 기간·사유 안내 띠를 보인다 */
function paintSuspend() {
  const box = document.getElementById('suspend-banner');
  const me = state.me;
  box.hidden = !(me && me.suspended_until);
  if (box.hidden) return;
  box.replaceChildren(`⛔ 계정이 ${me.suspended_until} 까지 정지되었습니다. 글은 볼 수 있지만 글쓰기·댓글·참여는 할 수 없습니다.`,
                      el('small', null, `사유: ${me.suspend_reason || '-'}`));
}

/* 안 읽은 알림 수를 상단 🔔 배지에, 안 읽은 채팅 수를 사이드바 채팅방 배지에 그린다 (99 넘으면 99+) */
function paintUnread() {
  const n = state.me ? state.unread : 0;
  const text = n > 99 ? '99+' : String(n);
  for (const b of document.querySelectorAll('.bell-badge')) {
    b.hidden = !n;
    b.textContent = text;
  }
  const bell = document.querySelector('.bell');
  if (bell) bell.setAttribute('aria-label', n ? `알림함, 안 읽은 알림 ${n}개` : '알림함');
  /* 채팅방 배지 (안 읽은 메시지 수) */
  const c = state.me ? state.chatUnread : 0;
  for (const b of document.querySelectorAll('[data-chat-unread]')) {
    b.hidden = !c;
    b.textContent = c > 99 ? '99+' : String(c);
  }
}

/* 서버에서 안 읽은 알림 수를 다시 받아 온다. 화면을 옮길 때와 1분마다 부른다. */
async function refreshUnread() {
  if (!state.me) return;
  try {
    /* /api/me 는 안 읽은 알림 수와 함께 정지 상태도 알려 준다 (관리자가 방금 정지·해제했을 수 있다) */
    const me = await api('/api/me');
    if (!me.user) return;
    state.unread = me.unread_notifications || 0;
    state.chatUnread = me.unread_chats || 0;
    state.me.suspended_until = me.user.suspended_until;
    state.me.suspend_reason = me.user.suspend_reason;
    paintUnread();
    paintSuspend();
  } catch { /* 배지는 못 그려도 화면은 계속 쓸 수 있다 */ }
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
  state.unread = 0;
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

/* 누르면 onClick 을 부르는 타일 (공모전 목록의 빠른 필터). 고른 타일에는 on 클래스가 붙는다 */
function buttonTile(cls, value, label, note, onClick) {
  const n = tile(cls, value, label, note);
  const b = document.createElement('button');
  b.type = 'button';
  b.className = n.className;
  b.replaceChildren(...n.childNodes);
  b.onclick = onClick;
  return b;
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
  if (r.terms && r.terms.length) a.append(recoTerms(r.terms));
  return a;
}

/* 자체 모델이 찾은 근거 키워드 줄 (src/ml.c 의 explain 결과). 공모전·스터디·전공책 추천 카드가 함께 쓴다 */
function recoTerms(terms) {
  const why = el('div', 'reco-terms');
  why.append(el('span', 'spark', '✦'), el('span', 'muted', '겹친 키워드'));
  for (const t of terms) why.append(el('span', 'ttag hit', fmtTerm(t)));
  return why;
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

/* #/reco/{학과번호} - 공모전 목록의 "학과별 추천" 탭. 학과를 골라 자체 모델의 추천을 구간별로 본다. */
async function viewReco(deptParam) {
  markNav('posts');
  setPageTitle('학과별 추천 공모전');

  const node = tpl('tpl-reco');
  const root = node.firstElementChild;
  subTabs(root.querySelector('[data-tabs]'), POST_TABS, 'reco');
  const sel = root.querySelector('[data-reco-dept]');
  const groups = root.querySelector('[data-reco-groups]');
  const modelLine = root.querySelector('[data-model-line]');

  fillDeptSelect(sel);   /* 학과 선택 상자: 단과대학별 <optgroup> 으로 묶는다 */
  const mine = state.me && state.me.department_id;
  const dept = recoDept(deptParam);
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

/* #/posts (forceMine = false) 또는 #/mine (true) - 공모전 목록의 "전체 공모전" / "내 학과 대상" 탭 */
async function viewList(forceMine) {
  markNav('posts');
  setPageTitle(forceMine ? '내 학과 대상 공모전' : '공모전 목록');
  if (forceMine && !state.me)
    return renderTabNote(POST_TABS, 'mine', loginNote('로그인하면 내 학과가 참여할 수 있는 공모전만 모아 볼 수 있습니다.'));

  const node = tpl('tpl-list');
  const root = node.firstElementChild;
  subTabs(root.querySelector('[data-tabs]'), POST_TABS, forceMine ? 'mine' : 'all');
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

  /* 필터 두 가지: 학과, 종류 ("내 학과가 참여 가능한 것만" 은 내 학과 대상 탭이 맡는다) */
  const filters = {
    department: deptSel,
    kind: root.querySelector('[data-f="kind"]'),
  };

  /* 타일 빠른 필터: 누른 타일에 맞는 글만 목록에 남긴다 (학과·종류 필터와 함께 걸린다) */
  const isSoon = p => {                                  /* 마감 0~7일 남은 공모전 */
    const d = p.deadline ? daysLeft(p.deadline) : null;
    return d !== null && d >= 0 && d <= 7;
  };
  const QUICK = {
    all:       { title: '공모전',            test: () => true },
    forme:     { title: '내가 참여 가능',    test: p => p.can_comment },
    soon:      { title: '마감 7일 이내',     test: isSoon },
    hackathon: { title: '해커톤',            test: p => p.kind === 'hackathon' },
  };
  let quick = 'all';
  let loaded = null;                                     /* 마지막으로 불러온 글 (null = 불러오는 중·실패) */
  const headEl = countEl.parentElement;

  /* 불러온 글에 빠른 필터를 걸어 목록을 그린다 */
  function show() {
    headEl.firstChild.textContent = QUICK[quick].title + ' ';
    for (const t of tilesEl.children) t.classList.toggle('on', t.dataset.quick === quick);
    if (!loaded) return;
    const posts = loaded.filter(QUICK[quick].test);
    countEl.textContent = `(${posts.length})`;
    listEl.replaceChildren(...(posts.length
      ? posts.map(postItem)
      : [el('p', 'empty', '조건에 맞는 공모전이 없습니다.')]));
  }

  /* 현재 필터로 목록을 다시 불러온다 */
  async function reload() {
    /* URLSearchParams: 쿼리 문자열을 안전하게 만든다 (한글 등은 자동 인코딩) */
    const qs = new URLSearchParams();
    if (filters.department.value) qs.set('department_id', filters.department.value);
    if (filters.kind.value) qs.set('kind', filters.kind.value);
    if (forceMine) qs.set('mine', '1');

    loaded = null;
    listEl.replaceChildren(...skeleton(4));
    try {
      ({ posts: loaded } = await api(`/api/posts?${qs}`));   /* 구조 분해: 응답의 posts 만 꺼낸다 */
      show();
    } catch (e) {
      listEl.replaceChildren(el('p', 'empty', e.message));
    }
  }

  /* 타일을 누르면 그 조건으로 거르고 목록으로 내려간다. 고른 타일을 다시 누르면 전체로 돌아간다 */
  function pick(key) {
    quick = quick === key ? 'all' : key;
    show();
    listEl.closest('.card').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }
  const quickTile = (key, cls, value, label, note) => {
    const t = buttonTile(cls, value, label, note, () => pick(key));
    t.dataset.quick = key;
    return t;
  };

  for (const f of Object.values(filters)) f.onchange = reload;   /* 필터가 바뀔 때마다 다시 불러오기 */

  render(node);
  reload();
  if (!forceMine) mountReco(root.querySelector('[data-reco]'));

  /* 타일의 숫자는 필터와 무관한 전체 집계라 따로 한 번 불러온다. */
  try {
    const { posts } = await api('/api/posts');
    const soon = posts.filter(isSoon).length;
    const forMe = posts.filter(p => p.can_comment).length;

    tilesEl.replaceChildren(
      quickTile('all', 'c1', posts.length, '전체 공모전', '학교 공지에서 수집'),
      state.me
        ? quickTile('forme', 'c2', forMe, '내가 참여 가능', state.me.department_name || '관리자')
        : tile('c2', '—', '내가 참여 가능', '로그인이 필요합니다'),
      quickTile('soon', 'c3', soon, '마감 7일 이내', soon ? '서두르세요' : '임박한 공모전 없음'),
      quickTile('hackathon', 'c4', posts.filter(p => p.kind === 'hackathon').length, '해커톤', '개발·아이디어'),
    );
    show();
  } catch { /* 타일은 없어도 목록은 보여준다 */ }
}

/* ══════════════════════════════════════════════════ 게시물 상세 */

/* 댓글 한 줄. 내 댓글이거나 관리자면 [삭제] 버튼을 붙인다.
 * (버튼을 숨기는 것은 편의일 뿐이고, 실제 권한 검사는 서버가 한다)
 * badge = {userId, label} 을 주면 그 회원의 댓글에 이름을 강조하고 팻말을 단다 (예: 전공책 장터의 판매자) */
function commentRow(c, onDelete, badge) {
  const wrap = el('div', 'comment');
  const head = el('div', 'comment-head');

  if (badge && c.author_id === badge.userId) {
    head.append(el('strong', 'comment-author-mark', c.author_nickname), el('span', 'comment-badge', badge.label));
  } else {
    head.append(el('strong', null, c.author_nickname));
  }
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
  /* 전화번호 세 칸을 010-0000-0000 하나로 합친다 */
  const phoneEls = [...reg.querySelectorAll('[data-phone] input')];
  const phoneOf = () => phoneEls.map(p => p.value.trim()).join('-');
  const refreshPreview = () => {
    const v = formValues(reg);
    preview.textContent = displayName(v.name, phoneOf()) || '···';
  };
  reg.querySelector('[name="name"]').oninput = refreshPreview;    /* 한 글자 입력할 때마다 */
  /* 전화번호 칸: 숫자만 남기고, 칸이 차면 다음 칸으로 넘어간다 */
  phoneEls.forEach((p, i) => {
    p.oninput = () => {
      p.value = p.value.replace(/\D/g, '').slice(0, p.maxLength);
      if (p.value.length === p.maxLength && phoneEls[i + 1]) phoneEls[i + 1].focus();
      refreshPreview();
    };
  });

  /* 관심 키워드 (선택, 0~8개) - 고르면 팀원 추천과 개인 맞춤 공모전 추천에 쓰인다 */
  const getTags = interestPicker(reg.querySelector('[data-interest-picker]'), [],
                                 { countEl: reg.querySelector('[data-pick-count]') });

  /* 학교 이메일 인증
   * 흐름: [인증 코드 받기] → 메일로 6자리 → 코드 입력 후 [확인] → 이메일 잠금 → 나머지 입력 → [가입] */
  const emailEl = reg.querySelector('[name="email_id"]');
  /* 입력칸에는 아이디만 받는다. 습관적으로 전체 주소를 붙여 넣어도 @ 뒤는 버린다 */
  const emailId = () => emailEl.value.trim().toLowerCase().split('@')[0];
  const emailOf = () => `${emailId()}@skuniv.ac.kr`;
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
    const email = emailOf();
    emailEl.value = emailId();
    if (!SCHOOL_EMAIL.test(email)) {
      say('학교 이메일 아이디를 입력하세요. (영문 소문자·숫자·. _ - 만 쓸 수 있습니다)', 'blocked');
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
    if (sentTo && emailOf() !== sentTo) {
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
      reg.querySelector('[name="name"]').focus();
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

  /* 비밀번호 확인: 두 칸이 같은지 입력할 때마다 라벨 옆에 표시 */
  const pwEl = reg.querySelector('[name="password"]');
  const pw2El = reg.querySelector('[name="password_confirm"]');
  const pwMatch = reg.querySelector('[data-pw-match]');
  const refreshPwMatch = () => {
    const same = pwEl.value === pw2El.value;
    pwMatch.textContent = !pw2El.value ? '' : same ? '일치합니다' : '일치하지 않습니다';
    pwMatch.className = same ? 'pw-ok' : 'pw-bad';
  };
  pwEl.oninput = pw2El.oninput = refreshPwMatch;

  /* [가입] 제출: 화면에서 한 번 더 확인한 뒤 서버로 (최종 검사는 서버가 한다) */
  reg.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(ev.target);
    v.email = emailOf();
    delete v.email_id;
    if (!SCHOOL_EMAIL.test(v.email)) return toast('학교 이메일 아이디를 입력하세요.');
    if (!sentTo) return toast('먼저 인증 코드 받기를 누르세요.');
    if (!verified) return toast('인증 코드를 입력하고 [확인]을 눌러 이메일 인증을 마치세요.');
    delete v.code;   /* 이미 [확인] 으로 인증했으므로 코드는 보내지 않는다 */
    delete v.phone1; delete v.phone2; delete v.phone3;
    v.phone = phoneEls.every(p => p.value) ? phoneOf() : '';   /* 한 칸이라도 비면 아래에서 '입력하세요' */
    if (v.phone && !/^\d{2,3}-\d{3,4}-\d{4}$/.test(v.phone)) {
      toast('전화번호를 끝까지 입력하세요.');
      return phoneEls.find(p => p.value.length < p.maxLength)?.focus();
    }
    for (const [k, label] of [['name', '이름'], ['student_no', '학번'], ['phone', '전화번호'],
                              ['password', '비밀번호'], ['department_id', '소속 학과']])
      if (!v[k]) return toast(`${label}을(를) 입력하세요.`);
    if (v.password !== v.password_confirm) {
      toast('비밀번호 확인이 비밀번호와 다릅니다.');
      return pw2El.focus();
    }
    delete v.password_confirm;   /* 서버에는 비밀번호 하나만 보낸다 */
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
  adminTabs(root.querySelector('[data-tabs]'), 'admin');
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
  aiForce.onclick = async () => {
    if (await askConfirm('모든 게시물을 다시 분석합니다. API 사용량이 발생합니다. 계속할까요?')) startAi(true);
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

/* 로그인 안내 + 로그인 버튼 상자 */
function loginNote(message) {
  const box = el('div', 'empty');
  box.append(message, el('br'));
  const a = el('a', 'btn small', '로그인 · 가입');
  a.href = '#/login';
  box.append(a);
  return box;
}

/* 로그인이 필요한 화면 대신 보여 줄 안내 + 로그인 버튼 */
function requireLogin(message) {
  render(loginNote(message));
}

/* 탭 화면에서 지금 탭을 볼 수 없을 때: 탭 줄은 그대로 두고 그 아래에 안내(note)만 그린다 */
function renderTabNote(tabs, current, note) {
  const nav = el('nav', 'sub-tabs');
  subTabs(nav, tabs, current);
  const wrap = el('div', 'stack');
  wrap.append(nav, note);
  render(wrap);
}

/* "내 학과 대상" 탭을 볼 수 있는지 (로그인 + 소속 학과). 볼 수 없으면 안내를 그리고 false */
function needMyDept(tabs, current, loginMessage) {
  if (!state.me) { renderTabNote(tabs, current, loginNote(loginMessage)); return false; }
  if (!state.me.department_id) {
    renderTabNote(tabs, current, el('p', 'empty', '소속 학과가 있는 회원만 볼 수 있습니다.'));
    return false;
  }
  return true;
}

/* ══════════════════════════════════════════════ 내 모집 관리 (팀원 모집글) */

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
  const tools = el('div', 'foot-tools');
  if (r.in_team) tools.append(chatCardButton(`#/chats/recruit/${r.id}`));
  tools.append(likeButton(r));
  foot.append(meta, tools);
  a.append(foot);
  return a;
}

/* 내가 쓴 모집글 카드: 목록 카드 아래에 팀 인원과 관리 버튼(팀원 찾기 · 모집 마감/다시 모집)을 붙인다.
 * 카드 전체가 링크라서 버튼은 누를 때 카드로 이동하지 않게 막는다 (chatCardButton 과 같은 방식) */
function myRecruitCard(r, reload) {
  const a = recruitCard(r);
  const button = (text, cls, onClick) => {
    const b = el('button', cls, text);
    b.type = 'button';
    b.onclick = ev => { ev.preventDefault(); ev.stopPropagation(); onClick(b); };
    return b;
  };
  const tools = el('div', 'foot-tools');
  if (r.status === 'open')
    tools.append(button('팀원 찾기', 'small', () => { location.hash = `#/people/recruit/${r.id}`; }));
  tools.append(button(r.status === 'open' ? '모집 마감' : '다시 모집', 'ghost small', async b => {
    b.disabled = true;
    try {
      await api(`/api/recruits/${r.id}/status`, { method: 'PUT',
        body: JSON.stringify({ status: r.status === 'open' ? 'closed' : 'open' }) });
      toast(r.status === 'open' ? '모집을 마감했습니다.' : '다시 모집합니다.');
      reload();
    } catch (e) { toast(e.message); b.disabled = false; }
  }));
  const bar = el('div', 'manage-bar');
  bar.append(el('span', 'muted small', `👥 팀 ${r.team_count}명 (글쓴이 포함)`), tools);
  a.append(bar);
  return a;
}

/* 내 모집 관리 탭마다 제목 · 설명 · 비었을 때 문구 */
const RECRUIT_TAB_TEXT = {
  mine:   { heading: '내가 쓴 모집글',
            sub: '댓글을 남긴 회원을 팀원으로 받고, 모집을 마감하거나 다시 열 수 있습니다. 공모전 화면의 [이 공모전으로 팀원 모집] 으로도 쓸 수 있습니다.',
            empty: '아직 쓴 모집글이 없습니다. 함께 나갈 공모전을 골라 첫 모집글을 써 보세요.' },
  joined: { heading: '참여 중인 팀',
            sub: '글쓴이가 팀원으로 받아 준 모집글입니다. 팀 채팅방으로 바로 갈 수 있습니다.',
            empty: '아직 참여 중인 팀이 없습니다. 모집글 둘러보기에서 마음에 드는 팀에 댓글을 남겨 보세요.' },
  all:    { heading: '모집글 둘러보기',
            sub: '다른 회원이 올린 모집글입니다. 댓글로 참여 의사를 남기면 글쓴이가 팀원으로 받아 줍니다.',
            empty: '조건에 맞는 모집글이 없습니다. 첫 모집글을 써 보세요.' },
};

/* #/recruits (내가 쓴 모집글), #/recruits/joined (참여 중인 팀), #/recruits/all (모집글 둘러보기)
 * 앞의 두 탭은 /api/recruits?mine=1 (내가 쓰거나 팀원인 글, 마감한 글도) 을 글쓴이 여부로 나눈다.
 * 둘러보기 탭만 키워드·상태 필터와 최신순/나와 맞는 순 정렬이 있다. */
async function viewRecruits(tab = 'mine') {
  markNav('recruits');
  setPageTitle('내 모집 관리');
  if (tab !== 'all' && !state.me)
    return renderTabNote(RECRUIT_TABS, tab, loginNote('로그인하면 내가 쓴 모집글과 참여 중인 팀을 관리할 수 있습니다.'));

  const node = tpl('tpl-recruits');
  const root = node.firstElementChild;
  subTabs(root.querySelector('[data-tabs]'), RECRUIT_TABS, tab);
  const text = RECRUIT_TAB_TEXT[tab];
  root.querySelector('[data-heading]').textContent = text.heading;
  root.querySelector('[data-sub]').textContent = text.sub;
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const f = {
    tag: root.querySelector('[data-f="tag"]'),
    status: root.querySelector('[data-f="status"]'),
    sort: root.querySelector('[data-f="sort"]'),
  };
  const filtersEl = root.querySelector('[data-filters]');
  if (tab === 'all') filtersEl.hidden = false;
  else filtersEl.remove();   /* 내 글·내 팀 탭에는 필터가 없다 (f 의 요소는 떼어 낸 채로 남아 있어도 된다) */
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
    const qs = tab === 'all'
      ? new URLSearchParams({ status: f.status.value, sort: f.sort.value })
      : new URLSearchParams({ status: 'all', mine: '1' });
    if (tab === 'all' && f.tag.value) qs.set('tag', f.tag.value);
    listEl.replaceChildren(...skeleton(4, 'sk-reco'));
    try {
      const r = await api(`/api/recruits?${qs}`);
      const items = tab === 'all' ? r.recruits
        : r.recruits.filter(x => (x.author_id === state.me.id) === (tab === 'mine'));
      countEl.textContent = `(${items.length})`;
      listEl.replaceChildren(...(items.length
        ? items.map(x => (tab === 'mine' ? myRecruitCard(x, reload) : recruitCard(x)))
        : [el('p', 'empty', text.empty)]));
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
  let team = data.team || [];
  const isAuthor = state.me && state.me.id === r.author_id;
  const node = tpl('tpl-recruit');
  const root = node.firstElementChild;

  /* 돌아갈 곳: 내 글 → 내 모집 관리, 내 팀 → 참여 중인 팀, 남의 글 → 연결된 공모전 (없으면 둘러보기) */
  const back = root.querySelector('[data-back]');
  [back.href, back.textContent] = isAuthor ? ['#/recruits', '← 내 모집 관리']
    : r.in_team ? ['#/recruits/joined', '← 참여 중인 팀']
    : r.post_id ? [`#/posts/${r.post_id}`, '← 공모전으로']
    : ['#/recruits/all', '← 모집글 둘러보기'];
  if (!r.in_team && r.post_id) markNav('posts');   /* 공모전 화면에서 들어온 남의 모집글 */

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
    /* 팀원 찾기 (글쓴이만, 모집 중일 때) - 관리자가 남의 글을 볼 때는 보이지 않는다 */
    const find = ownerRow.querySelector('[data-find]');
    find.hidden = !(state.me && state.me.id === r.author_id && r.status === 'open');
    find.href = `#/people/recruit/${id}`;
    if (isAuthor) {   /* 글쓴이는 언제나 팀이다 */
      const go = el('a', 'btn small', '💬 채팅방으로 이동');
      go.href = `#/chats/recruit/${id}`;
      ownerRow.prepend(go);
    }
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
      if (!(await askConfirm('이 모집글을 지울까요? 댓글도 함께 지워집니다.'))) return;
      try {
        await api(`/api/recruits/${id}`, { method: 'DELETE' });
        toast('지웠습니다.');
        location.hash = '#/recruits';
      } catch (e) { toast(e.message); }
    };
  }

  /* 팀: 글쓴이 + 글쓴이가 받은 팀원. 팀은 팀 채팅방에 들어갈 수 있다 */
  const teamActions = root.querySelector('[data-team-actions]');
  const teamList = root.querySelector('[data-team-list]');
  const inTeam = () => !!state.me && team.some(m => m.user_id === state.me.id);
  const paintTeam = () => {
    root.querySelector('[data-team-count]').textContent = `(${team.length}명)`;
    teamList.replaceChildren(...team.map(m => {
      const row = el('div', 'study-member');
      const who = el('div', 'person-who');
      who.append(el('strong', null, m.nickname),
                 el('span', 'muted small', [m.department, m.is_author ? '글쓴이' : `${m.joined_at} 합류`].filter(Boolean).join(' · ')));
      row.append(el('span', 'avatar', m.nickname.slice(0, 1)), who);
      const tools = el('div', 'study-member-tools');
      if (m.is_author) tools.append(el('span', 'tag owner', '글쓴이'));
      else if (can_edit) {
        const out = el('button', 'ghost small danger', '내보내기');
        out.onclick = async () => {
          if (!(await askConfirm(`${m.nickname} 님을 팀에서 내보낼까요? 팀 채팅방에도 더는 들어갈 수 없습니다.`))) return;
          try {
            await api(`/api/recruits/${id}/members/${m.user_id}`, { method: 'DELETE' });
            refresh();
          } catch (e) { toast(e.message); }
        };
        tools.append(out);
      }
      row.append(tools);
      return row;
    }));

    /* 팀원: 팀 채팅방 / 팀 나가기 (글쓴이의 채팅방 버튼은 글쓴이 버튼 줄에 따로 둔다) */
    teamActions.replaceChildren();
    if (inTeam() && !isAuthor) {
      const go = el('a', 'btn small', '💬 채팅방으로 이동');
      go.href = `#/chats/recruit/${id}`;
      const leave = el('button', 'ghost small', '팀 나가기');
      leave.onclick = async () => {
        if (!(await askConfirm('이 팀에서 나갈까요? 팀 채팅방에도 더는 들어갈 수 없습니다.'))) return;
        try {
          await api(`/api/recruits/${id}/members/${state.me.id}`, { method: 'DELETE' });
          toast('팀에서 나왔습니다.');
          refresh();
        } catch (e) { toast(e.message); }
      };
      teamActions.append(go, leave);
    }
    teamActions.hidden = !teamActions.childElementCount;
    root.querySelector('[data-team-hint]').textContent =
      can_edit ? '댓글을 남긴 회원을 [팀원으로 받기] 하면 팀이 되어 팀 채팅방에 함께 들어갑니다.'
      : inTeam() ? '팀원끼리는 팀 채팅방에서 이야기할 수 있습니다.'
      : '글쓴이가 팀원으로 받아 주면 팀 채팅방에 들어갈 수 있습니다. 댓글로 참여 의사를 남겨 보세요.';
  };

  /* 댓글 목록 (게시물 상세의 commentRow 를 재사용, 삭제 API 만 다르다)
   * 글쓴이·관리자에게는 아직 팀이 아닌 댓글 작성자 옆에 [팀원으로 받기] 를 붙인다 */
  const listEl = root.querySelector('[data-comments]');
  const countEl = root.querySelector('[data-count]');
  const paint = items => {
    countEl.textContent = `(${items.length})`;
    const teamIds = new Set(team.map(m => m.user_id));
    listEl.replaceChildren(...(items.length
      ? items.map(c => {
          const row = commentRow(c, async cid => {
            try {
              await api(`/api/recruit-comments/${cid}`, { method: 'DELETE' });
              refresh();
            } catch (e) { toast(e.message); }
          });
          if (can_edit && !teamIds.has(c.author_id)) {
            const add = el('button', 'small', '팀원으로 받기');
            add.onclick = async () => {
              add.disabled = true;
              try {
                await api(`/api/recruits/${id}/members`, { method: 'POST', body: JSON.stringify({ user_id: c.author_id }) });
                toast(`${c.author_nickname} 님을 팀원으로 받았습니다. 알림이 갔습니다.`);
                refresh();
              } catch (e) { toast(e.message); add.disabled = false; }
            };
            const head = row.querySelector('.comment-head');
            head.append(add);
            head.classList.add('has-tools');
          }
          return row;
        })
      : [el('p', 'empty', '아직 댓글이 없습니다. 참여 의사를 남겨 보세요.')]));
  };
  /* 팀·댓글만 다시 받아 그린다 (화면 전체를 다시 그리면 맨 위로 올라가므로) */
  const refresh = async () => {
    try {
      const d = await api(`/api/recruits/${id}`);
      team = d.team || [];
      paintTeam();
      paint(d.comments);
    } catch (e) { toast(e.message); }
  };
  paintTeam();
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
      refresh();
    } catch (e) { toast(e.message); }
  };

  render(node);
}

/* ══════════════════════════════════════════════════ 그룹 스터디 */

const ROLE_LABEL = { mentor: '멘토', mentee: '멘티', member: '멤버' };

/* 그룹 스터디 화면의 글자·주소
 *   path: 주소(#/{path}), tpl/list: 상세·목록 템플릿 이름, title: 메뉴 이름, n: 이름, eul/ga: 조사 붙은 이름
 * (예전의 멘토링 메뉴는 없앴다. 스터디에서 멘토·멘티를 정할 수 있다) */
const STUDY = { path: 'studies', tpl: 'study', list: 'studies', title: '그룹 스터디', n: '스터디', eul: '스터디를', ga: '스터디가' };

/* 스터디 목록 카드. 학과별 추천 탭에서는 관련도 게이지(score)와 겹친 키워드(terms)도 보인다 */
function studyCard(s) {
  const full = s.member_count >= s.max_members;
  const a = el('a', 'recruit-card' + (full && !s.my_role ? ' closed' : ''));
  a.href = `#/${STUDY.path}/${s.id}`;

  const top = el('div', 'reco-card-head');
  const tags = el('div', 'item-top');
  tags.append(el('span', full ? 'tag plain' : 'tag ok', `${s.member_count} / ${s.max_members}명`));
  if (s.mentor_count) tags.append(el('span', 'tag mentor', `멘토 ${s.mentor_count}`));
  if (s.mentee_count) tags.append(el('span', 'tag mentee', `멘티 ${s.mentee_count}`));
  if (s.my_role) tags.append(el('span', 'tag owner',
    state.me && state.me.id === s.owner_id ? '내가 방장' : `참여 중 · ${ROLE_LABEL[s.my_role]}`));
  top.append(tags);
  if (typeof s.score === 'number') top.append(scoreRing(s.score));

  a.append(top, el('h3', null, s.name));
  if (s.excerpt) a.append(el('p', 'item-summary excerpt', s.excerpt));
  if (s.terms && s.terms.length) a.append(recoTerms(s.terms));
  const meta = el('div', 'item-meta');
  meta.append(el('span', null, `방장 ${s.owner_nickname}`));
  if (s.department) meta.append(el('span', null, `${s.department} 대상`));
  meta.append(el('span', null, s.created_at.slice(0, 10)));
  if (s.end_date) meta.append(el('span', null, `~ ${s.end_date} 종료`));
  const foot = el('div', 'recruit-foot');
  foot.append(meta);
  if (s.my_role) foot.append(chatCardButton(`#/chats/study/${s.id}`));   /* 내가 속한 모임이면 채팅방 */
  a.append(foot);
  return a;
}

/* 스터디 목록. 탭(tab)마다 서버에 묻는 조건이 다르다.
 *   all   #/studies              전체 (보기: 전체 / 내가 속한 스터디)
 *   major #/studies/major        내 학과를 대상 학과로 정한 스터디
 *   reco  #/studies/reco/{학과}  자체 모델이 이름·소개를 학과 전공 분야와 비교해 관련 있는 순으로 */
async function viewStudies(tab = 'all', deptParam) {
  markNav(STUDY.path);
  setPageTitle(tab === 'major' ? '내 학과 대상 스터디' : tab === 'reco' ? '학과별 추천 스터디' : STUDY.title);
  if (tab === 'major' && !needMyDept(STUDY_TABS, 'major', '로그인하면 내 학과 대상 스터디를 모아 볼 수 있습니다.')) return;

  const node = tpl(`tpl-${STUDY.list}`);
  const root = node.firstElementChild;
  subTabs(root.querySelector('[data-tabs]'), STUDY_TABS, tab);
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const heading = root.querySelector('[data-heading]');
  const sub = root.querySelector('[data-sub]');
  const mineSel = root.querySelector('[data-f="mine"]');
  const deptSel = root.querySelector('[data-f="dept"]');
  /* 지금 탭에 쓰지 않는 필터 줄은 떼어 낸다 (mineSel 은 떼어 낸 뒤에도 값이 '' 로 남는다) */
  if (tab !== 'all') root.querySelector('[data-filter-all]').remove();
  if (tab === 'reco') root.querySelector('[data-filter-reco]').hidden = false;
  else root.querySelector('[data-filter-reco]').remove();
  if (!state.me) {
    root.querySelector('[data-new]').href = '#/login';
    mineSel.querySelector('[value="1"]').disabled = true;
  }

  const qs = new URLSearchParams();
  let empty = `아직 ${STUDY.ga} 없습니다. 첫 ${STUDY.eul} 만들어 보세요.`;
  if (tab === 'major') {
    qs.set('dept', state.me.department_id);
    heading.textContent = `${state.me.department_name} 대상 스터디`;
    sub.textContent = '스터디를 만들 때 대상 학과를 내 학과로 정한 스터디입니다.';
    empty = `아직 내 학과 대상 ${STUDY.ga} 없습니다. 첫 ${STUDY.eul} 만들어 보세요.`;
  } else if (tab === 'reco') {
    const dept = recoDept(deptParam);
    fillDeptSelect(deptSel);
    deptSel.value = String(dept);
    deptSel.onchange = () => { location.hash = `#/${STUDY.path}/reco/${deptSel.value}`; };
    qs.set('reco', dept);
    heading.textContent = `${deptName(dept)} 추천 스터디`;
    sub.textContent = '자체 AI 모델이 스터디 이름·소개·대상 학과를 학과의 전공 분야와 비교해 관련 있는 순서로 보여 줍니다.';
    empty = `이 학과와 관련 있다고 판단한 ${STUDY.ga} 아직 없습니다.`;
  }
  sub.hidden = !sub.textContent;

  async function reload() {
    listEl.replaceChildren(...skeleton(4, 'sk-reco'));
    if (mineSel.value) qs.set('mine', '1');
    else qs.delete('mine');
    try {
      const r = await api(`/api/studies?${qs}`);
      countEl.textContent = `(${r.studies.length})`;
      listEl.replaceChildren(...(r.studies.length
        ? r.studies.map(studyCard)
        : [el('p', 'empty', mineSel.value ? `아직 참여한 ${STUDY.ga} 없습니다.` : empty)]));
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
  markNav(STUDY.path);
  setPageTitle(`${STUDY.n} 만들기`);
  if (!state.me) return requireLogin(`${STUDY.eul} 만들려면 로그인하세요.`);

  const node = tpl(`tpl-${STUDY.tpl}-new`);
  const form = node.querySelector('[data-form]');
  const deptSel = form.querySelector('[data-dept]');
  fillDeptSelect(deptSel);
  if (state.me.department_id) deptSel.value = String(state.me.department_id);   /* 기본은 내 학과 (전공책 판매와 같다) */
  const now = new Date();   /* 종료일은 오늘부터 고를 수 있다 (지난 날짜면 바로 지워지므로) */
  form.elements.end_date.min = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}`;
  /* 아이콘뿐 아니라 칸 어디를 눌러도 달력이 펼쳐지게 */
  form.elements.end_date.addEventListener('click', ev => { try { ev.currentTarget.showPicker(); } catch (e) {} });
  form.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(form);
    const btn = form.querySelector('button[type="submit"]');
    btn.disabled = true;
    try {
      const out = await api('/api/studies', {
        method: 'POST',
        body: JSON.stringify({ name: v.name, description: v.description, max_members: Number(v.max_members || 6),
                               end_date: v.end_date || null, department_id: Number(v.department_id) || null,
                               role: v.role }),
      });
      toast(`${STUDY.eul} 만들었습니다. 멤버를 추가해 보세요.`);
      location.hash = `#/${STUDY.path}/${out.id}`;
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
        if (!(await askConfirm(`${m.nickname} 님을 ${STUDY.n}에서 내보낼까요?`))) return;
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

/* #/studies/{id} - 상세: 멤버(멘토·멘티·멤버 묶음), 참여/나가기, 방장의 멤버 추가 */
async function viewStudy(id) {
  markNav(STUDY.path);
  setPageTitle(STUDY.title);

  let data;
  try { data = await api(`/api/studies/${id}`); }
  catch (e) { return renderMessage(e.message); }

  const { study: s, description, members, can_manage } = data;
  const reload = () => viewStudy(id);
  const node = tpl(`tpl-${STUDY.tpl}`);
  const root = node.firstElementChild;
  const full = s.member_count >= s.max_members;

  const top = root.querySelector('[data-tags-top]');
  top.append(el('span', full ? 'tag plain' : 'tag ok', full ? '정원 마감' : '참여 가능'));
  if (s.my_role) top.append(el('span', 'tag owner', `참여 중 · ${ROLE_LABEL[s.my_role]}`));
  root.querySelector('[data-name]').textContent = s.name;
  root.querySelector('[data-owner]').textContent = s.owner_nickname;
  root.querySelector('[data-count]').textContent = `${s.member_count} / ${s.max_members}명`;
  root.querySelector('[data-mm]').textContent = `멘토 ${s.mentor_count} · 멘티 ${s.mentee_count}`;
  root.querySelector('[data-dept]').textContent = s.department || '학과 무관';
  root.querySelector('[data-created]').textContent = s.created_at;
  root.querySelector('[data-end]').textContent = s.end_date ? `${s.end_date} (지나면 자동 삭제)` : '정하지 않음';
  root.querySelector('[data-description]').textContent = description || '소개가 없습니다.';

  /* 채팅방 / 참여하기 / 나가기 / 삭제 */
  const actions = root.querySelector('[data-actions]');
  const isOwner = state.me && state.me.id === s.owner_id;
  if (s.my_role) {
    const go = el('a', 'btn small', '💬 채팅방으로 이동');
    go.href = `#/chats/study/${id}`;
    actions.append(go);
  }
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
        toast(`${STUDY.n}에 참여했습니다.`);
        reload();
      } catch (e) { toast(e.message); }
    };
    actions.append(join);
  } else if (s.my_role && !isOwner) {
    const leave = el('button', 'ghost small', `${STUDY.n} 나가기`);
    leave.onclick = async () => {
      if (!(await askConfirm(`이 ${STUDY.n}에서 나갈까요?`))) return;
      try {
        await api(`/api/studies/${id}/members/${state.me.id}`, { method: 'DELETE' });
        toast(`${STUDY.n}에서 나왔습니다.`);
        reload();
      } catch (e) { toast(e.message); }
    };
    actions.append(leave);
  }
  if (isOwner && !full) {
    const find = el('a', 'btn small', '팀원 찾기');
    find.href = `#/people/study/${id}`;
    actions.append(find);
  }
  if (can_manage) {
    const del = el('button', 'ghost small danger', `${STUDY.n} 삭제`);
    del.onclick = async () => {
      if (!(await askConfirm(`이 ${STUDY.eul} 지울까요? 멤버 목록도 함께 지워집니다.`))) return;
      try {
        await api(`/api/studies/${id}`, { method: 'DELETE' });
        toast('지웠습니다.');
        location.hash = `#/${STUDY.path}`;
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

/* ═══════════════════════════════════════ 관리자 · 회원 관리 · 전체 게시물 */

/* 관리자 화면 위의 탭 (운영 / 회원 관리 / 전체 게시물) */
function adminTabs(nav, current) {
  subTabs(nav, [['#/admin', 'admin', '운영'], ['#/admin/users', 'users', '회원 관리'],
                ['#/admin/content', 'content', '전체 게시물']], current);
}

/* 관리자가 회원 알림함으로 메시지를 보내는 작은 입력칸. row 안에 열었다 닫는다.
 * preset = {title, link} 를 주면 미리 채운다 (게시물 주인에게 보낼 때). 알림 종류는 서버에서 "admin". */
function adminNotifyForm(row, user, preset = {}) {
  row.querySelector('.admin-suspend')?.remove();   /* 정지 입력칸이 열려 있으면 닫고 */
  const old = row.querySelector('.admin-notify');
  if (old) { old.remove(); return; }   /* 다시 누르면 닫는다 */

  const form = el('form', 'admin-notify stack-sm');
  const title = el('input');
  title.name = 'title';
  title.maxLength = 100;
  title.required = true;
  title.value = preset.title || '';
  title.placeholder = '제목 (예: 게시물 내용을 확인해 주세요)';
  const body = el('textarea');
  body.name = 'body';
  body.rows = 3;
  body.maxLength = 1000;
  body.placeholder = '내용 (선택)';
  const tl = el('label'); tl.append(el('span', null, `${user.nickname} 님에게 보낼 알림`), title);
  const bl = el('label'); bl.append(el('span', null, '내용'), body);
  const send = el('button', 'small', '알림 보내기');
  const cancel = el('button', 'ghost small', '취소');
  cancel.type = 'button';
  cancel.onclick = () => form.remove();
  const btns = el('div', 'row');
  btns.append(send, cancel);
  if (preset.link) btns.append(el('span', 'muted small', `누르면 이 글로 이동합니다 (${preset.link})`));
  form.append(tl, bl, btns);
  form.onsubmit = async ev => {
    ev.preventDefault();
    send.disabled = true;
    try {
      await api('/api/admin/notify', { method: 'POST', body: JSON.stringify({
        user_id: user.id, title: title.value.trim(), body: body.value.trim(), link: preset.link || '' }) });
      toast(`${user.nickname} 님의 알림함으로 보냈습니다.`);
      form.remove();
    } catch (e) { toast(e.message); send.disabled = false; }
  };
  row.append(form);
  title.focus();
}

/* #/admin/users - 모든 회원 보기·찾기, 회원 추가·삭제, 알림 보내기 */
async function viewAdminUsers() {
  markNav('admin');
  setPageTitle('관리자 · 회원 관리');
  if (!state.me || !state.me.is_admin) return renderMessage('관리자만 볼 수 있습니다.');

  const node = tpl('tpl-admin-users');
  const root = node.firstElementChild;
  adminTabs(root.querySelector('[data-tabs]'), 'users');
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const q = root.querySelector('[data-q]');
  const addForm = root.querySelector('[data-add]');
  fillDeptSelect(addForm.querySelector('[data-dept]'));
  root.querySelector('[data-toggle-add]').onclick = () => {
    addForm.hidden = !addForm.hidden;
    if (!addForm.hidden) addForm.elements.student_no.focus();
  };
  addForm.querySelector('[data-cancel]').onclick = () => { addForm.hidden = true; addForm.reset(); };
  addForm.onsubmit = async ev => {
    ev.preventDefault();
    const v = formValues(addForm);
    try {
      const out = await api('/api/admin/users', { method: 'POST', body: JSON.stringify({
        ...v, department_id: Number(v.department_id) || 0 }) });
      toast(`${out.nickname} 님을 추가했습니다.`);
      addForm.reset();
      addForm.hidden = true;
      reload();
    } catch (e) { toast(e.message); }
  };

  function row(u) {
    const r = el('div', 'admin-row');
    const main = el('div', 'admin-row-main');
    const who = el('div', 'admin-who');
    const name = el('strong', null, `${u.nickname} `);
    if (u.role === 'admin') name.append(el('span', 'tag admin', '관리자'));
    if (u.suspended_until) name.append(el('span', 'tag suspended', `정지 중 · ${u.suspended_until} 까지`));
    who.append(name,
      el('span', null, [`${u.name} · ${u.student_no}`, u.department, u.email].filter(Boolean).join(' · ')),
      el('span', null, `가입 ${u.created_at} · 모집글 ${u.recruits} · 스터디 ${u.studies} · 전공책 ${u.books}`));
    if (u.suspended_until) who.append(el('span', null, `정지 사유: ${u.suspend_reason || '-'}`));
    const tools = el('div', 'admin-tools');
    if (u.id !== state.me.id) {
      const notify = el('button', 'ghost small', '📢 알림 보내기');
      notify.onclick = () => adminNotifyForm(r, u);
      tools.append(notify);
    }
    if (u.role === 'student' && u.suspended_until) {
      const lift = el('button', 'ghost small', '🔓 정지 풀기');
      lift.onclick = async () => {
        if (!(await askConfirm(`${u.nickname} 님의 정지를 지금 풀까요?`))) return;
        try {
          await api(`/api/admin/users/${u.id}/suspend`, { method: 'DELETE' });
          toast('정지를 풀었습니다. 회원에게 알림이 갔습니다.');
          reload();
        } catch (e) { toast(e.message); }
      };
      tools.append(lift);
    } else if (u.role === 'student') {
      const ban = el('button', 'ghost small danger', '⛔ 정지');
      ban.onclick = () => suspendForm(r, u);
      tools.append(ban);
    }
    if (u.role === 'student') {
      const del = el('button', 'ghost small danger', '삭제');
      del.onclick = async () => {
        if (!(await askConfirm(`${u.nickname} (${u.name}, ${u.student_no}) 회원을 지울까요?\n` +
                     '이 회원의 모집글·스터디·전공책·댓글·알림이 모두 함께 지워지며 되돌릴 수 없습니다.'))) return;
        try {
          await api(`/api/admin/users/${u.id}`, { method: 'DELETE' });
          toast('회원을 지웠습니다.');
          reload();
        } catch (e) { toast(e.message); }
      };
      tools.append(del);
    }
    main.append(el('span', 'avatar', u.nickname.slice(0, 1)), who, tools);
    r.append(main);
    return r;
  }

  /* 정지 입력칸: 기간(일)과 사유. 다시 누르면 닫는다 */
  function suspendForm(r, u) {
    const old = r.querySelector('.admin-suspend, .admin-notify');
    if (old) { const same = old.classList.contains('admin-suspend'); old.remove(); if (same) return; }
    const form = el('form', 'admin-notify admin-suspend stack-sm');
    const days = el('select');
    for (const d of [1, 3, 7, 14, 30, 90]) days.append(new Option(`${d}일`, d));
    days.value = '7';
    const custom = el('input');
    custom.type = 'number'; custom.min = 1; custom.max = 365; custom.placeholder = '1~365일';
    const reason = el('textarea');
    reason.rows = 2; reason.maxLength = 300; reason.required = true;
    reason.placeholder = '정지 사유 (회원 알림함으로 함께 보냅니다)';
    const dl = el('label'); dl.append(el('span', null, '정지 기간'), days);
    const cl = el('label'); cl.append(el('span', null, '또는 일수'), custom);
    const rl = el('label'); rl.append(el('span', null, `${u.nickname} 님 정지 사유`), reason);
    const go = el('button', 'small danger-fill', '정지하기');
    const cancel = el('button', 'ghost small', '취소');
    cancel.type = 'button';
    cancel.onclick = () => form.remove();
    const msg = el('p', 'form-msg', '');   /* 입력 오류·확인 안내를 눈에 보이게 적는 줄 */
    const top = el('div', 'row'); top.append(dl, cl);
    const btns = el('div', 'row'); btns.append(go, cancel, msg);
    form.append(top, rl, btns);

    /* 브라우저 기본 검사(말풍선)와 confirm 창은 눈에 띄지 않거나 막혀 있을 수 있어 쓰지 않는다.
     * 대신 오류는 이 줄에 빨갛게 적고, 확인은 같은 버튼을 한 번 더 누르게 한다. */
    form.noValidate = true;
    let armed = false;
    const disarm = () => { armed = false; go.textContent = '정지하기'; msg.textContent = ''; msg.className = 'form-msg'; };
    const fail = (text, field) => {
      disarm();
      msg.textContent = text;
      msg.className = 'form-msg error';
      field.classList.add('invalid');
      field.focus();
    };
    for (const f of [days, custom, reason]) f.addEventListener('input', () => { f.classList.remove('invalid'); disarm(); });
    days.addEventListener('change', disarm);

    form.onsubmit = async ev => {
      ev.preventDefault();
      const n = custom.value.trim() ? Number(custom.value) : Number(days.value);
      if (!Number.isInteger(n) || n < 1 || n > 365) return fail('정지 기간은 1~365일로 정하세요.', custom);
      if (!reason.value.trim()) return fail('정지 사유를 적어 주세요. 회원에게 알림으로 함께 보냅니다.', reason);
      if (!armed) {
        armed = true;
        go.textContent = `${n}일 정지 확정`;
        msg.textContent = `한 번 더 누르면 ${u.nickname} 님을 ${n}일 동안 정지합니다.`;
        msg.className = 'form-msg warn';
        return;
      }
      go.disabled = true;
      go.textContent = '정지하는 중…';
      try {
        const out = await api(`/api/admin/users/${u.id}/suspend`, { method: 'POST',
          body: JSON.stringify({ days: n, reason: reason.value.trim() }) });
        toast(`${u.nickname} 님을 ${out.suspended_until} 까지 정지했습니다. 회원 알림함으로 알렸습니다.`);
        reload();
      } catch (e) {
        go.disabled = false;
        fail(e.message, reason);
      }
    };
    r.append(form);
    reason.focus();
  }

  async function reload() {
    listEl.replaceChildren(...skeleton(4));
    try {
      const r = await api(`/api/admin/users${q.value.trim() ? `?q=${encodeURIComponent(q.value.trim())}` : ''}`);
      countEl.textContent = `(${r.users.length})`;
      listEl.replaceChildren(...(r.users.length ? r.users.map(row) : [el('p', 'empty', '찾는 회원이 없습니다.')]));
    } catch (e) { listEl.replaceChildren(el('p', 'empty', e.message)); }
  }
  let timer = null;
  q.oninput = () => { clearTimeout(timer); timer = setTimeout(reload, 300); };
  render(node);
  reload();
}

/* 전체 게시물의 종류별 이름·주소·주인 호칭 */
const CONTENT_KIND = {
  recruit:   { label: '팀원 모집',   path: 'recruits',  owner: '작성자', cls: 'kind-contest' },
  study:     { label: '그룹 스터디', path: 'studies',   owner: '방장',   cls: 'kind-hackathon' },
  book:      { label: '전공책',      path: 'books',     owner: '판매자', cls: 'kind-etc' },
};
const contentStatus = it => it.type === 'recruit' ? (it.status === 'open' ? '모집 중' : '마감')
  : it.type === 'book' ? BOOK_STATUS[it.status] : `${it.status}명`;

/* #/admin/content - 회원이 쓴 모든 글과 주인(작성자·방장·판매자), 주인에게 알림 보내기 */
async function viewAdminContent() {
  markNav('admin');
  setPageTitle('관리자 · 전체 게시물');
  if (!state.me || !state.me.is_admin) return renderMessage('관리자만 볼 수 있습니다.');

  const node = tpl('tpl-admin-content');
  const root = node.firstElementChild;
  adminTabs(root.querySelector('[data-tabs]'), 'content');
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const q = root.querySelector('[data-q]');
  const typeSel = root.querySelector('[data-type]');
  const sortSel = root.querySelector('[data-sort]');
  let items = [];

  function row(it) {
    const K = CONTENT_KIND[it.type];
    const link = `#/${K.path}/${it.id}`;
    const r = el('div', 'admin-row');
    const main = el('div', 'admin-row-main');
    const who = el('div', 'admin-who');
    const title = el('strong');
    const a = el('a', null, it.title);
    a.href = link;
    title.append(a);
    const tags = el('div', 'item-top');
    tags.style.marginBottom = '2px';
    tags.append(el('span', `tag ${K.cls}`, K.label), el('span', 'tag plain', contentStatus(it)));
    who.append(tags, title,
      el('span', null, `${K.owner} ${it.owner_nickname} (${it.owner_student_no}) · ${it.created_at} · ` +
                       (it.type === 'study' ? `멤버 ${it.count}명` : `댓글 ${it.count}개`)));
    const tools = el('div', 'admin-tools');
    const notify = el('button', 'ghost small', `📢 ${K.owner}에게 알림`);
    notify.onclick = () => adminNotifyForm(r, { id: it.owner_id, nickname: it.owner_nickname },
                                           { title: `관리자 안내: '${it.title}'`.slice(0, 100), link });
    tools.append(notify);
    main.append(who, tools);
    r.append(main);
    return r;
  }

  function paint() {
    const sorted = [...items].sort((x, y) => sortSel.value === 'old'
      ? x.created_at.localeCompare(y.created_at) : y.created_at.localeCompare(x.created_at));
    countEl.textContent = `(${sorted.length})`;
    listEl.replaceChildren(...(sorted.length ? sorted.map(row) : [el('p', 'empty', '게시물이 없습니다.')]));
  }

  async function reload() {
    listEl.replaceChildren(...skeleton(4));
    const qs = new URLSearchParams({ type: typeSel.value });
    if (q.value.trim()) qs.set('q', q.value.trim());
    try {
      items = (await api(`/api/admin/content?${qs}`)).items;
      paint();
    } catch (e) { listEl.replaceChildren(el('p', 'empty', e.message)); }
  }
  let timer = null;
  q.oninput = () => { clearTimeout(timer); timer = setTimeout(reload, 300); };
  typeSel.onchange = reload;
  sortSel.onchange = paint;
  render(node);
  reload();
}

/* ══════════════════════════════════════════════════ 알림함 */

const NOTI_ICON = { invite: '✉', invite_accepted: '✅', team: '🤝', admin: '📢', suspend: '⛔', unsuspend: '🔓' };
const NOTI_KIND = { invite: '참여 권유', invite_accepted: '권유 수락', team: '팀 합류', admin: '관리자 알림', suspend: '계정 정지', unsuspend: '정지 해제' };
/* 누르면 바로 그 화면(link)으로 가는 알림. 나머지(관리자·정지 알림)는 자세히 보기 화면으로 */
const NOTI_GO_LINK = new Set(['invite', 'invite_accepted', 'team']);
/* 참여 권유 알림의 상태(서버 invite_state)별 버튼 글자. open 만 누를 수 있다 */
const INVITE_BTN = { open: '참여하기', accepted: '✓ 참여함', member: '이미 참여 중', closed: '마감됨', gone: '지워진 글' };

/* #/notifications - 내 알림. 누르면 읽음으로 표시하고 그 화면(link)으로 간다 */
async function viewNotifications() {
  markNav('notifications');
  setPageTitle('알림함');
  if (!state.me) return requireLogin('알림을 보려면 로그인하세요.');

  const node = tpl('tpl-notifications');
  const root = node.firstElementChild;
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const readAll = root.querySelector('[data-read-all]');
  listEl.append(...skeleton(3));
  render(node);

  const setUnread = n => {
    state.unread = n;
    paintUnread();
    countEl.textContent = n ? `(안 읽음 ${n})` : '';
    readAll.disabled = !n;
  };

  function row(n) {
    const item = el('div', 'noti' + (n.read ? '' : ' unread'));
    item.tabIndex = 0;
    item.setAttribute('role', 'link');
    const main = el('div', 'noti-main');
    main.append(el('strong', null, `${NOTI_ICON[n.kind] || '🔔'} ${n.title}`));
    if (n.body) main.append(el('p', null, n.body));
    main.append(el('small', null, (NOTI_GO_LINK.has(n.kind) ? '' : `${n.actor_nickname || '관리자'} · `) + n.created_at +
                                  (NOTI_GO_LINK.has(n.kind) ? '' : ' · 눌러서 자세히 보기')));
    const tools = el('div', 'noti-tools');

    /* 참여 권유: [참여하기] 를 누르면 바로 참여한다
     * (스터디는 멤버로 가입, 팀원 모집글은 팀원이 되고 참여 댓글을 남김 - 서버 api_invite.c) */
    if (n.kind === 'invite' && n.invite_state) {
      const join = el('button', 'small', INVITE_BTN[n.invite_state] || '참여하기');
      join.type = 'button';
      /* 참여함·이미 참여 중은 초록, 마감·지워진 글은 회색 */
      const lock = text => {
        join.disabled = true;
        join.classList.toggle('done', text === INVITE_BTN.accepted || text === INVITE_BTN.member);
        join.textContent = text;
      };
      if (n.invite_state !== 'open') lock(INVITE_BTN[n.invite_state]);
      join.onclick = async ev => {
        ev.stopPropagation();
        const isRecruit = n.link && n.link.startsWith('#/recruits/');
        if (!(await askConfirm(isRecruit ? '이 모집글의 팀에 들어갈까요?\n글쓴이에게 참여 댓글과 알림이 갑니다.'
                               : '이 모임에 바로 참여할까요?'))) return;
        join.disabled = true;
        try {
          const out = await api(`/api/notifications/${n.id}/accept`, { method: 'POST' });
          lock(INVITE_BTN.accepted);
          n.invite_state = 'accepted';
          if (!n.read) { n.read = true; item.classList.remove('unread'); refreshUnread(); }
          toast(out.kind === 'recruit' ? '팀에 들어갔습니다! 팀 채팅방에서 인사해 보세요.' : '참여했습니다! 글쓴이에게도 알렸습니다.');
          location.hash = out.link;   /* 참여한 화면으로 */
        } catch (e) {
          toast(e.message);
          if (e.code === 'already_accepted') lock(INVITE_BTN.accepted);
          else if (e.code === 'gone') lock(INVITE_BTN.gone);
          else if (e.code === 'closed' || e.code === 'study_full') lock(INVITE_BTN.closed);
          else join.disabled = false;
        }
      };
      tools.append(join);
    }

    const del = el('button', 'ghost small', '삭제');
    del.type = 'button';
    del.onclick = async ev => {
      ev.stopPropagation();   // 알림을 누른 것으로 치지 않게
      try {
        const out = await api(`/api/notifications/${n.id}`, { method: 'DELETE' });
        item.remove();
        setUnread(out.unread);
        if (!listEl.childElementCount) listEl.append(el('p', 'empty', '알림이 없습니다.'));
      } catch (e) { toast(e.message); }
    };
    const open = async () => {
      if (!NOTI_GO_LINK.has(n.kind)) { location.hash = `#/notifications/${n.id}`; return; }
      if (!n.read) {
        try { setUnread((await api(`/api/notifications/${n.id}/read`, { method: 'POST' })).unread); }
        catch { /* 읽음 표시가 안 돼도 이동은 한다 */ }
        n.read = true;
        item.classList.remove('unread');
      }
      if (n.link && n.link.startsWith('#/')) location.hash = n.link;   /* 사이트 안 화면으로만 */
    };
    item.onclick = open;
    item.onkeydown = ev => { if (ev.key === 'Enter') open(); };
    tools.append(del);
    item.append(el('span', 'noti-dot'), main, tools);
    return item;
  }

  readAll.onclick = async () => {
    try {
      setUnread((await api('/api/notifications/read-all', { method: 'POST' })).unread);
      for (const it of listEl.querySelectorAll('.noti.unread')) it.classList.remove('unread');
    } catch (e) { toast(e.message); }
  };

  try {
    const r = await api('/api/notifications');
    setUnread(r.unread);
    listEl.replaceChildren(...(r.notifications.length
      ? r.notifications.map(row)
      : [el('p', 'empty', '알림이 없습니다. 다른 회원이 참여를 권유하면 여기에 표시됩니다.')]));
  } catch (e) {
    listEl.replaceChildren(el('p', 'empty', e.message));
  }
}

/* #/notifications/{id} - 알림 하나 자세히 (연 순간 읽음으로 표시) */
async function viewNotification(id) {
  markNav('notifications');
  setPageTitle('알림');
  if (!state.me) return requireLogin('알림을 보려면 로그인하세요.');

  let data;
  try { data = await api(`/api/notifications/${id}`); }
  catch (e) { return renderMessage(e.message); }
  const n = data.notification;
  state.unread = data.unread;
  paintUnread();

  const node = tpl('tpl-notification');
  const root = node.firstElementChild;
  root.querySelector('[data-tags-top]').append(
    el('span', n.kind === 'suspend' ? 'tag suspended' : 'tag owner', NOTI_KIND[n.kind] || '알림'));
  root.querySelector('[data-title]').textContent = `${NOTI_ICON[n.kind] || '🔔'} ${n.title}`;
  root.querySelector('[data-actor]').textContent = n.actor_nickname || '관리자';
  root.querySelector('[data-created]').textContent = n.created_at;
  root.querySelector('[data-body]').textContent = n.body || '내용이 없습니다.';

  const actions = root.querySelector('[data-actions]');
  if (n.link && n.link.startsWith('#/')) {
    const go = el('a', 'btn small', '관련 화면으로 가기 →');
    go.href = n.link;
    actions.append(go);
  }
  const del = el('button', 'ghost small', '이 알림 지우기');
  del.onclick = async () => {
    if (!(await askConfirm('이 알림을 지울까요?'))) return;
    try {
      await api(`/api/notifications/${n.id}`, { method: 'DELETE' });
      toast('지웠습니다.');
      location.hash = '#/notifications';
    } catch (e) { toast(e.message); }
  };
  actions.append(del);
  render(node);
}

/* ══════════════════════════════════════════════════ 전공책 장터 */

const BOOK_STATUS = { selling: '판매 중', reserved: '예약 중', sold: '판매 완료' };
const BOOK_STATUS_TAG = { selling: 'tag ok', reserved: 'tag warn', sold: 'tag plain' };
const BOOK_COND = { new: '새 책', good: '깨끗함', fair: '필기 조금', poor: '필기 많음' };
const BOOK_PHOTO_MAX = 5;
const photoUrl = id => `/api/book-photos/${id}`;
const priceText = n => (n ? `${n.toLocaleString('ko-KR')}원` : '나눔');

/* 거래 정보 버튼에 쓰는 고정 목록. 장소는 우리 학교 건물에 맞게 고쳐 쓴다. */
const BOOK_PLACES = ['정문', '중앙도서관', '학생회관', '학생식당', '기숙사 앞', '버스정류장'];
const BOOK_DAYS   = ['월', '화', '수', '목', '금', '토', '일'];
const BOOK_SLOTS  = [['오전', '9~12시'], ['점심', '12~13시'], ['오후', '13~18시'], ['저녁', '18시~']];

/* 서버에 저장하는 글자 형식 (사람이 읽어도 되고, 다시 버튼으로 풀 수도 있다)
 *   trade_places  "중앙도서관 · 정문"
 *   trade_times   "월·수·금 / 점심·오후"   (요일만 / 시간대만 있으면 그쪽만)
 *   trade_wish    "수요일 12:30"           (요일만 / 시각만도 된다) */
const splitPlaces = s => (s || '').split(' · ').map(x => x.trim()).filter(Boolean);
function splitTimes(s) {
  const out = { days: [], slots: [] };
  for (const part of (s || '').split(' / '))
    for (const x of part.split('·').map(v => v.trim()).filter(Boolean))
      if (BOOK_DAYS.includes(x)) out.days.push(x);
      else if (BOOK_SLOTS.some(([k]) => k === x)) out.slots.push(x);
  return out;
}
function joinTimes(days, slots) {
  return [days.join('·'), slots.join('·')].filter(Boolean).join(' / ');
}

/* 여러 개 고르는 버튼 묶음 (체크박스). items = [[값, 작은 설명?], ...], picked = 처음 눌린 값들 */
function pickChips(items, picked, label, single = false) {
  const box = el('div', 'pick-chips');
  box.setAttribute('role', single ? 'radiogroup' : 'group');
  box.setAttribute('aria-label', label);
  const name = 'pick-' + Math.random().toString(36).slice(2);
  for (const [value, hint] of items) {
    const l = el('label');
    const input = el('input');
    input.type = single ? 'radio' : 'checkbox';
    input.name = name;
    input.value = value;
    input.checked = picked.includes(value);
    const span = el('span', null, value);
    if (hint) span.append(el('small', null, hint));
    l.append(input, span);
    box.append(l);
  }
  box.values = () => [...box.querySelectorAll('input:checked')].map(i => i.value);
  return box;
}

/* 희망 거래 시간: 요일 고르기 + 시각 칸. "수요일 12:30" 형식으로 돌려준다. */
function wishPicker(initial) {
  const m = /^(?:([월화수목금토일])요일)?\s*(\d{1,2}:\d{2})?$/.exec((initial || '').trim()) || [];
  const box = el('div', 'row wish-pick');
  const day = el('select');
  day.setAttribute('aria-label', '희망 요일');
  day.append(new Option('요일 선택', ''), ...BOOK_DAYS.map(d => new Option(`${d}요일`, d)));
  day.value = m[1] || '';
  const time = el('input');
  time.type = 'time';
  time.step = 600;
  time.setAttribute('aria-label', '희망 시각');
  time.value = m[2] ? m[2].padStart(5, '0') : '';
  box.append(day, time);
  box.value = () => [day.value && `${day.value}요일`, time.value].filter(Boolean).join(' ');
  return box;
}

/* 거래 정보 입력 칸 세 개 (판매 글 쓰기와 상세의 "거래 정보 바꾸기"에서 같이 쓴다).
 * host 안에 그리고, 서버로 보낼 {trade_places, trade_times, trade_wish} 를 돌려주는 함수를 돌려준다. */
function tradeFields(host, b = {}) {
  const places = splitPlaces(b.trade_places);
  const extra = places.filter(p => !BOOK_PLACES.includes(p));   /* 버튼에 없는 장소 = 직접 입력한 것 */
  const placeChips = pickChips(BOOK_PLACES.map(p => [p]), places, '원하는 학교 장소');
  const placeEtc = el('input');
  placeEtc.maxLength = 60;
  placeEtc.placeholder = '직접 입력 (예: 공학관 3층 라운지)';
  placeEtc.value = extra.join(', ');

  const t = splitTimes(b.trade_times);
  const dayChips = pickChips(BOOK_DAYS.map(d => [d]), t.days, '거래 가능한 요일');
  const slotChips = pickChips(BOOK_SLOTS, t.slots, '거래 가능한 시간대');
  const wish = wishPicker(b.trade_wish);

  const field = (title, hint, ...nodes) => {
    const f = el('div', 'trade-field');
    const head = el('span', 'field-label', title);
    if (hint) head.append(' ', el('small', 'muted', hint));
    f.append(head, ...nodes);
    return f;
  };
  host.replaceChildren(
    field('📍 원하는 학교 장소', '여러 곳 고를 수 있어요', placeChips, placeEtc),
    field('🗓 거래 가능한 시간', '요일과 시간대를 눌러 고르세요', dayChips, slotChips),
    field('⏰ 희망 거래 시간', '가장 좋은 때 하나', wish));

  return () => ({
    trade_places: [...placeChips.values(),
                   ...placeEtc.value.split(',').map(x => x.trim().replace(/·/g, '')).filter(Boolean)].join(' · '),
    trade_times: joinTimes(dayChips.values(), slotChips.values()),
    trade_wish: wish.value(),
  });
}

/* 상세 화면의 거래 정보 보기 (정하지 않은 칸은 "협의") */
function tradeView(host, b) {
  const row = (icon, title, chips) => {
    const r = el('div', 'trade-row');
    r.append(el('span', 'trade-key', `${icon} ${title}`));
    const v = el('div', 'chips');
    v.append(...(chips.length ? chips.map(c => el('span', 'chip', c)) : [el('span', 'muted', '협의')]));
    r.append(v);
    return r;
  };
  const t = splitTimes(b.trade_times);
  const times = [t.days.join('·'), ...t.slots.map(k => `${k} ${BOOK_SLOTS.find(([x]) => x === k)[1]}`)]
    .filter(Boolean);
  if (!t.days.length && !t.slots.length && b.trade_times) times.push(b.trade_times);   /* 형식이 다른 옛 값 */
  host.replaceChildren(
    row('📍', '원하는 장소', splitPlaces(b.trade_places)),
    row('🗓', '거래 가능 시간', times),
    row('⏰', '희망 거래 시간', b.trade_wish ? [b.trade_wish] : []));
}

/* 학과 선택 상자에 단과대학별 학과를 채운다 (optgroup 으로 묶음) */
function fillDeptSelect(sel) {
  for (const c of state.colleges) {
    const group = document.createElement('optgroup');
    group.label = c.name;
    for (const d of c.departments) group.append(new Option(d.name, d.id));
    sel.append(group);
  }
}

/* 책 목록 카드: 대표 사진, 판매 상태·책 상태, 제목, 저자·과목, 가격
 * 학과별 추천 탭에서는 사진 위에 관련도 게이지(score), 아래에 겹친 키워드(terms)가 붙는다 */
function bookCard(b) {
  const a = el('a', 'book-card' + (b.status === 'sold' ? ' sold' : ''));
  a.href = `#/books/${b.id}`;

  const thumb = el('div', 'book-thumb');
  if (b.photo_id) {
    const img = el('img');
    img.src = photoUrl(b.photo_id);
    img.alt = '';
    img.loading = 'lazy';
    thumb.append(img);
    if (b.photo_count > 1) thumb.append(el('span', 'photo-n', `📷 ${b.photo_count}`));
  } else {
    thumb.append(el('span', null, '📚'));
  }
  if (typeof b.score === 'number') {
    const ring = scoreRing(b.score);
    ring.classList.add('book-score');
    thumb.append(ring);
  }

  const body = el('div', 'book-body');
  const tags = el('div', 'item-top');
  tags.append(el('span', BOOK_STATUS_TAG[b.status], BOOK_STATUS[b.status]),
              el('span', 'tag cond', BOOK_COND[b.condition]));
  if (state.me && state.me.id === b.seller_id) tags.append(el('span', 'tag owner', '내 책'));
  body.append(tags, el('h3', null, b.title));
  const sub = [b.author, b.course].filter(Boolean).join(' · ');
  if (sub) body.append(el('p', 'book-sub', sub));
  body.append(el('p', 'book-price' + (b.price ? '' : ' free'), priceText(b.price)));
  const places = splitPlaces(b.trade_places);
  if (places.length || b.trade_wish) {
    const where = places.length ? `📍 ${places[0]}${places.length > 1 ? ` 외 ${places.length - 1}` : ''}` : '';
    body.append(el('p', 'book-trade', [where, b.trade_wish && `⏰ ${b.trade_wish}`].filter(Boolean).join('  ')));
  }
  if (b.terms && b.terms.length) body.append(recoTerms(b.terms));
  const meta = el('div', 'item-meta');
  meta.append(el('span', null, b.seller_nickname), el('span', null, b.created_at.slice(5, 10).replace('-', '/')));
  if (b.comment_count) meta.append(el('span', null, `문의 ${b.comment_count}`));
  body.append(meta);

  a.append(thumb, body);
  return a;
}

/* 책 목록. 탭(tab)마다 서버에 묻는 조건이 다르다.
 *   all   #/books              전체 (검색, 학과, 판매 상태, 정렬)
 *   major #/books/major        판매 글의 학과가 내 학과인 책 (학과 칸 대신 내 학과로 고정)
 *   reco  #/books/reco/{학과}  자체 모델이 제목·과목·설명을 학과 전공 분야와 비교해 관련 있는 순으로 (판매 중만) */
async function viewBooks(tab = 'all', deptParam) {
  markNav('books');
  setPageTitle(tab === 'major' ? '내 학과 대상 전공책' : tab === 'reco' ? '학과별 추천 전공책' : '전공책 장터');
  if (tab === 'major' && !needMyDept(BOOK_TABS, 'major', '로그인하면 내 학과 전공책만 모아 볼 수 있습니다.')) return;

  const node = tpl('tpl-books');
  const root = node.firstElementChild;
  subTabs(root.querySelector('[data-tabs]'), BOOK_TABS, tab);
  const listEl = root.querySelector('[data-list]');
  const countEl = root.querySelector('[data-count]');
  const heading = root.querySelector('[data-heading]');
  const sub = root.querySelector('[data-sub]');
  const form = root.querySelector('[data-filters]');
  const f = name => root.querySelector(`[data-f="${name}"]`);
  fillDeptSelect(f('dept'));
  if (!state.me) {
    root.querySelector('[data-new]').href = '#/login';
    f('status').querySelector('[value="mine"]').disabled = true;
  }

  let reco = 0;   /* 학과별 추천 탭에서 고른 학과 */
  if (tab === 'major') {
    f('dept').value = String(state.me.department_id);
    root.querySelector('[data-dept-filter]').hidden = true;
    form.classList.replace('four', 'three');
    heading.textContent = `${state.me.department_name} 전공책`;
    sub.textContent = '판매 글에 학과를 내 학과로 정한 책입니다.';
  } else if (tab === 'reco') {
    const deptSel = root.querySelector('[data-reco-dept]');
    reco = recoDept(deptParam);
    fillDeptSelect(deptSel);
    deptSel.value = String(reco);
    deptSel.onchange = () => { location.hash = `#/books/reco/${deptSel.value}`; };
    form.hidden = true;   /* 떼어 내지 않는다: reload 가 f() 로 그 안의 칸을 찾는다 */
    root.querySelector('[data-filter-reco]').hidden = false;
    heading.textContent = `${deptName(reco)} 추천 전공책`;
    sub.textContent = '자체 AI 모델이 책 제목·과목·설명을 학과의 전공 분야와 비교해 관련 있는 순서로 보여 줍니다. 판매 완료된 책은 빠집니다.';
  }
  sub.hidden = !sub.textContent;

  async function reload() {
    listEl.replaceChildren(...skeleton(6, 'sk-reco'));
    const st = f('status').value;
    const qs = reco ? new URLSearchParams({ reco })
      : new URLSearchParams({ sort: f('sort').value, status: st === 'mine' ? 'all' : st });
    if (!reco && f('q').value.trim()) qs.set('q', f('q').value.trim());
    if (!reco && f('dept').value) qs.set('dept', f('dept').value);
    if (!reco && st === 'mine') qs.set('mine', '1');
    try {
      const r = await api(`/api/books?${qs}`);
      countEl.textContent = `(${r.books.length})`;
      listEl.replaceChildren(...(r.books.length
        ? r.books.map(bookCard)
        : [el('p', 'empty', reco ? '이 학과와 관련 있다고 판단한 판매 중인 책이 아직 없습니다.'
                          : st === 'mine' ? '아직 올린 판매 글이 없습니다.'
                          : '조건에 맞는 책이 없습니다. 다 본 전공책을 올려 보세요.')]));
    } catch (e) {
      listEl.replaceChildren(el('p', 'empty', e.message));
    }
  }
  /* 검색어는 입력을 멈추고 0.3초 뒤에 찾는다 (글자마다 요청하지 않게) */
  let timer = null;
  f('q').oninput = () => { clearTimeout(timer); timer = setTimeout(reload, 300); };
  root.querySelector('[data-filters]').onsubmit = ev => { ev.preventDefault(); reload(); };
  for (const k of ['dept', 'status', 'sort']) f(k).onchange = reload;
  render(node);
  reload();
}

/* 사진 파일을 긴 변 1280px 이하 JPEG data URL 로 줄인다 (휴대폰 사진은 몇 MB 라 그대로 올리면 무겁다).
 * 서버 한도(1MB)를 넘으면 화질을 낮춰 다시 만든다. */
async function shrinkPhoto(file, maxSide = 1280) {
  const url = URL.createObjectURL(file);
  try {
    const img = await new Promise((ok, fail) => {
      const im = new Image();
      im.onload = () => ok(im);
      im.onerror = () => fail(new Error('이미지를 읽을 수 없습니다. JPG·PNG 사진을 골라 주세요.'));
      im.src = url;
    });
    const scale = Math.min(1, maxSide / Math.max(img.naturalWidth, img.naturalHeight));
    const canvas = document.createElement('canvas');
    canvas.width = Math.round(img.naturalWidth * scale);
    canvas.height = Math.round(img.naturalHeight * scale);
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#fff';   /* 투명한 PNG 의 배경을 흰색으로 */
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
    for (const q of [0.82, 0.7, 0.55, 0.4]) {
      const data = canvas.toDataURL('image/jpeg', q);
      if (data.length < 1300 * 1024) return data;
    }
    throw new Error('사진이 너무 큽니다.');
  } finally {
    URL.revokeObjectURL(url);
  }
}

/* #/books/new       - 판매 글 쓰기 (사진은 카메라로 찍거나 앨범에서 고른다)
 * #/books/{id}/edit - 내 판매 글 고치기 (같은 화면에 원래 값을 채운다) */
async function viewBookNew(editId) {
  markNav('books');
  setPageTitle(editId ? '판매 글 고치기' : '책 팔기');
  if (!state.me) return requireLogin(editId ? '로그인 후 고칠 수 있습니다.' : '책을 팔려면 로그인하세요.');

  let orig = null;   /* 고치기: 서버의 원래 글 */
  if (editId) {
    try { orig = await api(`/api/books/${editId}`); }
    catch (e) { return renderMessage(e.message); }
    if (!orig.can_manage) return renderMessage('내가 올린 판매 글만 고칠 수 있습니다.');
  }

  const node = tpl('tpl-book-new');
  const form = node.querySelector('[data-form]');
  const picker = node.querySelector('[data-photos]');
  const countEl = node.querySelector('[data-photo-count]');
  const camera = node.querySelector('[data-camera]');
  const gallery = node.querySelector('[data-gallery]');
  const deptSel = node.querySelector('[data-dept]');
  fillDeptSelect(deptSel);
  if (state.me.department_id) deptSel.value = String(state.me.department_id);   /* 기본은 내 학과 */
  const tradeValues = tradeFields(node.querySelector('[data-trade]'), orig ? orig.book : {});

  /* 사진 목록 (순서 = 올릴 순서, 첫 장이 대표). 숫자 = 이미 올린 사진 id, 글자 = 새로 줄인 사진 data URL */
  const photos = orig ? [...orig.photos] : [];
  const submitText = editId ? '고친 내용 저장' : '판매 글 올리기';
  if (orig) {
    const b = orig.book;
    const back = node.querySelector('.back');
    back.href = `#/books/${editId}`;
    back.textContent = '← 판매 글로 돌아가기';
    node.querySelector('[data-form-title]').textContent = '판매 글 고치기';
    form.querySelector('button[type="submit"]').textContent = submitText;
    const f = form.elements;   /* form.title 은 폼 자신의 title 속성이라 elements 로 찾는다 */
    f.title.value = b.title;
    f.author.value = b.author;
    f.course.value = b.course;
    deptSel.value = b.department_id ? String(b.department_id) : '';
    f.price.value = b.price;
    f.condition.value = b.condition;
    f.description.value = orig.description;
  }
  let busy = 0;        /* 줄이는 중인 사진 수 */

  function paint() {
    picker.replaceChildren(...photos.map((data, i) => {
      const slot = el('div', 'photo-slot');
      const img = el('img');
      img.src = typeof data === 'number' ? photoUrl(data) : data;
      img.alt = `사진 ${i + 1}`;
      const x = el('button', 'photo-x', '×');
      x.type = 'button';
      x.setAttribute('aria-label', `사진 ${i + 1} 빼기`);
      x.onclick = () => { photos.splice(i, 1); paint(); };
      slot.append(img, x);
      if (i === 0) slot.append(el('span', 'photo-main', '대표'));
      return slot;
    }), ...Array.from({ length: busy }, () => el('div', 'photo-slot busy', '줄이는 중…')));
    countEl.textContent = `${photos.length} / ${BOOK_PHOTO_MAX}장 · 첫 사진이 대표 사진`;
    const full = photos.length + busy >= BOOK_PHOTO_MAX;
    camera.disabled = gallery.disabled = full;
  }

  async function addFiles(input) {
    const files = [...input.files].slice(0, BOOK_PHOTO_MAX - photos.length - busy);
    input.value = '';   /* 같은 파일을 다시 골라도 change 가 오게 */
    if (input.files.length > files.length) toast(`사진은 ${BOOK_PHOTO_MAX}장까지 올릴 수 있습니다.`);
    busy += files.length;
    paint();
    for (const file of files) {
      try { photos.push(await shrinkPhoto(file)); }
      catch (e) { toast(e.message); }
      busy--;
      paint();
    }
  }
  camera.onchange = () => addFiles(camera);
  gallery.onchange = () => addFiles(gallery);
  paint();

  form.onsubmit = async ev => {
    ev.preventDefault();
    if (busy) return toast('사진을 준비하는 중입니다. 잠시 뒤에 다시 눌러 주세요.');
    const v = formValues(form);
    if (!photos.length && !(await askConfirm('사진 없이 올릴까요? 사진이 있으면 훨씬 빨리 팔립니다.'))) return;
    const btn = form.querySelector('button[type="submit"]');
    btn.disabled = true;
    btn.textContent = editId ? '저장하는 중…' : '올리는 중…';
    try {
      const out = await api(editId ? `/api/books/${editId}` : '/api/books', {
        method: editId ? 'PUT' : 'POST',
        body: JSON.stringify({
          title: v.title, author: v.author, course: v.course,
          department_id: Number(v.department_id) || null, price: Number(v.price || 0),
          condition: v.condition, description: v.description, photos, ...tradeValues(),
        }),
      });
      toast(editId ? '판매 글을 고쳤습니다.' : '판매 글을 올렸습니다.');
      location.hash = `#/books/${editId || out.id}`;
    } catch (e) {
      toast(e.message);
      btn.disabled = false;
      btn.textContent = submitText;
    }
  };
  render(node);
}

/* #/books/{id} - 상세: 사진, 책 정보, 판매자의 상태 변경·삭제, 문의 댓글 */
async function viewBook(id) {
  markNav('books');
  setPageTitle('전공책 장터');

  let data;
  try { data = await api(`/api/books/${id}`); }
  catch (e) { return renderMessage(e.message); }

  const { book: b, description, photos, can_manage, comments } = data;
  const node = tpl('tpl-book');
  const root = node.firstElementChild;

  /* 사진: 큰 사진 하나 + 아래 작은 사진을 누르면 바뀐다 */
  if (photos.length) {
    const gal = root.querySelector('[data-gallery]');
    const main = gal.querySelector('[data-main]');
    const thumbs = gal.querySelector('[data-thumbs]');
    gal.hidden = false;
    const show = i => {
      main.src = photoUrl(photos[i]);
      main.alt = `${b.title} 사진 ${i + 1}`;
      [...thumbs.children].forEach((t, j) => t.classList.toggle('on', i === j));
    };
    if (photos.length > 1) {
      thumbs.append(...photos.map((pid, i) => {
        const t = el('button');
        t.type = 'button';
        t.setAttribute('aria-label', `사진 ${i + 1} 보기`);
        const img = el('img');
        img.src = photoUrl(pid);
        img.alt = '';
        t.append(img);
        t.onclick = () => show(i);
        return t;
      }));
    }
    show(0);
  }

  root.querySelector('[data-tags-top]').append(
    el('span', BOOK_STATUS_TAG[b.status], BOOK_STATUS[b.status]),
    el('span', 'tag cond', BOOK_COND[b.condition]));
  root.querySelector('[data-title]').textContent = b.title;
  const price = root.querySelector('[data-price]');
  price.textContent = priceText(b.price);
  price.classList.toggle('free', !b.price);
  root.querySelector('[data-seller]').textContent =
    b.seller_nickname + (b.seller_department ? ` · ${b.seller_department}` : '');
  root.querySelector('[data-author]').textContent = b.author || '-';
  root.querySelector('[data-course]').textContent = b.course || '-';
  root.querySelector('[data-dept]').textContent = b.department || '-';
  root.querySelector('[data-created]').textContent = b.created_at;
  root.querySelector('[data-description]').textContent = description || '설명이 없습니다.';

  /* 거래 정보: 판매자는 [바꾸기], 사는 사람은 [이 조건으로 거래 요청] (문의 칸에 글을 채워 준다) */
  const tradeBox = root.querySelector('[data-trade]');
  const tradeActs = root.querySelector('[data-trade-actions]');
  tradeView(tradeBox, b);
  const isSeller = state.me && state.me.id === b.seller_id;
  if (can_manage) {
    const edit = el('button', 'ghost small', '✏ 거래 정보 바꾸기');
    edit.type = 'button';
    edit.onclick = () => {
      const values = tradeFields(tradeBox, b);
      const save = el('button', 'small', '저장');
      const cancel = el('button', 'ghost small', '취소');
      save.type = cancel.type = 'button';
      cancel.onclick = () => viewBook(id);
      save.onclick = async () => {
        save.disabled = true;
        try {
          await api(`/api/books/${id}/trade`, { method: 'PUT', body: JSON.stringify(values()) });
          toast('거래 정보를 바꿨습니다.');
          viewBook(id);
        } catch (e) { toast(e.message); save.disabled = false; }
      };
      tradeActs.replaceChildren(save, cancel);
    };
    tradeActs.append(edit);
  }
  if (!isSeller && b.status !== 'sold') {
    const ask = el('button', 'small', '🤝 이 조건으로 거래 요청');
    ask.type = 'button';
    ask.onclick = () => {
      if (!state.me) return toast('로그인 후 거래를 요청할 수 있습니다.');
      /* 판매자가 고른 장소 중 하나 + 희망 시간(판매자 희망 시간으로 미리 채움) */
      const places = splitPlaces(b.trade_places);
      const pick = el('div', 'trade-ask');
      const placeChips = places.length ? pickChips(places.map(p => [p]), [places[0]], '만날 장소', true) : null;
      const wish = wishPicker(b.trade_wish);
      const go = el('button', 'small', '문의 칸에 넣기');
      go.type = 'button';
      go.onclick = () => {
        const where = placeChips ? placeChips.values()[0] : '';
        const when = wish.value();
        const text = `${where ? `📍 ${where}` : ''}${where && when ? ', ' : ''}${when ? `⏰ ${when}` : ''}`;
        const ta = root.querySelector('[data-comment-form] textarea');
        ta.value = `${text ? text + ' 에 ' : ''}거래 가능할까요? 「${b.title}」 사고 싶습니다.`;
        ta.focus();
        ta.scrollIntoView({ behavior: 'smooth', block: 'center' });
        pick.remove();
        ask.hidden = false;
      };
      if (placeChips) pick.append(el('span', 'field-label', '📍 만날 장소'), placeChips);
      pick.append(el('span', 'field-label', '⏰ 내가 원하는 시간'), wish, go);
      ask.hidden = true;
      tradeBox.after(pick);
    };
    tradeActs.append(ask);
  }

  /* 판매자·관리자: 판매 상태 바꾸기, 삭제 */
  if (can_manage) {
    const box = root.querySelector('[data-owner]');
    box.hidden = false;
    const sel = box.querySelector('[data-status]');
    sel.value = b.status;
    sel.onchange = async () => {
      try {
        await api(`/api/books/${id}/status`, { method: 'PUT', body: JSON.stringify({ status: sel.value }) });
        toast(sel.value === 'sold' ? '판매 완료로 바꿨습니다. 30일 뒤 자동으로 지워집니다.'
                                   : `${BOOK_STATUS[sel.value]}(으)로 바꿨습니다.`);
        viewBook(id);
      } catch (e) { toast(e.message); sel.value = b.status; }
    };
    box.querySelector('[data-edit]').href = `#/books/${id}/edit`;
    box.querySelector('[data-delete]').onclick = async () => {
      if (!(await askConfirm('이 판매 글을 지울까요? 사진과 문의 댓글도 함께 지워집니다.'))) return;
      try {
        await api(`/api/books/${id}`, { method: 'DELETE' });
        toast('지웠습니다.');
        location.hash = '#/books';
      } catch (e) { toast(e.message); }
    };
  }

  /* 문의 댓글 (게시물 상세의 commentRow 를 재사용, 삭제 API 만 다르다) */
  const listEl = root.querySelector('[data-comments]');
  const countEl = root.querySelector('[data-count]');
  const paint = items => {
    countEl.textContent = `(${items.length})`;
    listEl.replaceChildren(...(items.length
      ? items.map(c => commentRow(c, async cid => {
          try {
            await api(`/api/book-comments/${cid}`, { method: 'DELETE' });
            paint((await api(`/api/books/${id}`)).comments);
          } catch (e) { toast(e.message); }
        }, { userId: b.seller_id, label: '판매자' }))
      : [el('p', 'empty', '아직 문의가 없습니다.')]));
  };
  paint(comments);

  const form = root.querySelector('[data-comment-form]');
  const ta = form.querySelector('textarea');
  const reason = root.querySelector('[data-reason]');
  if (!state.me) {
    ta.disabled = true;
    form.querySelector('button').disabled = true;
    reason.textContent = '로그인 후 문의할 수 있습니다.';
    reason.classList.add('blocked');
  } else {
    reason.textContent = `${state.me.nickname} 이름으로 씁니다.`;
  }
  form.onsubmit = async ev => {
    ev.preventDefault();
    const text = ta.value.trim();
    if (!text) return;
    try {
      await api(`/api/books/${id}/comments`, { method: 'POST', body: JSON.stringify({ body: text }) });
      ta.value = '';
      paint((await api(`/api/books/${id}`)).comments);
    } catch (e) { toast(e.message); }
  };

  render(node);
}

/* ══════════════════════════════════════════ 팀원 찾기 · 내 프로필 */

/* 잘 맞는 회원 카드: 아바타, 소속, 점수 게이지, 키워드(겹치는 것 강조), ♥ 관계, 자기소개 */
function personCard(m, target) {
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

  /* 참여 권유: 누르면 서버가 그 회원의 학교 메일로 안내를 보낸다. 한 번 보내면 다시 누를 수 없다
   * (서버도 같은 모임·같은 회원에게 두 번 보내지 않는다). */
  if (target) {
    const btn = el('button', 'small invite-btn');
    btn.type = 'button';
    const done = () => {
      btn.disabled = true;
      btn.classList.add('sent');
      btn.textContent = '✓ 권유 보냄';
    };
    if (m.invited) done();
    else if (!target.open) { btn.disabled = true; btn.textContent = '모집 마감'; }
    else {
      btn.textContent = '✉ 참여 권유';
      btn.onclick = async () => {
        if (!(await askConfirm(`${m.nickname} 님에게 '${target.title}' 참여를 권유할까요?\n알림함과 학교 메일로 안내가 가며, 한 번만 보낼 수 있습니다.`))) return;
        btn.disabled = true;
        btn.textContent = '보내는 중…';
        try {
          const out = await api('/api/invitations', { method: 'POST',
            body: JSON.stringify({ kind: target.kind, id: target.id, user_id: m.id }) });
          m.invited = true;
          done();
          toast(out.mailed ? `${m.nickname} 님의 알림함과 메일로 참여 권유를 보냈습니다.`
                           : `${m.nickname} 님의 알림함으로 참여 권유를 보냈습니다.`);
        } catch (e) {
          if (e.code === 'already_invited') { m.invited = true; done(); }
          else { btn.disabled = false; btn.textContent = '✉ 참여 권유'; }
          toast(e.message);
        }
      };
    }
    c.append(btn);
  }
  return c;
}

/* 권유 대상 종류별 이름과 돌아갈 주소 (서버의 target.subkind) */
const INVITE_KIND = {
  recruit:   { label: '팀원 모집',   path: 'recruits',  nav: 'recruits' },
  study:     { label: '그룹 스터디', path: 'studies',   nav: 'studies' },
};

/* #/people/{recruit|study}/{id} - 내 모집글·스터디에 맞는 회원 찾기 + 참여 권유 (글쓴이·방장) */
async function viewPeople(kind, id) {
  markNav(kind === 'recruit' ? 'recruits' : 'studies');
  setPageTitle('팀원 찾기');
  if (!state.me) return requireLogin('팀원을 찾으려면 로그인하세요.');
  if (state.me.is_admin) return renderMessage('팀원 찾기는 학생 계정에서 쓸 수 있습니다.');

  const node = tpl('tpl-people');
  const root = node.firstElementChild;
  const list = root.querySelector('[data-list]');
  const mine = root.querySelector('[data-my-tags]');
  const back = root.querySelector('[data-back]');
  back.textContent = '← 돌아가기';
  back.href = kind === 'recruit' ? `#/recruits/${id}` : `#/studies/${id}`;
  list.append(...skeleton(6, 'sk-reco'));
  render(node);

  try {
    const r = await api(`/api/members/matches?limit=24&kind=${kind}&id=${id}`);
    const K = INVITE_KIND[r.target.subkind] || INVITE_KIND.recruit;
    markNav(K.nav);
    setPageTitle(`${K.label} · 팀원 찾기`);
    back.textContent = `← ${r.target.title}`;
    back.href = `#/${K.path}/${id}`;
    root.querySelector('[data-heading]').textContent = `'${r.target.title}' 에 함께할 회원`;

    mine.replaceChildren(el('span', 'muted small', '내 키워드'),
      ...(r.my_tags.length ? r.my_tags.map(t => el('span', 'ttag hit', t))
                           : [el('span', 'small', '아직 고르지 않았습니다')]));
    /* 키워드가 없으면 비교할 근거가 없으므로 프로필 작성을 안내 */
    if (!r.my_tags.length) {
      list.replaceChildren(el('p', 'empty', '내 프로필에서 관심 키워드를 고르면 잘 맞는 회원을 찾아 드립니다.'));
      return;
    }
    list.replaceChildren(...(r.members.length
      ? r.members.map(m => personCard(m, r.target))
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

/* ══════════════════════════════════════════════════════ 채팅방 */

/* 방 종류(서버의 subkind) → 화면 이름·아이콘 */
const CHAT_KIND = {
  recruit:   { label: '팀원 모집', icon: '🤝' },
  study:     { label: '그룹 스터디', icon: '📚' },
};
const CHAT_POLL_MS = 3000;   /* 방을 보는 동안 새 메시지를 묻는 간격 */
let chatTimer = null;        /* 방 화면의 새 메시지 확인 타이머 (route() 가 화면을 바꿀 때 멈춘다) */

/* 목록 카드(링크) 안에 넣는 [채팅방으로 이동] 버튼. 카드 자체로 이동하지 않게 막고 채팅방으로 간다 */
function chatCardButton(href) {
  const b = el('button', 'chat-chip', '💬 채팅방으로 이동');
  b.type = 'button';
  b.onclick = ev => {
    ev.preventDefault();
    ev.stopPropagation();
    location.hash = href;
  };
  return b;
}

/* "2026-10-14 21:05" → 오늘이면 "21:05", 아니면 "10/14" */
function chatShortTime(at) {
  if (!at) return '';
  const now = new Date();
  const today = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}`;
  return at.slice(0, 10) === today ? at.slice(11, 16) : `${Number(at.slice(5, 7))}/${Number(at.slice(8, 10))}`;
}

/* "2026-10-14" → "2026년 10월 14일 (화)" (대화 사이 날짜 구분선) */
function chatDateLabel(day) {
  const d = new Date(`${day}T00:00:00`);
  const w = '일월화수목금토'[d.getDay()];
  return `${d.getFullYear()}년 ${d.getMonth() + 1}월 ${d.getDate()}일 (${w})`;
}

/* #/chats - 내 채팅방 목록 (최근 대화가 있는 방이 위로) */
async function viewChats() {
  markNav('chats');
  setPageTitle('채팅방');
  if (!state.me) return requireLogin('채팅방을 보려면 로그인하세요.');

  const node = tpl('tpl-chats');
  const root = node.firstElementChild;
  const listEl = root.querySelector('[data-list]');
  listEl.append(...skeleton(3));
  render(node);

  try {
    const r = await api('/api/chats');
    state.chatUnread = r.unread;
    paintUnread();
    root.querySelector('[data-count]').textContent = `(${r.rooms.length})`;
    if (!r.rooms.length) {
      const empty = el('div', 'empty');
      empty.append(el('p', null, '아직 들어간 모임이 없습니다.'));
      const links = el('div', 'row chat-empty-links');
      for (const [href, text] of [['#/recruits/all', '모집글 둘러보기'], ['#/studies', '그룹 스터디 보기']]) {
        const a = el('a', 'btn ghost small', text);
        a.href = href;
        links.append(a);
      }
      empty.append(links);
      listEl.replaceChildren(empty);
      return;
    }
    listEl.replaceChildren(...r.rooms.map(room => {
      const K = CHAT_KIND[room.subkind] || CHAT_KIND.study;
      const a = el('a', 'chat-room' + (room.unread ? ' unread' : ''));
      a.href = `#/chats/${room.kind}/${room.id}`;
      const main = el('div', 'chat-room-main');
      const top = el('div', 'chat-room-top');
      top.append(el('strong', null, room.title));
      top.append(el('span', 'tag plain', `${K.label} · ${room.members}명`));
      if (room.closed) top.append(el('span', 'tag plain', '모집 마감'));
      main.append(top, el('p', 'chat-room-last', room.last
        ? `${room.last.nickname}: ${room.last.body}`
        : '아직 대화가 없습니다. 첫 인사를 남겨 보세요.'));
      const side = el('div', 'chat-room-side');
      side.append(el('small', 'muted', room.last ? chatShortTime(room.last.created_at) : ''));
      if (room.unread) side.append(el('span', 'nav-badge', room.unread > 99 ? '99+' : String(room.unread)));
      a.append(el('span', 'chat-room-icon', K.icon), main, side);
      return a;
    }));
  } catch (e) {
    listEl.replaceChildren(el('p', 'empty', e.message));
  }
}

/* #/chats/{recruit|study}/{id} - 채팅방 하나.
 * 처음에 최근 메시지를 받고, 보는 동안 CHAT_POLL_MS 마다 ?after=(마지막 id) 로 새 메시지만 받는다.
 * 위로 올라가 [이전 대화 더 보기] 를 누르면 ?before=(첫 id) 로 더 오래된 메시지를 앞에 붙인다. */
async function viewChat(kind, id) {
  markNav('chats');
  setPageTitle('채팅방');
  if (!state.me) return requireLogin('채팅방에 들어가려면 로그인하세요.');
  if (kind !== 'recruit' && kind !== 'study') { location.hash = '#/chats'; return; }

  let data;
  try { data = await api(`/api/chats/${kind}/${id}`); }
  catch (e) {
    /* 멤버가 아니면 모임 화면으로 가는 길을 함께 보여 준다 */
    const box = el('div', 'empty');
    box.append(el('p', null, e.message));
    if (e.code === 'not_member') {
      const a = el('a', 'btn small', '모임 보러 가기');
      a.href = kind === 'recruit' ? `#/recruits/${id}` : `#/studies/${id}`;
      box.append(a);
    }
    return render(box);
  }

  const { room, members } = data;
  const K = CHAT_KIND[room.subkind] || CHAT_KIND.study;
  setPageTitle(`채팅 · ${room.title}`);
  refreshUnread();   /* 이 방을 읽었으니 사이드바 배지를 다시 센다 */

  const node = tpl('tpl-chat');
  const root = node.firstElementChild;
  const top = root.querySelector('[data-tags-top]');
  top.append(el('span', 'tag owner', `${K.icon} ${K.label}`));
  if (room.closed) top.append(el('span', 'tag plain', '모집 마감'));
  root.querySelector('[data-title]').textContent = room.title;
  const ROLE = { mentor: '멘토', mentee: '멘티' };
  root.querySelector('[data-members]').textContent = `멤버 ${members.length}명 · ` + members.map(m =>
    m.nickname + (m.is_owner ? (kind === 'recruit' ? ' (글쓴이)' : ' (방장)') : ROLE[m.role] ? ` (${ROLE[m.role]})` : '')).join(', ');
  const go = root.querySelector('[data-go]');
  go.href = room.link;

  const log = root.querySelector('[data-log]');
  const form = root.querySelector('[data-form]');
  const ta = form.elements.body;
  const sendBtn = form.querySelector('button[type="submit"]');
  const reason = root.querySelector('[data-reason]');

  const shown = new Set();   /* 이미 그린 메시지 id (보내기 응답과 새 메시지 확인이 겹쳐도 한 번만) */
  let firstId = 0, lastId = 0;
  let olderBtn = null;

  /* 바닥 근처를 보고 있는지 (새 메시지가 오면 그때만 아래로 따라 내려간다) */
  const nearBottom = () => log.scrollHeight - log.scrollTop - log.clientHeight < 80;
  const toBottom = () => { log.scrollTop = log.scrollHeight; };

  /* 메시지 하나. prev 는 바로 앞 메시지 (같은 사람이 이어 쓰면 이름을 생략한다) */
  function bubble(m, prev) {
    const sameRun = prev && prev.user_id === m.user_id && prev.created_at.slice(0, 10) === m.created_at.slice(0, 10);
    const row = el('div', 'chat-msg' + (m.mine ? ' mine' : '') + (sameRun ? ' cont' : ''));
    row.dataset.id = m.id;
    if (!m.mine && !sameRun) row.append(el('span', 'chat-name', m.nickname));
    const line = el('div', 'chat-line');
    line.append(el('p', 'chat-bubble', m.body));
    const meta = el('span', 'chat-time', m.created_at.slice(11, 16));
    line.append(meta);
    if (m.mine) {
      const del = el('button', 'chat-del', '삭제');
      del.type = 'button';
      del.title = '내 메시지 지우기';
      del.onclick = async () => {
        if (!(await askConfirm('이 메시지를 지울까요?'))) return;
        try {
          await api(`/api/chat-messages/${m.id}`, { method: 'DELETE' });
          row.remove();
        } catch (e) { toast(e.message); }
      };
      line.append(del);
    }
    row.append(line);
    return row;
  }

  /* 메시지 목록을 노드 묶음으로 (날짜가 바뀌면 구분선). before = 이어 붙일 자리 바로 앞 메시지 */
  function build(list, before) {
    const frag = document.createDocumentFragment();
    let prev = before;
    for (const m of list) {
      if (shown.has(m.id)) continue;
      shown.add(m.id);
      if (!prev || prev.created_at.slice(0, 10) !== m.created_at.slice(0, 10))
        frag.append(el('div', 'chat-day', chatDateLabel(m.created_at.slice(0, 10))));
      frag.append(bubble(m, prev));
      prev = m;
    }
    return { frag, last: prev };
  }

  let lastMsg = null;   /* 화면 맨 아래 메시지 */
  function append(list) {
    if (!list.length) return;
    const stick = nearBottom();
    const { frag, last } = build(list, lastMsg);
    log.querySelector('.chat-empty')?.remove();
    log.append(frag);
    if (last) lastMsg = last;
    lastId = Math.max(lastId, ...list.map(m => m.id));
    if (!firstId) firstId = list[0].id;
    if (stick || list.some(m => m.mine)) toBottom();
  }

  /* [이전 대화 더 보기] */
  function paintOlder(hasMore) {
    if (olderBtn) olderBtn.remove();
    olderBtn = null;
    if (!hasMore) return;
    olderBtn = el('button', 'ghost small chat-older', '이전 대화 더 보기');
    olderBtn.type = 'button';
    olderBtn.onclick = async () => {
      olderBtn.disabled = true;
      try {
        const r = await api(`/api/chats/${kind}/${id}?before=${firstId}`);
        const h = log.scrollHeight;
        const oldFirstDay = log.querySelector('.chat-day');   /* 지금 맨 위 날짜 구분선 */
        const { frag, last } = build(r.messages, null);
        olderBtn.after(frag);
        /* 앞에 붙인 마지막 메시지와 날짜가 같으면 구분선이 두 번 나오므로 아래 것을 지운다 */
        if (last && oldFirstDay && oldFirstDay.textContent === chatDateLabel(last.created_at.slice(0, 10)))
          oldFirstDay.remove();
        if (r.messages.length) firstId = r.messages[0].id;
        log.scrollTop += log.scrollHeight - h;   /* 보던 자리 유지 */
        paintOlder(r.has_more);
      } catch (e) { toast(e.message); olderBtn.disabled = false; }
    };
    log.prepend(olderBtn);
  }

  if (data.messages.length) append(data.messages);
  else log.append(el('p', 'empty small chat-empty', '아직 대화가 없습니다. 첫 인사를 남겨 보세요! 👋'));
  paintOlder(data.has_more);

  /* 입력: Enter 보내기, Shift+Enter 줄바꿈. 한글 조합 중(isComposing)의 Enter 는 글자 확정이라 보내지 않는다 */
  const grow = () => {
    ta.style.height = 'auto';
    ta.style.height = `${Math.min(ta.scrollHeight, 160)}px`;
    ta.style.overflowY = ta.scrollHeight > 160 ? 'auto' : 'hidden';   /* 다 차기 전에는 스크롤 막대를 숨긴다 */
  };
  ta.addEventListener('input', grow);
  ta.addEventListener('keydown', ev => {
    if (ev.key === 'Enter' && !ev.shiftKey && !ev.isComposing && ev.keyCode !== 229) {
      ev.preventDefault();
      form.requestSubmit();
    }
  });
  form.onsubmit = async ev => {
    ev.preventDefault();
    const text = ta.value.trim();
    if (!text || sendBtn.disabled) return;
    sendBtn.disabled = true;
    try {
      const out = await api(`/api/chats/${kind}/${id}/messages`, { method: 'POST', body: JSON.stringify({ body: text }) });
      ta.value = '';
      grow();
      if (out.message) append([out.message]);
    } catch (e) { toast(e.message); }
    sendBtn.disabled = false;
    ta.focus();
  };
  if (state.me.suspended_until) {
    ta.disabled = sendBtn.disabled = true;
    reason.textContent = '계정이 정지되어 메시지를 보낼 수 없습니다. 대화는 볼 수 있습니다.';
    reason.classList.add('blocked');
  }

  render(node);
  toBottom();
  if (!ta.disabled) ta.focus();

  /* 새 메시지 확인. 화면을 떠나면 route() 가 타이머를 멈춘다. 탭이 가려져 있으면 건너뛴다 */
  const myHash = location.hash;
  let busy = false;
  clearInterval(chatTimer);
  chatTimer = setInterval(async () => {
    if (location.hash !== myHash) { clearInterval(chatTimer); return; }
    if (document.hidden || busy) return;
    busy = true;
    try {
      const r = await api(`/api/chats/${kind}/${id}?after=${lastId}`);
      append(r.messages);
    } catch (e) {
      if (e.code === 'not_member' || e.code === 'not_found') {   /* 내보내졌거나 모임이 지워졌다 */
        clearInterval(chatTimer);
        ta.disabled = sendBtn.disabled = true;
        reason.textContent = e.message;
        reason.classList.add('blocked');
      }
    }
    busy = false;
  }, CHAT_POLL_MS);
}

/* ══════════════════════════════════════════════════════ 라우팅 */

/* 주소의 # 부분을 보고 알맞은 화면을 그린다.
 *   '#/posts/12' → slice(2) → 'posts/12' → split('/') → ['posts', '12']
 * 처음 접속(해시 없음)이면 '#/posts' 로 본다. 순서가 중요하다: 구체적인 경로를 먼저 검사한다. */
function route() {
  closeSidebar();   /* 모바일에서 메뉴를 누른 뒤 사이드바가 열린 채로 남지 않게 */
  clearInterval(chatTimer);   /* 채팅방을 떠나면 새 메시지 확인을 멈춘다 */
  refreshUnread();
  const parts = (location.hash || '#/posts').slice(2).split('/');

  /* 히어로는 공모전 목록의 세 탭에만 보인다. 상세·로그인·관리자 화면은 바로 본문부터. */
  const isList = !parts[0] || parts[0] === 'mine' || parts[0] === 'reco' || (parts[0] === 'posts' && !parts[1]);
  document.getElementById('hero').hidden = !isList;

  if (parts[0] === 'login') {
    if (state.me) return logout();   /* 사이드바의 "로그아웃" 메뉴가 #/login 을 가리킨다 */
    return viewAuth();
  }
  if (parts[0] === 'register') {
    if (state.me) { location.hash = '#/posts'; return; }
    return viewAuth(true);
  }
  if (parts[0] === 'admin' && parts[1] === 'users') return viewAdminUsers();
  if (parts[0] === 'admin' && parts[1] === 'content') return viewAdminContent();
  if (parts[0] === 'admin') return viewAdmin();
  if (parts[0] === 'mine') return viewList(true);
  if (parts[0] === 'reco') return viewReco(parts[1]);
  if (parts[0] === 'recruits' && parts[1] === 'new') return viewRecruitNew(parts[2]);
  if (parts[0] === 'recruits' && (parts[1] === 'joined' || parts[1] === 'all')) return viewRecruits(parts[1]);
  if (parts[0] === 'recruits' && parts[1]) return viewRecruit(Number(parts[1]));
  if (parts[0] === 'recruits') return viewRecruits('mine');
  if (parts[0] === 'studies' && parts[1] === 'new') return viewStudyNew();
  if (parts[0] === 'studies' && parts[1] === 'major') return viewStudies('major');
  if (parts[0] === 'studies' && parts[1] === 'reco') return viewStudies('reco', parts[2]);
  if (parts[0] === 'studies' && parts[1]) return viewStudy(Number(parts[1]));
  if (parts[0] === 'studies') return viewStudies('all');
  /* 예전 멘토링 주소 (알림·메일 링크): 멘토링은 그룹 스터디로 옮겼다 */
  if (parts[0] === 'mentoring') { location.hash = parts[1] && parts[1] !== 'new' ? `#/studies/${parts[1]}` : '#/studies'; return; }
  if (parts[0] === 'books' && parts[1] === 'new') return viewBookNew(0);
  if (parts[0] === 'books' && parts[1] === 'major') return viewBooks('major');
  if (parts[0] === 'books' && parts[1] === 'reco') return viewBooks('reco', parts[2]);
  if (parts[0] === 'books' && parts[1] && parts[2] === 'edit') return viewBookNew(Number(parts[1]));
  if (parts[0] === 'books' && parts[1]) return viewBook(Number(parts[1]));
  if (parts[0] === 'books') return viewBooks('all');
  if (parts[0] === 'chats' && parts[2]) return viewChat(parts[1], Number(parts[2]));
  if (parts[0] === 'chats') return viewChats();
  if (parts[0] === 'notifications' && parts[1]) return viewNotification(Number(parts[1]));
  if (parts[0] === 'notifications') return viewNotifications();
  if (parts[0] === 'people' && parts[2]) return viewPeople(parts[1], Number(parts[2]));
  if (parts[0] === 'people') { location.hash = '#/recruits'; return; }   /* 예전 메뉴 주소 */
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
    state.unread = me.unread_notifications || 0;
    state.chatUnread = me.unread_chats || 0;
    state.aiEnabled = !!me.ai_enabled;   /* !! 는 어떤 값이든 true/false 로 바꾼다 */
    state.colleges = depts.colleges;
  } catch (e) {
    renderMessage(`서버에 연결할 수 없습니다: ${e.message}`);
    return;
  }

  paintChrome();
  window.addEventListener('hashchange', route);   /* 이후 주소의 # 가 바뀔 때마다 route() */
  /* 화면을 켜 둔 채 기다리는 동안 온 알림도 보이게 1분마다 확인한다 (탭이 가려져 있으면 건너뜀) */
  setInterval(() => { if (!document.hidden) refreshUnread(); }, 60000);
  route();                                         /* 지금 주소로 첫 화면 */
}

boot();
