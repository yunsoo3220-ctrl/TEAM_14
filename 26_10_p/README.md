# 그룹 스터디 기능

사이드바에 **그룹 스터디** 메뉴(`#/studies`)를 더했다. 학생이 스터디를 만들고, 멤버를 모으고,
원하면 멤버마다 **멘토 / 멘티** 역할을 정할 수 있다.

기존 **팀원 모집** 게시판(`api_community.c`, `#/recruits`)과 같은 구조·코딩 방식으로 만들었다.

---

## 1. 무엇을 할 수 있나

| 기능 | 누가 | 설명 |
| --- | --- | --- |
| 스터디 목록 보기 | 누구나 | 전체 스터디, 또는 "내가 속한 스터디"만 (로그인) |
| 스터디 만들기 | 로그인 | 이름(2~100자), 정원(2~50명), 소개(선택, 1000자). 만든 사람이 **방장**이자 첫 멤버가 된다 |
| 참여하기 | 로그인 학생 | 상세 화면의 "참여하기". 처음 역할은 `멤버` |
| 나가기 | 본인 | "스터디 나가기". 방장은 나갈 수 없다 |
| 멤버 추가 | 방장·관리자 | 표시 이름이나 학과로 회원을 찾아 역할(멤버/멘토/멘티)을 골라 바로 넣는다 |
| 역할 바꾸기 | 방장·관리자 | 멤버마다 선택 상자로 멤버 / 멘토 / 멘티. 방장 자신도 멘토로 정할 수 있다 |
| 내보내기 | 방장·관리자 | 방장이 아닌 멤버를 뺀다 |
| 스터디 삭제 | 방장·관리자 | 멤버 목록도 함께 지워진다 |

- 멘토·멘티는 **선택**이다. 정하지 않으면 모두 `멤버`로 남는다.
- 정원이 차면 참여·추가가 모두 막힌다 (`409 study_full`).
- 관리자 계정은 스터디 멤버가 되지 않지만 모든 스터디를 관리할 수 있다.

## 2. 화면

| 주소 | 화면 | 템플릿 / 함수 |
| --- | --- | --- |
| `#/studies` | 스터디 카드 목록 + "전체 / 내가 속한 스터디" 필터 | `tpl-studies` / `viewStudies()` |
| `#/studies/new` | 스터디 만들기 폼 | `tpl-study-new` / `viewStudyNew()` |
| `#/studies/{id}` | 상세: 정보, 참여/나가기/삭제 버튼, 멤버 목록, 방장용 멤버 추가 | `tpl-study` / `viewStudy()` |

**목록 카드**에는 인원(`3 / 6명`), 멘토·멘티 수, 내 상태("내가 방장", "참여 중 · 멘티")가 태그로 붙는다.
정원이 찼고 내가 멤버가 아닌 스터디는 흐리게 보인다.

**상세 화면의 멤버 목록**은 멘토 → 멘티 → 멤버 순으로 묶어 보여 준다 (빈 묶음은 생략).

- 방장에게는 각 줄에 역할 선택 상자와 [내보내기] 버튼이 보인다.
- 다른 사람에게는 역할 태그만 보인다.

**멤버 추가 카드**는 방장·관리자에게만 보인다.

1. 검색어와 "어떤 역할로 넣을지"를 고르고 찾는다.
2. 결과 줄의 "멘토(으)로 추가" 같은 버튼을 누르면 바로 들어간다.

검색 결과에서는 이미 멤버인 사람과 관리자 계정이 빠진다.

색은 기존 CSS 변수를 그대로 써서 다크 모드에서도 따라 바뀐다.

| 태그 | 색 |
| --- | --- |
| 멘토 | 초록 (`--ok`) |
| 멘티 | 청록 (`--k-hack`) |
| 방장 | 인디고 (`--brand`) |

## 3. DB

새 표 두 개. 새로 설치할 때는 `01_schema.sql` 에 들어 있고, 이미 쓰고 있던 DB 에는
`sql/12_study_groups.sql` 을 적용한다 (`CREATE TABLE IF NOT EXISTS` 라 다시 돌려도 된다).

