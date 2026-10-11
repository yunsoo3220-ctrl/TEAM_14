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
| libcurl (가입 인증 메일) | MSYS2 → `pacman -S mingw-w64-ucrt-x86_64-curl` |
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
- `04_ai.sql` — AI 분석 결과 표(`post_ai`, `post_ai_departments`)를 기존 DB 에 더하는
  변경분. `01` 을 처음부터 다시 돌렸다면 필요 없다.
- `05_dept_profiles.sql` — 자체 추천 모델의 학과 프로필. `make db` 가 함께 넣는다.
- `06_community.sql` — 자기소개·관심 키워드·팀원 모집 게시판 표를 기존 DB 에 더하는
  변경분 (한 번만). `01` 을 처음부터 돌렸다면 필요 없다.
- `07_interest_tags.sql` — 가입할 때 고르는 관심 키워드 41개. `make db` 가 함께 넣는다.
- `12_study_groups.sql` — 그룹 스터디 표(`study_groups`, `study_members`)를 기존 DB 에 더하는
  변경분 (다시 돌려도 된다). `01` 을 처음부터 돌렸다면 필요 없다.

### 시연용 가상 회원

```powershell
python scripts\seed_demo.py --count 40      # 서버가 떠 있어야 한다
```

학번 `2099000001`~ (존재할 수 없는 입학 연도), 비밀번호 `demo1234!` 인 가상 회원을 만든다.
학과마다 어울리는 관심 키워드·자기소개를 주고, 실제 공모전에 이어진 모집글 14건과 댓글도 만든다.
다시 돌려도 중복으로 만들지 않는다. 지우려면 `scripts\cleanup_demo.sql` 을 실행한다.

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
| `--ai-analyze [--force]` | | 아직 분석하지 않은 게시물을 AI 로 분석하고 종료 (`--force` 는 전체) |

### 학과별 추천 (자체 모델, 무료)

외부 API 없이 서버 안에서 학습·추론하는 추천 모델이다 (`src/ml.c`). 메뉴의
**학과별 추천** (`#/reco/{학과번호}`) 에서 학과를 고르면 관련 공모전을 관련도 구간
(매우 관련 70+ / 관련 50+ / 참고 25+) 으로 묶어 보여 준다. 로그인한 학생은 목록 위에서
내 학과 상위 3건을 바로 보고, 게시물 상세에는 관련 학과 상위 5개가 나온다.

1. **토큰화** — 낱말 + 한글 두 글자 묶음(형태소 분석기 불필요), 공지 상투어는 불용어로 뺀다.
2. **TF-IDF** — 게시물(제목 2배 + 본문)과 학과 프로필(`dept_profiles`)을 벡터로 만든다.
3. **학습 (Rocchio)** — 학과 벡터 = 프로필 + 그 학과에 연결된 게시물 특징.
   연결은 관리자가 대상 학과로 체크한 글(1.0), 그 학과 학생이 댓글을 단 글(0.5).
   잡음을 막으려고 상위 40개 특징만 쓰고, 데이터가 적을수록 반영을 줄인다.
4. **관련도** — 코사인 유사도를 0~100 으로 보정하고, 겹친 낱말을 근거로 보여 준다.

게시물·댓글·체크·프로필이 바뀌면 다음 요청 때 자동으로 다시 학습한다 (게시물 26건 기준 수십 ms).
학과 프로필은 `sql/05_dept_profiles.sql` 에서 고치면 된다.

- **동의어** — 유튜브·숏폼→영상, 코딩·앱개발→개발, 인공지능→AI 처럼 같은 뜻의 낱말을 함께 센다.
- **개인 맞춤** — 로그인한 학생이 내 학과를 보면 학과 벡터에 내 관심 키워드·자기소개 벡터를 더해 매긴다.
- **마감** — 마감일을 아는 공모전 가운데 지난 것은 뒤로 보내고 `closed` 로 표시한다.
- **적합 확률** — 유사도 s 를 `P = 1 / (1 + e^-(a s + b))` 로 바꾼다 (Platt scaling). 정답 쌍(체크·댓글·♥)을
  양성, 그 글의 나머지 학과를 음성으로 로지스틱 회귀를 푸는데, 정답이 적어도 흔들리지 않게 데이터 분포에서
  정한 사전값(모든 쌍의 중앙값 5%, 상위 10% 지점 70%) 쪽으로 당긴다 (MAP). 정답이 쌓일수록 데이터를 따른다.
