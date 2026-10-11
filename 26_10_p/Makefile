# 서경대 공모전 커뮤니티 서버 (Windows: MinGW-w64 gcc / 리눅스: gcc / macOS: clang)
#
#   make            server(.exe) 빌드
#   make run        빌드 후 실행
#   make db         스키마와 학과 데이터 적용 (MYSQL_PW 필요)
#   make clean      산출물 삭제
#
# 필요한 라이브러리: MySQL(MariaDB) 클라이언트, libcurl
#   Windows (MSYS2) : pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libmariadbclient
#                     mingw-w64-ucrt-x86_64-curl
#   Ubuntu/Debian   : sudo apt install build-essential libmariadb-dev libcurl4-openssl-dev
#   Fedora          : sudo dnf install gcc make mariadb-connector-c-devel libcurl-devel
#   macOS (Homebrew): brew install mariadb-connector-c   (curl 은 macOS 에 들어 있다)

# ---------------------------------------------------------------- 변수
# := 는 즉시 대입 (그 자리에서 값을 확정), ?= 는 "아직 값이 없을 때만" 대입
# (?= 로 둔 변수는 make MYSQL_INC=... 처럼 명령줄이나 환경변수로 바꿀 수 있다)

# 컴파일 옵션
#   -std=c11                C11 표준으로 컴파일
#   -O2                     최적화 수준 2
#   -Wall -Wextra           경고를 넉넉히 켠다 (실수를 일찍 발견)
#   -Wno-unused-parameter   쓰지 않는 매개변수 경고는 끈다 (라우터 함수들이 req/res 를 모두 쓰지는 않음)
WARN     := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter

# 운영체제 판별: Windows 는 환경변수 OS=Windows_NT, MSYS2 셸에서는 uname 이 MINGW64_NT.../MSYS_NT... 이다.
UNAME_S  := $(shell uname -s 2>/dev/null)
ifneq ($(filter Windows_NT,$(OS))$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)),)
WINDOWS  := 1
endif

ifdef WINDOWS
# ---- Windows (MSYS2 UCRT64) ----
CC        := gcc
MYSQL_INC ?= C:/msys64/ucrt64/include/mysql
MYSQL_CLI ?= C:/Program Files/MySQL/MySQL Server 8.4/bin/mysql.exe
CFLAGS    := $(WARN) -I$(MYSQL_INC)
# 링크할 라이브러리: MySQL 클라이언트, libcurl(메일·HTTPS), Winsock2(소켓)
LDFLAGS   := -lmysqlclient -lcurl -lws2_32
TARGET    := server.exe

