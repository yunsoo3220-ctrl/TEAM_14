# 서경대학교 공모전 팀원 모집 커뮤니티

에브리타임처럼 학교 구성원만 쓰는 커뮤니티로, 공모전 팀원 모집 게시판에 초점을 맞췄다.
관리자가 공모전을 올릴 때 **대상 학과를 체크**하고, **체크된 학과 학생만 그 글에 댓글을 쓸 수 있다.**

- 언어: C (C11)
- DB: MySQL 8.x
- 서버: 직접 구현한 HTTP/1.1 서버 (Winsock2) + 정적 파일 제공
- 공지 수집: 학교 홈페이지 WordPress REST API (Windows 내장 WinHTTP)
- 화면: 의존성 없는 HTML/CSS/JS

---

## 1. 준비물

| 항목 | 설치 |
| --- | --- |
| MinGW-w64 gcc | MSYS2 → `pacman -S mingw-w64-ucrt-x86_64-gcc` |
| MySQL 클라이언트 라이브러리 | MSYS2 → `pacman -S mingw-w64-ucrt-x86_64-libmariadbclient` |
| make | MSYS2 → `pacman -S make` |
| MySQL 서버 8.x | 실행 중이어야 한다 |

이 저장소는 `C:/msys64/ucrt64` 설치를 기준으로 맞춰져 있다. 경로가 다르면
`make MYSQL_INC=...` 로 헤더 경로를 넘기면 된다.

## 2. 빌드

```sh
# MSYS2 UCRT64 셸 또는 ucrt64\bin 과 usr\bin 이 PATH 에 있는 셸에서
make
```

`server.exe` 가 만들어진다. 경고 없이 빌드된다.

## 3. DB 준비

```sh
make db
```

비밀번호를 물어본다. 스키마를 건드리기 전에 접속을 먼저 시험하므로,
비밀번호가 틀리면 아무것도 바꾸지 않고 멈춘다.

root 가 아닌 계정을 쓰려면 `make db MYSQL_USER=앱계정`,
`mysql.exe` 경로가 다르면 `make db MYSQL_CLI="D:/.../mysql.exe"` 로 넘긴다.

또는 직접:

```sh
mysql -u root -p --default-character-set=utf8mb4 < sql/01_schema.sql
mysql -u root -p --default-character-set=utf8mb4 < sql/02_seed_departments.sql
```

### `Access denied for user 'root'@'localhost'` 가 나오면

서버까지는 닿았고 비밀번호만 틀린 것이다. 순서대로 확인한다.

1. **설치할 때 정한 비밀번호를 쓴다.** MySQL Installer 가 기본값으로 `1234` 를
   넣어주지는 않는다. MySQL Workbench 나 DBeaver 에 저장된 연결이 있으면
   그 연결의 비밀번호가 맞는 값이다.
2. **맞는지 먼저 시험한다.** 스키마를 적용하기 전에 이것만 돌려본다.

   ```sh
   mysql -u root -p -e "SELECT CURRENT_USER()"
   ```
3. **잊었으면 초기화한다.** 관리자 권한 명령 프롬프트에서:

   ```bat
   net stop MySQL84
   ```
   초기화 명령을 담은 파일을 하나 만든다 (예: `C:\mysql-init.txt`).
   ```sql
   ALTER USER 'root'@'localhost' IDENTIFIED BY '새비밀번호';
   ```
   그 파일을 읽히면서 서버를 띄운다.
   ```bat
   "C:\Program Files\MySQL\MySQL Server 8.4\bin\mysqld.exe" --init-file=C:\mysql-init.txt --console
   ```
   창에 기동 로그가 뜨면 `Ctrl+C` 로 끄고, 파일을 지우고 서비스를 다시 올린다.
   ```bat
   del C:\mysql-init.txt
   net start MySQL84
   ```