- **새 글 실시간 예측** — 관리자 화면 "직접 등록" 에서 제목·내용을 쓰는 동안 `POST /api/ml/predict` 가
  재학습 없이 지금 모델로 학과별 적합 확률을 매긴다 (한 번에 약 50~100ms). 학과를 누르거나
  "30% 이상 학과 체크" 로 대상 학과를 고를 수 있고, 게시하면 그 글까지 넣어 다시 학습한다.

### 딥러닝 임베딩 (선택, 무료)

사전학습된 다국어 트랜스포머(`intfloat/multilingual-e5-small`, 384차원)로 글의 **뜻**을 벡터로
만들어 위 모델에 더한다. "AI 숏폼"과 "미디어영상학과"처럼 글자가 겹치지 않아도 연결된다.

```
ml/embed_server.py  (Python, 127.0.0.1:8001)  ← WinHTTP ←  src/embed.c  ←  src/ml.c
```

- 게시물은 `passage`, 학과 프로필·관심사는 `query` 로 임베딩한다. 결과는 `ml/cache/` 의 SQLite 에
  캐시하므로, 다시 학습할 때는 새 글만 계산한다.
- 임베딩 벡터에도 같은 Rocchio 를 적용한다 (학과 = 프로필 + β × 연결된 게시물 평균).
- **최종 유사도 = 0.7 × 임베딩 + 0.3 × TF-IDF**. 트랜스포머 코사인은 관계없는 글끼리도 0.8 근처라서
  전체 평균(mu) 아래는 0, 위는 0~1 로 편 뒤 섞는다. 근거 키워드는 TF-IDF 쪽에서 뽑는다.
- 개인 맞춤 추천, 팀원 찾기, 모집글 "나와 맞는 순" 도 같은 방식으로 섞는다.
- 서비스가 꺼져 있으면 자동으로 TF-IDF 만 쓰고, 나중에 서비스가 뜨면 다음 요청 때 다시 학습한다.
  `/api/ml/info` 의 `embedding_model`, `embedding_dim` 으로 지금 상태를 볼 수 있다.

설치 (한 번, 약 1GB):

```powershell
python -m venv ml\.venv
ml\.venv\Scripts\python.exe -m pip install torch --index-url https://download.pytorch.org/whl/cpu
ml\.venv\Scripts\python.exe -m pip install sentence-transformers pymysql
```

`scripts\start-server.ps1` 은 `ml\.venv` 가 있으면 임베딩 서비스를 함께 띄운다 (`-NoEmbed` 로 끈다).
처음 실행할 때 모델(약 470MB)을 내려받는다. 따로 띄우려면 `ml\.venv\Scripts\python.exe ml\embed_server.py`.

**미세조정** — 관리자 체크와 댓글로 쌓인 정답 쌍으로 모델을 대조학습한다. 게시물의 20%를 떼어 두고
Recall@5·MRR 을 사전학습 모델과 비교해, 나아졌을 때만 `ml/models/finetuned` 에 저장한다.
정답 쌍이 수백 개는 쌓인 뒤에 돌리는 것을 권한다.

```powershell
$env:SKU_DB_PASS="<root 비밀번호>"
ml\.venv\Scripts\python.exe ml\finetune.py --eval-only   # 지금 성능만
ml\.venv\Scripts\python.exe ml\finetune.py               # 미세조정 → 임베딩 서비스 재시작 시 적용
```

### 관심 키워드 · 팀원 모집 · 팀원 찾기

- **가입** — 관심 분야 / 맡고 싶은 역할 / 협업 성향 키워드를 1~8개 고르고, 자기소개를 자유롭게 적는다.
  나중에 **내 프로필** (`#/me`) 에서 바꿀 수 있다.
- **팀원 모집** (`#/recruits`) — 학생이 직접 모집글을 쓴다. 함께 나갈 공모전, 찾는 분야·역할 키워드,
  인원, 마감일을 넣고 댓글로 참여 의사를 받는다. 작성자는 마감·삭제할 수 있다.
  공모전 상세에도 그 공모전으로 올라온 모집글이 보인다. "나와 맞는 순" 정렬은 아래 모델을 쓴다.