```sql
CREATE TABLE study_groups (
  id          INT UNSIGNED  NOT NULL AUTO_INCREMENT,
  owner_id    INT UNSIGNED  NOT NULL,                 -- 방장
  name        VARCHAR(100)  NOT NULL,                 -- 스터디 이름
  description VARCHAR(1000) NOT NULL DEFAULT '',
  max_members SMALLINT      NOT NULL DEFAULT 10,      -- 정원 (2~50, 서버에서 검사)
  created_at  DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY ix_study_groups_owner (owner_id),
  CONSTRAINT fk_sg_owner FOREIGN KEY (owner_id) REFERENCES users (id) ON DELETE CASCADE
);

CREATE TABLE study_members (
  group_id  INT UNSIGNED NOT NULL,
  user_id   INT UNSIGNED NOT NULL,
  role      ENUM('member','mentor','mentee') NOT NULL DEFAULT 'member',
  joined_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (group_id, user_id),                    -- 한 스터디에 한 번만
  KEY ix_study_members_user (user_id),
  CONSTRAINT fk_sm_group FOREIGN KEY (group_id) REFERENCES study_groups (id) ON DELETE CASCADE,
  CONSTRAINT fk_sm_user  FOREIGN KEY (user_id)  REFERENCES users (id) ON DELETE CASCADE
);
```

- **역할을 별도 표가 아닌 `study_members.role` 열로 둔 이유**: 한 사람은 한 스터디에서 역할이 하나뿐이라
  (멘토이면서 멘티일 수 없음) 열 하나로 충분하고, 조회가 단순해진다.
- **CASCADE**: 스터디를 지우면 멤버 행이, 회원이 탈퇴하면 그 회원의 멤버 행과 그가 방장인 스터디가 함께 지워진다.
- **방장도 `study_members` 에 들어간다**: 인원 수·역할 계산을 한 표에서 끝내기 위해서다.
  방장인지는 `study_groups.owner_id` 로 판단한다.

## 4. API

모두 `src/api_study.c` 의 `route_study()` 가 처리한다. 오류 형식은 기존과 같다
(`{"error":{"code":"...","message":"..."}}`).

| 메서드 | 경로 | 권한 | 설명 |
| --- | --- | --- | --- |
| GET | `/api/studies` | 누구나 | 목록. `?mine=1` 이면 내가 속한 것만 (최신순, 최대 200) |
| POST | `/api/studies` | 로그인 | 만들기 `{name, description?, max_members?}` → `201 {ok, id}` |
| GET | `/api/studies/{id}` | 누구나 | 상세 + 멤버 |
| DELETE | `/api/studies/{id}` | 방장·관리자 | 삭제 |
| POST | `/api/studies/{id}/join` | 로그인 | 참여하기 |
| GET | `/api/studies/{id}/candidates?q=` | 방장·관리자 | 추가할 회원 검색 (최대 10명) |
| POST | `/api/studies/{id}/members` | 방장·관리자 | 멤버 추가 `{user_id, role?}` |
| PUT | `/api/studies/{id}/members/{uid}` | 방장·관리자 | 역할 바꾸기 `{role}` |
| DELETE | `/api/studies/{id}/members/{uid}` | 방장·관리자 / 본인 | 내보내기 / 나가기 |

### 응답 예

`GET /api/studies`

```json
{"studies":[
  {"id":3,"name":"정보처리기사 실기 스터디","excerpt":"매주 화요일 저녁 온라인...",
   "max_members":6,"created_at":"2026-10-10 15:20","owner_id":12,"owner_nickname":"손*권_9948",
   "member_count":4,"mentor_count":1,"mentee_count":2,"my_role":"mentee"}
]}
```

`my_role` 은 지금 로그인한 사람의 역할이고, 멤버가 아니면 `null` 이다.

`GET /api/studies/3`

```json
{"study":{ ...목록과 같은 형태... },
 "description":"소개 전체",
 "can_manage":true,
 "members":[
   {"user_id":12,"nickname":"손*권_9948","department":"컴퓨터공학과","role":"mentor","is_owner":true,"joined_at":"2026-10-10"},
   {"user_id":40,"nickname":"김*_1234","department":"소프트웨어학과","role":"mentee","is_owner":false,"joined_at":"2026-10-10"}
 ]}
```