- `01_schema.sql` — `sku_contest` 데이터베이스와 표 전체를 다시 만든다 (**기존 데이터가 지워진다**).
- `02_seed_departments.sql` — 대학 8개와 학과 27개를 넣는다.
- `03_display_name.sql` — 이미 쓰고 있던 DB 를 새 가입 방식(전화번호 + 표시 이름)에
  맞추는 변경분. `01` 을 처음부터 다시 돌렸다면 필요 없다.

## 4. 관리자 계정 만들기

공모전을 등록하고 학과를 체크하는 일은 관리자만 할 수 있다.

```sh
SKU_DB_PASS=<root 비밀번호> ./server.exe --add-admin 20250001 운영자 비밀번호8자이상
```

### exe 를 실행할 수 없는 환경이면

[`scripts/make_admin.py`](scripts/make_admin.py) 가 `--add-admin` 과 똑같은 해시를
계산해 준다. 같은 솔트 반복 SHA-256 규칙을 쓰므로 어느 쪽으로 만들든 로그인된다.

```sh
# INSERT 문만 찍어 Workbench 에 붙여넣기
python scripts/make_admin.py 20250001 운영자 비밀번호8자이상

# 바로 DB 에 적용
SKU_DB_PASS=<root 비밀번호> python scripts/make_admin.py 20250001 운영자 비밀번호8자이상 --apply
```

## 5. 주소 연결 (www.sku14.com)