- **팀원 찾기** (`#/people`) — 같은 자체 모델로 회원끼리 비교한다. 회원 문서 = 고른 키워드(가중 3) +
  자기소개 + 학과 이름, 모집글 문서 = 찾는 키워드 + 제목 + 본문. TF-IDF 라 많은 사람이 고른
  키워드보다 드문 키워드가 겹칠수록 점수가 높다. 공통 키워드를 근거로 보여 준다.
- **마음에 들어요** — 모집글 카드와 상세에 ♥ 버튼이 있다 (로그인, 내 글 제외, 다시 누르면 취소).
  기존 DB 에는 `sql/08_recruit_likes.sql` 을 한 번 적용한다. 누른 기록은 추천 모델에 이렇게 들어간다.
  - 관련 공모전: 공모전에 연결된 모집글을 좋아하면 (그 공모전, 내 학과) 를 학습 데이터로 쓴다 (댓글과 같은 0.5).
  - 개인 맞춤: 최근 좋아한 모집글 10개의 제목·키워드·공모전 제목을 내 관심사에 더한다.
  - 팀원 찾기·나와 맞는 순: 좋아한 모집글 내용을 회원 문서에 0.5 배로 넣고, 같은 글을 좋아한 회원
    (Jaccard × 0.15) 과 내 글을 좋아했거나 내가 글을 좋아한 회원 (+0.10) 에게 가산점을 준다.
    근거에 "내 모집글에 ♥", "같은 모집글에 ♥" 처럼 보여 준다.
- 다른 회원에게 보이는 것은 표시 이름·학과·키워드·자기소개뿐이다. 실명·전화번호·학번은 나가지 않는다.

### 그룹 스터디

- **만들기** (`#/studies/new`) — 스터디 이름, 정원(2~50), 소개를 정한다. 만든 사람이 방장이자 첫 멤버가 된다.
- **참여** — 로그인한 학생은 상세 화면에서 "참여하기" 로 들어오고 "스터디 나가기" 로 나간다. 정원이 차면 막힌다.
- **멤버 추가** — 방장은 표시 이름이나 학과로 회원을 찾아 바로 넣을 수 있다 (학번·실명으로는 찾지 않는다).
- **멘토·멘티 (선택)** — 방장이 멤버마다 멤버 / 멘토 / 멘티를 고른다. 화면에는 멘토 → 멘티 → 멤버 순으로 묶여 보인다.
- 방장은 나갈 수 없고 스터디를 삭제한다. 관리자는 모든 스터디를 관리할 수 있다.
- 정원 검사와 추가는 한 트랜잭션이라 두 사람이 동시에 마지막 자리에 들어와도 정원을 넘지 않는다.
- 기존 DB 에는 `sql/12_study_groups.sql` 을 한 번 적용한다.
- 자세한 내용(API 응답 예, 설계, 바뀐 파일, 시험 목록)은 [STUDY_GROUPS.md](STUDY_GROUPS.md) 에 있다.

### AI 분석·추천 켜기 (Claude, 유료·선택)

환경변수 `ANTHROPIC_API_KEY` 를 주고 서버를 띄우면 켜진다. 없으면 AI 메뉴와 추천 영역이
화면에서 숨겨지고 나머지 기능은 그대로 동작한다.

```powershell
$env:ANTHROPIC_API_KEY="sk-ant-..."; $env:SKU_DB_PASS="<root 비밀번호>"; .\server.exe --port 8080
```

- 게시물마다 한 번 `claude-opus-5-5` 로 분석해 요약·태그·주최·마감일과 학과별 관련도(0~100)를
  저장한다 (`src/ai.c`). 새로 수집·등록한 글은 백그라운드에서 자동으로 분석한다.
- 학생 화면의 추천은 저장된 점수(50점 이상)로만 계산하므로, 화면을 볼 때마다 API 를 부르지 않는다.
- 관리자 화면의 "AI 게시물 분석" 에서 진행 상황을 보고 다시 분석할 수 있다.
- 안전 분류기가 거절하면 Anthropic 이 권하는 모델로 다시 시도하도록
  `fallbacks: "default"` (`anthropic-beta: server-side-fallback-2026-07-01`) 를 켜 두었다.

---

## 7. 열람 범위와 댓글 권한

과제의 핵심이다. **보는 것과 쓰는 것을 나눈다.**