`GET /api/studies/3/candidates?q=컴퓨터`

```json
{"users":[{"id":51,"nickname":"이*민_5521","department":"컴퓨터공학과"}]}
```

### 오류 코드

| 상태 | 코드 | 언제 |
| --- | --- | --- |
| 400 | `bad_name` / `bad_description` / `bad_max` | 이름·소개 길이, 정원 범위가 틀림 |
| 400 | `bad_role` | 역할이 member/mentor/mentee 가 아님 |
| 400 | `bad_user` | 추가하려는 회원이 없거나 관리자 계정 |
| 400 | `owner_cannot_leave` | 방장을 빼려 함 |
| 401 | `login_required` | 로그인 필요 |
| 403 | `not_allowed` | 방장·관리자가 아님 |
| 404 | `not_found` / `not_member` | 스터디가 없음 / 멤버가 아님 |
| 409 | `already_member` | 이미 멤버 |
| 409 | `study_full` | 정원이 참 |

## 5. 서버 코드 구조 (`src/api_study.c`)

| 함수 | 하는 일 |
| --- | --- |
| `route_study()` | 경로·메서드를 보고 아래 핸들러로 보낸다. `api_dispatch()` 가 `route_community()` 다음에 부른다 |
| `handle_study_list()` / `handle_study_detail()` | 목록·상세. 같은 열 구성을 `STUDY_SELECT` 매크로로 공유 |
| `handle_study_create()` | 스터디 INSERT + 방장 멤버 INSERT 를 한 트랜잭션으로 |
| `handle_study_join()` / `handle_study_add()` | 둘 다 `add_member()` 를 부른다 |
| `add_member()` | 중복·정원 검사와 INSERT 를 한 트랜잭션으로 묶는다 |
| `handle_study_candidates()` | 표시 이름·학과 이름 `LIKE` 검색 |
| `handle_study_role()` / `handle_study_remove()` | 역할 바꾸기 / 내보내기·나가기 |
| `study_owner()` / `study_manage_ok()` | 방장 id 조회 / 방장·관리자 권한 판정 (1 / 0 / 없으면 -1) |
| `path_two_ids()` | `/api/studies/{id}/members/{uid}` 에서 두 id 를 꺼낸다 (기존 `path_id()` 는 id 하나만 처리) |
| `role_code()` | 입력 역할을 코드 상수로 바꾼다. 모르는 값이면 NULL |

### 설계 메모

- **정원 동시성**: `db_begin()` 은 커밋할 때까지 DB 연결 잠금을 쥔다. `add_member()` 는 그 안에서
  "이미 멤버인지 → 정원 → 현재 인원" 을 확인하고 INSERT 한다.
  두 사람이 동시에 마지막 자리를 눌러도 한 명만 들어간다.
- **SQL 인젝션**: 사용자 문자열은 전부 `%Q`(이스케이프)로 넣는다. 역할은 `role_code()` 가 돌려준
  코드 상수만 `'%s'` 로 넣으므로 입력이 SQL 에 그대로 들어가지 않는다 (`handle_recruit_status()` 와 같은 방식).
- **검색어의 `LIKE` 특수 문자**: `%`, `_`, `\` 앞에 `\` 를 붙여 글자 그대로 찾게 한다.
  `김%` 로 전체 회원을 훑는 것을 막는다.
- **개인정보**: 다른 회원에게는 표시 이름·학과만 나간다. 학번·실명·전화번호는 응답에도, 검색 조건에도 쓰지 않는다.
- **역할 변경의 존재 확인**: MySQL 은 같은 값으로 UPDATE 하면 바뀐 행 수가 0 이다.
  그래서 "멤버가 아님" 판단은 `db_affected()` 가 아니라 별도 `COUNT(*)` 로 한다.
- **권한 검사는 서버에서**: 화면에서 버튼을 숨기는 것은 편의일 뿐이다. 모든 변경 API 가
  `auth_require()` 와 `study_manage_ok()` 로 다시 확인한다.

## 6. 바뀐 파일

| 파일 | 변경 |
| --- | --- |
| `src/api_study.c` | **새 파일**. 그룹 스터디 API 전체 |
| `sql/12_study_groups.sql` | **새 파일**. 기존 DB 에 표를 더하는 변경분 |
| `sql/01_schema.sql` | `study_groups`, `study_members` 표 정의 추가 (`app_config` 앞) |
| `src/api.h` | `route_study()` 선언, 파일 설명에 `api_study.c` 추가 |
| `src/api.c` | `api_dispatch()` 에서 `route_study()` 호출 |
| `www/index.html` | 사이드바 "그룹 스터디" 메뉴, 템플릿 `tpl-studies` · `tpl-study-new` · `tpl-study` |
| `www/app.js` | `studyCard`, `viewStudies`, `viewStudyNew`, `studyMemberRow`, `viewStudy`, `route()` 에 `#/studies` 경로 |
| `www/style.css` | `.tag.mentor/.mentee/.owner`, `.study-member*`, `.study-search`, `.filters.one`, 휴대폰 배치 |
| `README.md` | SQL 목록, "그룹 스터디" 절, API 표, 파일 구조 |
| `Makefile` | 주석의 갱신용 SQL 범위 `08~12` |