이 서버는 `www.sku14.com` 을 기준 주소로 쓴다. 그 이름이 이 PC를 가리키도록
hosts 파일에 두 줄을 넣어야 한다. 아래 스크립트를 **관리자 권한 PowerShell** 에서
실행하면 알아서 넣어준다.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-domain.ps1
```

넣는 내용은 이것뿐이다 (`C:\Windows\System32\drivers\etc\hosts`).

```
127.0.0.1	www.sku14.com	# sku-contest-board
127.0.0.1	sku14.com	# sku-contest-board
```

되돌리려면 `-Remove` 를 붙여 다시 실행한다.

> 이 설정은 **이 PC 안에서만** 유효하다. 인터넷에서 누구나 `www.sku14.com` 으로
> 들어오게 하려면 실제로 `sku14.com` 도메인을 등록기관에서 구입해 A 레코드를
> 서버 공인 IP로 지정하고, 서버를 `--bind-all` 로 띄우고 80 포트를 열어야 한다.
> 코드는 그 구성에서도 그대로 동작한다.

## 6. 실행

```sh
SKU_DB_PASS=<root 비밀번호> ./server.exe
```
```powershell
$env:SKU_DB_PASS="<root 비밀번호>"; .\server.exe
```

브라우저에서 <http://www.sku14.com> 을 연다. 80 포트는 관리자 권한이 필요할 수 있다.
권한 없이 쓰려면 `--port 8080` 으로 띄우고 <http://www.sku14.com:8080> 으로 접속한다.

`sku14.com` (www 없이) 으로 들어온 요청은 `www.sku14.com` 으로 301 리다이렉트된다.
`127.0.0.1` 이나 `localhost` 로 들어온 요청은 그대로 처리한다.

### 명령행 옵션

| 옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `--port N` | 80 | HTTP 포트 |
| `--bind-all` | 꺼짐 | 모든 인터페이스에 바인드 (기본은 127.0.0.1 전용) |
| `--host NAME` | www.sku14.com | 기준 호스트 이름. www 를 뗀 이름은 여기로 넘긴다 |
| `--db-host H` | 127.0.0.1 | MySQL 호스트 |
| `--db-port N` | 3306 | MySQL 포트 |
| `--db-user U` | root | MySQL 사용자 |
| `--db-pass P` | `$SKU_DB_PASS` | MySQL 비밀번호 |
| `--db-name N` | sku_contest | 데이터베이스 |
| `--webroot DIR` | www | 정적 파일 디렉터리 |
| `--add-admin 학번 이름 비밀번호` | | 관리자 계정을 만들고 종료 |
| `--crawl` | | 공지를 한 번 수집하고 종료 (작업 스케줄러용) |

---

## 7. 열람 범위와 댓글 권한

과제의 핵심이다. **보는 것과 쓰는 것을 나눈다.**

### 가입과 표시 이름

가입할 때 받는 것은 **학번, 이름, 전화번호, 비밀번호, 소속 학과** 다.
닉네임은 받지 않는다. 게시판에 쓰이는 표시 이름은 서버가 만든다.

```
손동권 + 010-9948-9687  ->  손*권_9948
```

- 이름은 가운데 글자를 가린다. 2글자면 `김*`, 3글자면 `손*권`, 4글자면 `남**수`.
  한글은 UTF-8 3바이트라 바이트가 아니라 글자 단위로 센다.
- 전화번호는 가운데 묶음만 쓴다. 11자리는 4자리(`9948`), 10자리는 3자리.
- 실명과 전화번호는 `users` 표에만 있고 **어떤 API 응답에도 나가지 않는다.**
  다른 사람에게 보이는 것은 표시 이름뿐이다.

규칙은 [`src/api.c`](src/api.c) 의 `make_display_name()` 에 있다. 가입 화면의
미리보기는 [`www/app.js`](www/app.js) 의 `displayName()` 이 같은 규칙을 다시
구현한 것이지만, **저장되는 값은 언제나 서버가 만든 것**이다.

표시 이름은 파생값이라 동명이인이 같은 번호 묶음을 쓰면 겹칠 수 있다.
그래서 유일 키를 걸지 않았다. 신원은 학번으로 가린다.

### 열람 — 누구나

공모전 목록과 상세는 **로그인 없이 모두 볼 수 있다.** `/api/posts` 와
`/api/posts/{id}` 는 인증을 요구하지 않는다.

### 댓글 — 대상 학과만

1. 학생은 가입할 때 소속 학과를 하나 고른다 → `users.department_id`.
2. 관리자가 게시물에 대상 학과를 체크한다 → `post_departments`.
3. 댓글을 쓰려 하면 서버가 `post_departments` 에 그 학생의 학과가 있는지 확인한다.

| 상황 | 결과 |
| --- | --- |
| 비로그인 | `401` — 글은 보이지만 댓글은 불가 |
| 관리자 | 항상 가능 |
| 대상 학과 미지정 글 | 로그인한 누구나 가능 |
| 대상 학과 지정 글 + 해당 학과 | 가능 |
| 대상 학과 지정 글 + 다른 학과 | `403 department_not_allowed` |

판정은 [`src/api_posts.c`](src/api_posts.c) 의 `can_comment()` 한 곳에 모여 있다.
화면에서도 쓸 수 없는 글은 입력창이 잠기고 사유가 뜨지만,
**권한 판정은 전부 서버에서 한다.**

### 수집한 공모전은 바로 공개된다

학교에서 가져온 공지는 수집 즉시 게시물이 되어 누구나 볼 수 있다. 대상 학과는
비어 있는 상태로 시작하고, 관리자가 게시물 상세의 **"대상 학과 지정 (관리자)"**
에서 체크하면 그때부터 댓글이 해당 학과로 좁혀진다. 체크를 모두 풀면 다시 열린다.

## 8. 학교 공지 수집

관리자 화면의 "학교 공지 다시 수집" 을 누르면 다음을 호출한다.

```
GET https://www.skuniv.ac.kr/wp-json/wp/v2/notice
    ?search=공모전        (그리고 해커톤)
    &after=<오늘-92일>T00:00:00
    &per_page=50
    &_fields=id,date,link,title