### 가입과 표시 이름

로그인 화면(`#/login`)은 아이디(학번)·비밀번호만 먼저 보여 주고, 그 아래 작은 **회원가입** 링크로
가입 화면(`#/register`)을 연다.

가입할 때 꼭 받는 것은 **학교 이메일(@skuniv.ac.kr) + 인증 코드, 학번, 이름, 전화번호, 비밀번호,
소속 학과** 다. 관심 키워드와 자기소개는 **선택**이다 (적어 두면 추천이 더 잘 맞는다).
닉네임은 받지 않는다. 게시판에 쓰이는 표시 이름은 서버가 만든다.

#### 학교 이메일 인증

1. `POST /api/register/send-code {email}` — `@skuniv.ac.kr` 로 끝나는 주소만 받는다 (소문자로 맞춤).
   이미 가입한 주소는 거절하고, 같은 주소로는 60초에 한 번만 보낸다. 6자리 코드를 OS 난수원으로 만들어
   메일로 보내고, DB(`email_verifications`)에는 **소금 친 해시만** 남긴다. 코드는 10분 동안 유효하다.
2. `POST /api/register {..., email, code}` — 다른 항목을 모두 확인한 뒤 마지막에 코드를 본다.
   5번 틀리면 코드를 다시 받아야 한다. 가입에 성공하면 코드를 지운다 (한 번만 쓸 수 있다).

메일은 libcurl 로 SMTP 서버를 통해 보낸다 (`src/mailer.c`, TLS 필수). 설정은 프로젝트 폴더의 `smtp.env`
한 곳에 적는다 (`smtp.env.example` 을 복사해 채운다 · 비밀번호가 들어가므로 `.gitignore` 에 있다).

```
SKU_SMTP_URL=smtps://smtp.gmail.com:465        # 네이버 smtps://smtp.naver.com:465
SKU_SMTP_USER=보내는계정@gmail.com
SKU_SMTP_PASS=앱비밀번호16자리                   # Gmail 은 2단계 인증 후 '앱 비밀번호' (로그인 비밀번호 아님)
SKU_SMTP_FROM=보내는계정@gmail.com
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\send-test-mail.ps1 -To 학번@skuniv.ac.kr   # 설정 시험
powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1 -Port 8080 -NoAdmin       # smtp.env 를 읽어 띄움
```

메일 서버가 설정되지 않으면 인증 코드 요청은 `503 mail_not_configured` 로 거절된다 (가입 불가).
시험할 때만 `SKU_MAIL_DEV=1` 을 주면 메일 대신 서버 로그(`logs\server.err.log`)에 코드를 남기는 개발 모드가 된다.
API 응답에는 어떤 경우에도 코드가 실리지 않는다. 기존 DB 에는 `sql/10_email_verify.sql` 을 적용한다.

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
    ?search=공모전        (해커톤 · 경진대회 · 공모 · 대회 · 챌린지 · 아이디어 · 콘테스트)
    &after=<오늘-730일>T00:00:00
    &per_page=100&page=1,2,…
    &_fields=id,date,link,title