# MSYS2 셸에서는 TMPDIR 이 비어 gcc 가 C:\WINDOWS 에 임시 파일을 만들려다 실패한다.
# 빌드 디렉터리 안에 임시 폴더를 두어 환경에 의존하지 않게 한다.
export TMPDIR := $(CURDIR)/build/tmp
export TMP    := $(TMPDIR)
export TEMP   := $(TMPDIR)
else
# ---- 리눅스 / macOS ----
CC        ?= cc
MYSQL_CLI ?= mysql
# mysql.h 위치와 링크 옵션은 설치된 도구가 알려 준다 (배포판마다 경로가 다르다).
#   MariaDB Connector/C → mariadb_config,  MySQL 클라이언트 → mysql_config
# 둘 다 없으면 pkg-config 로 찾는다. 다른 것을 쓰려면 make MYSQL_CONFIG=/경로/mysql_config
MYSQL_CONFIG ?= $(shell command -v mariadb_config 2>/dev/null || command -v mysql_config 2>/dev/null)
ifneq ($(strip $(MYSQL_CONFIG)),)
MYSQL_CFLAGS := $(shell $(MYSQL_CONFIG) --include)
MYSQL_LIBS   := $(shell $(MYSQL_CONFIG) --libs)
else
MYSQL_CFLAGS := $(shell pkg-config --cflags libmariadb 2>/dev/null || pkg-config --cflags mysqlclient 2>/dev/null)
MYSQL_LIBS   := $(shell pkg-config --libs libmariadb 2>/dev/null || pkg-config --libs mysqlclient 2>/dev/null)
endif
CURL_CFLAGS  := $(shell curl-config --cflags 2>/dev/null || pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS    := $(shell curl-config --libs 2>/dev/null || pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
CFLAGS    := $(WARN) $(MYSQL_CFLAGS) $(CURL_CFLAGS)
# -pthread: 요청마다 스레드를 띄운다,  -lm: ml.c 의 sqrt/log/exp
LDFLAGS   := $(MYSQL_LIBS) $(CURL_LIBS) -pthread -lm
TARGET    := server
endif

SRCDIR   := src
OBJDIR   := build

# src/*.c 를 모두 찾고, 각각 build/이름.o 로 바꾼 목록을 만든다
#   예: src/http.c → build/http.o
SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

# .PHONY: 파일 이름이 아닌 "동작 이름" 대상. 같은 이름의 파일이 있어도 항상 실행된다.
.PHONY: all run db check-mysql clean

# ---------------------------------------------------------------- 빌드 규칙
# 형식:  대상: 의존 파일들
#        <탭>명령
# 의존 파일이 대상보다 새로우면(수정됐으면) 명령을 다시 실행한다 → 바뀐 파일만 다시 컴파일

all: $(TARGET)

# 링크: 모든 .o 를 묶어 server(.exe) 를 만든다.  $@ = 대상 이름
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "빌드 완료: $@"

# 패턴 규칙: build/X.o 는 src/X.c 로 컴파일한다.  $< = 첫 번째 의존 파일(src/X.c)
# '| $(OBJDIR)' 는 순서 전용 의존: 폴더가 먼저 있어야 하지만, 폴더 시각이 바뀌었다고 다시 컴파일하지는 않는다.
# 참고: 헤더(.h)는 의존 목록에 없으므로 헤더만 고쳤을 때는 make clean 후 다시 빌드하는 것이 안전하다.
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 빌드 폴더와 그 안의 임시 폴더 만들기 (@ 를 붙이면 명령 자체는 화면에 찍지 않는다)
$(OBJDIR):
	@mkdir -p $(OBJDIR)/tmp

# 스키마 + 학과 데이터 적용.
#   make db                      비밀번호를 물어본다 (권장)
#   make db MYSQL_PW=비밀번호    명령줄로 넘긴다 (셸 기록에 남는다)
#   make db MYSQL_USER=앱계정    root 외의 계정으로 적용
MYSQL_USER ?= root
# MYSQL_PW 가 있으면 -p비밀번호, 없으면 -p 만 (mysql 이 비밀번호를 물어본다)
PW_ARG     := $(if $(MYSQL_PW),-p$(MYSQL_PW),-p)

# 스키마(01) → 학과(02) → 학과 프로필(05) → 관심 키워드(07) 순서로 적용 (03,04,06,08~12 는 기존 DB 갱신용이라 새로 만들 때는 필요 없음)
# 주의: 01_schema.sql 은 DB 를 통째로 지우고 다시 만든다.
db: check-mysql
	"$(MYSQL_CLI)" -u $(MYSQL_USER) $(PW_ARG) --default-character-set=utf8mb4 < sql/01_schema.sql
	"$(MYSQL_CLI)" -u $(MYSQL_USER) $(PW_ARG) --default-character-set=utf8mb4 < sql/02_seed_departments.sql
	"$(MYSQL_CLI)" -u $(MYSQL_USER) $(PW_ARG) --default-character-set=utf8mb4 < sql/05_dept_profiles.sql
	"$(MYSQL_CLI)" -u $(MYSQL_USER) $(PW_ARG) --default-character-set=utf8mb4 < sql/07_interest_tags.sql
	@echo "DB 준비 완료"

# 접속만 먼저 시험해 비밀번호 문제를 스키마 적용 전에 알려준다.
check-mysql:
	@(test -f "$(MYSQL_CLI)" || command -v "$(MYSQL_CLI)" >/dev/null 2>&1) || \
	  (echo "mysql 클라이언트를 못 찾았습니다: $(MYSQL_CLI)" && \
	   echo "경로가 다르면 make db MYSQL_CLI=... 로 넘기세요." && false)
	@"$(MYSQL_CLI)" -u $(MYSQL_USER) $(PW_ARG) -e "SELECT CONCAT('접속 성공: ', CURRENT_USER()) AS ok" \
	  || (echo "" && \
	      echo "$(MYSQL_USER) 계정으로 로그인하지 못했습니다. README 3절을 보세요." && false)

# 빌드 후 실행. Windows 는 기본 80 포트, 리눅스·맥은 root 없이 열 수 있는 8080 포트.
#   make run RUN_ARGS="--port 9000 --bind-all"  처럼 옵션을 바꿀 수 있다
ifdef WINDOWS
RUN_ARGS ?=
else
RUN_ARGS ?= --port 8080
endif
run: $(TARGET)
	./$(TARGET) $(RUN_ARGS)

# 빌드 산출물 삭제 (rm -rf 는 MSYS2 셸·리눅스·맥 모두 된다)
clean:
	rm -rf $(OBJDIR) $(TARGET)