```

학교 홈페이지가 WordPress 로 되어 있어 공지사항이 REST API 로 열려 있다.
HTML 을 긁는 대신 이 API 를 쓰면 제목·날짜·링크를 그대로 받을 수 있다.
`after` 파라미터로 **3개월 이내 공지만** 가져온다 (`app_config.crawl.window_days`, 기본 92일).

받은 공지는 `notices` 표에 `status='pending'` 으로 쌓인다. 관리자가 목록에서
대상 학과를 체크하고 "게시물로 등록" 을 누르면 `posts` + `post_departments` 가 만들어지고
공지는 `published` 로 바뀐다. 쓸모 없는 공지는 "숨기기" 로 `ignored` 처리한다.

수집을 다시 돌려도 이미 있는 공지는 `ext_id` 유일 키로 걸러지고, 제목과 링크만 갱신되며
`status` 는 유지된다. 즉 같은 공지를 두 번 올릴 일이 없다.

## 9. API

| 메서드 | 경로 | 권한 | 설명 |
| --- | --- | --- | --- |
| POST | `/api/register` | 누구나 | 가입 (학번·이름·전화번호·비밀번호·학과) |
| POST | `/api/login` | 누구나 | 로그인, 세션 쿠키 발급 |
| POST | `/api/logout` | 로그인 | 로그아웃 |
| GET | `/api/me` | 누구나 | 현재 로그인 정보 |
| GET | `/api/departments` | 누구나 | 대학·학과 목록 |
| GET | `/api/posts` | 누구나 | 목록. `?department_id=&kind=&mine=1` |
| GET | `/api/posts/{id}` | 누구나 | 상세 + 댓글 + 댓글 권한 |
| POST | `/api/posts` | 관리자 | 게시물 등록 (대상 학과 필수) |
| DELETE | `/api/posts/{id}` | 관리자 | 게시물 삭제 |
| PUT | `/api/posts/{id}/departments` | 관리자 | 대상 학과 다시 체크 |
| POST | `/api/posts/{id}/comments` | 대상 학과 | 댓글 작성 |
| DELETE | `/api/comments/{id}` | 본인·관리자 | 댓글 삭제 |
| GET | `/api/notices` | 관리자 | 수집한 공지. `?status=pending` |
| POST | `/api/notices/crawl` | 관리자 | 학교 공지 수집 실행 |
| POST | `/api/notices/{id}/publish` | 관리자 | 공지를 게시물로 등록 |
| POST | `/api/notices/{id}/ignore` | 관리자 | 공지 숨기기 |
| GET | `/api/admin/summary` | 관리자 | 통계 요약 |

## 10. 파일 구조

```
src/
  main.c         진입점, 명령행 옵션, 기동 점검
  http.c/.h      Winsock HTTP 서버, 요청 파싱, 응답 조립
  api.c/.h       라우팅, 정적 파일, 가입·로그인·세션, 학과 목록
  api_posts.c    게시물 목록/상세, 댓글, 댓글 권한 판정
  api_admin.c    게시물 등록/삭제, 공지 수집·게시
  db.c/.h        MySQL 연결, SQL 조립(%Q 이스케이프), 트랜잭션
  crawler.c/.h   WinHTTP 로 학교 공지 수집, HTML 엔티티 디코딩
  json.c/.h      JSON 파서와 빌더
  sha256.c/.h    SHA-256, 솔트 반복 해싱 비밀번호
  str.c          가변 문자열 버퍼, 로그
  common.h       공통 타입
sql/
  01_schema.sql              표 정의
  02_seed_departments.sql    대학·학과 기초 데이터
www/
  index.html, style.css, app.js
scripts/
  setup-domain.ps1           hosts 에 www.sku14.com 등록 / 제거
  start-server.ps1           서버 기동/중지 (로그를 logs/ 에 남긴다)
  make_admin.py              exe 없이 관리자 계정 만들기 (--add-admin 과 동일한 해시)
logs/
  server.err.log             서버 로그 (start-server.ps1 로 띄웠을 때)