```

학교 홈페이지가 WordPress 로 되어 있어 공지사항이 REST API 로 열려 있다.
HTML 을 긁는 대신 이 API 를 쓰면 제목·날짜·링크를 그대로 받을 수 있다.
`after` 파라미터로 **2년 이내 공지**를 페이지를 넘겨 가며 모두 가져온다
(`app_config.crawl.window_days`, 기본 730일 · 기존 DB 는 `sql/09_crawl_more.sql`).
학교 검색은 본문까지 뒤지므로 제목으로 한 번 더 거른다: 공모전·대회·챌린지 등이 들어 있고
수상자 발표·장학금·서포터즈·동아리·교직원 대상이 아닌 글만 남긴다 (`crawler.c` 의 `is_contest_title()`).
게시물의 등록 시각은 학교 공지 게시일로 둔다. 2026-10 기준 약 140건이 모여 추천 모델 학습에 쓰인다.

게시물 본문은 공지마다 `GET /wp-json/wp/v2/notice/{id}?_fields=content` 로 원문을 받아
텍스트로 옮긴다 (`crawler.c` 의 `notice_body_text()`). 문단은 줄로, 목록은 `•`, 표는 `|` 로
잇고 이미지와 링크 주소는 뺀다. 원문 링크는 `posts.source_url` 에 따로 남는다.
원문을 받지 못하면 본문에는 출처(`학교 공지 (공모전, 날짜 게시)`)만 적는다.

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
| GET | `/api/related` | 누구나 | 학과 관련 게시물 (자체 모델). `?department_id=&limit=` |
| GET | `/api/ml/info` | 누구나 | 자체 모델 상태 |
| POST | `/api/ml/retrain` | 관리자 | 자체 모델 강제 재학습 |
| POST | `/api/ml/predict` | 관리자 | `{title, body}` → 학과별 적합 확률 (재학습 없이 실시간) |
| GET | `/api/recommendations` | 로그인 | 내 학과와 관련도가 높은 게시물 (Claude). `?limit=` |
| GET | `/api/tags` | 누구나 | 고를 수 있는 관심 키워드 |
| GET / PUT | `/api/profile` | 로그인 | 내 관심 키워드·자기소개 |
| GET | `/api/members/matches` | 로그인 | 나와 성향이 비슷한 회원 (자체 모델) |
| GET / POST | `/api/recruits` | 누구나 / 로그인 | 모집글 목록 `?status=&tag=&post_id=&sort=new\|match` / 쓰기 |
| GET / DELETE | `/api/recruits/{id}` | 누구나 / 작성자 | 모집글 상세 / 삭제 |
| PUT | `/api/recruits/{id}/status` | 작성자 | 모집 마감·재개 |
| POST | `/api/recruits/{id}/comments` | 로그인 | 모집글 댓글 |
| DELETE | `/api/recruit-comments/{id}` | 본인·관리자 | 모집글 댓글 삭제 |
| POST | `/api/recruits/{id}/like` | 로그인 | 마음에 들어요 (내 글 제외) → `{liked, like_count}` |
| DELETE | `/api/recruits/{id}/like` | 로그인 | 마음에 들어요 취소 |
| GET / POST | `/api/studies` | 누구나 / 로그인 | 스터디 목록 `?mine=1` / 만들기 `{name, description, max_members}` |
| GET / DELETE | `/api/studies/{id}` | 누구나 / 방장 | 스터디 + 멤버 / 삭제 |
| POST | `/api/studies/{id}/join` | 로그인 | 참여하기 |
| GET | `/api/studies/{id}/candidates` | 방장 | 추가할 회원 찾기 `?q=표시이름·학과` |
| POST | `/api/studies/{id}/members` | 방장 | 멤버 추가 `{user_id, role}` |
| PUT | `/api/studies/{id}/members/{uid}` | 방장 | 역할 `{role: member\|mentor\|mentee}` |
| DELETE | `/api/studies/{id}/members/{uid}` | 방장 / 본인 | 내보내기 / 나가기 |
| GET | `/api/ai/status` | 관리자 | AI 분석 진행 상황 |
| POST | `/api/ai/analyze` | 관리자 | AI 분석 시작. `{"force": true}` 면 전체 다시 분석 |

## 10. 파일 구조

```
src/
  main.c         진입점, 명령행 옵션, 기동 점검
  http.c/.h      Winsock HTTP 서버, 요청 파싱, 응답 조립
  api.c/.h       라우팅, 정적 파일, 가입·로그인·세션, 학과 목록
  api_posts.c    게시물 목록/상세, 댓글, 댓글 권한 판정
  api_admin.c    게시물 등록/삭제, 공지 수집·게시
  db.c/.h        MySQL 연결, SQL 조립(%Q 이스케이프), 트랜잭션
  crawler.c/.h   WinHTTP 로 학교 공지 수집, 공지 본문 HTML -> 텍스트
  ai.c/.h        Claude API 로 게시물 분석 (백그라운드 작업)
  api_ai.c       AI 추천·분석 관리 API
  ml.c/.h        자체 추천 모델 (딥러닝 임베딩 + TF-IDF + Rocchio, 외부 API 없음)
  embed.c/.h     임베딩 서비스 클라이언트 (WinHTTP → 127.0.0.1:8001)
  api_ml.c       학과별 추천 API
  api_community.c 관심 키워드·프로필·팀원 찾기·팀원 모집 게시판 API
  api_study.c    그룹 스터디·멤버·멘토/멘티 API
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