`Makefile` 은 `src/*.c` 를 자동으로 모으므로 빌드 규칙은 고치지 않았다.

## 7. 적용 방법

이미 쓰고 있는 DB 라면 (`26_10_p` 폴더, PowerShell):

```powershell
# 1. 떠 있는 서버를 내린다 (실행 중에는 server.exe 를 덮어쓸 수 없다)
powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1 -Stop

# 2. 표 추가 (기존 데이터 유지, 다시 돌려도 됨)
Get-Content sql\12_study_groups.sql -Encoding UTF8 | & "C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe" -u root -p --default-character-set=utf8mb4

# 3. 다시 빌드하고 띄운다
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH; make
powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1 -Port 8080 -NoAdmin -DbPass "DB비밀번호"
```

처음부터 설치한다면 `make db` 만으로 표까지 만들어진다.

## 8. 확인 상태와 시험 목록

- C 코드는 `-Wall -Wextra` 로 경고 없이 컴파일되고 링크까지 된다.
- 2026-10-10 에 실제 서버(8080)·DB 에서 **API 를 시험했고 26개 항목이 모두 통과했다**.
  임시 학생 계정 3개로 돌린 뒤 계정과 스터디를 지웠다.
  - 비로그인 차단, 만들기, 방장 자동 멤버
  - 참여·중복 참여 거부, 정원 초과 거부(`study_full`)
  - 멤버의 역할 변경·검색·삭제 거부(403)
  - 표시 이름 검색, `%` 와일드카드 차단, 멘티로 추가·멘토로 변경, 잘못된 역할 거부
  - 방장 나가기 거부, 나가기·내보내기, 삭제 후 404
- **화면(브라우저)은 아직 직접 눌러 보지 않았다.** `app.js` 문법 검사도 하지 않았다 (이 PC에 Node.js 가 없다).

아래 체크리스트는 브라우저에서 확인할 것들이다.

**만들기·참여**

- [ ] 로그인 안 한 상태로 `#/studies` 목록이 보이고, "스터디 만들기"가 로그인 화면으로 간다
- [ ] 스터디를 만들면 상세로 이동하고, 내가 방장·멤버 1명으로 보인다
- [ ] 다른 계정으로 "참여하기" → 멤버 목록에 나타나고 "스터디 나가기"로 바뀐다

**방장 기능**

- [ ] 방장이 표시 이름·학과로 검색해 멘토/멘티로 바로 추가된다 (이미 멤버인 사람은 검색에서 빠진다)
- [ ] 역할 선택 상자로 멘토 ↔ 멘티 ↔ 멤버를 바꾸면 묶음이 바뀐다
- [ ] 방장이 아닌 사람에게는 선택 상자·내보내기·삭제·멤버 추가 카드가 보이지 않는다
- [ ] 방장 줄에는 [내보내기]가 없다

**정원·삭제**

- [ ] 정원이 차면 참여 버튼이 막히고, API 로 직접 보내도 `409 study_full`
- [ ] 스터디 삭제 후 목록에서 사라진다
- [ ] "내가 속한 스터디" 필터가 동작한다

**화면**

- [ ] 휴대폰 폭에서 검색 폼과 멤버 줄이 세로로 쌓인다