```

## 11. 설계 메모

- **SQL 조립** — `db_sqlf()` 의 `%Q` 가 `mysql_real_escape_string` 으로 감싼다.
  사용자 입력은 전부 `%Q` 나 `%u` 를 거치고, `%s`(그대로 삽입)는 코드 안에서
  검증한 열거값(`kind`)에만 쓴다.
- **비밀번호** — 사용자마다 16바이트 솔트를 두고 SHA-256 을 12,000회 반복한다.
  외부 암호 라이브러리 없이 `src/sha256.c` 에 직접 구현했고, 표준 테스트 벡터로 검증했다.
- **세션** — 랜덤 48자 토큰을 `sessions` 표에 넣고 HttpOnly 쿠키로 내려준다. 12시간 만료.
- **난수** — 솔트와 세션 토큰은 `rand_s()`(OS 난수원)로 만든다. `rand()/srand()` 는
  쓰지 않는다. Windows UCRT 에서 그 상태는 스레드마다 따로라, 요청마다 스레드를
  만드는 이 서버에서는 모든 스레드가 같은 값을 내놓는다 (실제로 서로 다른 사용자가
  같은 솔트를 받고 세션 토큰이 충돌했다).
- **동시성** — 요청마다 스레드를 만들지만 MySQL 연결은 하나를 재진입 임계구역으로
  감싸 직렬화한다. 과제 규모에서는 충분하고, 연결 공유 버그를 피할 수 있다.
- **XSS** — 화면은 `textContent` 로만 값을 넣는다. `innerHTML` 을 쓰지 않는다.
- **경로 탈출** — 정적 파일 경로에서 `..`, `:`, `\` 를 거른다.

## 12. 데이터 출처와 확인이 필요한 부분

학과 목록은 **서경대학교 > 입학·교육 > 대학** (<https://www.skuniv.ac.kr/undergraduate>) 을
기준으로 했다. 다만 각 대학 페이지의 학과 목록은 스크립트로 렌더링되어 정적으로
가져올 수 없었다. 그래서 이렇게 채웠다.

- **이공대학 7개 학과** — 과제 명세에 제시된 목록 그대로.
- **대학 8개** — 홈페이지 네비게이션에서 그대로 확인 (인성교양대학은 전공이 없어 제외).
- **나머지 대학의 학과** — 교육과정편성표와 학부 안내를 대조해 입력.

따라서 아래는 **학교 자료로 다시 확인하는 것을 권한다.** 틀렸으면
`sql/02_seed_departments.sql` 만 고쳐 다시 적용하면 된다.

- `군사학과` 를 인문사회과학대학에 두었다. 융합대학 소속으로 적은 자료도 있다.
- `자유전공학부` 를 창의인재대학에, `미래융합학부1·2` 를 미래융합대학에 두었다.
  최근 개편으로 소속이 바뀌었을 수 있다.
- 공연예술대학·디자인&영상대학의 세부 전공(실용음악 기악/작곡/보컬 등)은
  학부 단위까지만 넣었다.

## 13. 이 PC에서 실행이 막히는 점

빌드는 되지만 **이 PC에서는 `server.exe` 가 실행되지 않는다.** Windows
Smart App Control 이 켜져 있어(enforce) 서명되지 않은 실행 파일을 모두 차단한다.
`hello world` 수준의 exe 도 같은 코드(`0xC0E90002`)로 막힌다.

실행해 보려면 다음 중 하나가 필요하다.

1. Smart App Control 을 끈다 — **한 번 끄면 다시 켤 수 없다.**
   (설정 → 개인 정보 및 보안 → Windows 보안 → 앱 및 브라우저 제어)
2. SAC 가 꺼져 있는 다른 Windows PC에서 빌드해 실행한다.
3. 코드 서명 인증서로 `server.exe` 에 서명한다.

리눅스에서 돌릴 경우 고쳐야 할 부분은 `src/http.c`(Winsock → BSD 소켓),
`src/crawler.c`(WinHTTP → libcurl), `localtime_s`/`_strnicmp` 정도다.
