# 서경대 공모전 커뮤니티 서버 (Windows / MinGW-w64 gcc)
#
#   make            server.exe 빌드
#   make run        빌드 후 실행
#   make db         스키마와 학과 데이터 적용 (MYSQL_PW 필요)
#   make clean      산출물 삭제
#
# MySQL 클라이언트 헤더/라이브러리는 MSYS2 의 mingw-w64 패키지를 쓴다.
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libmariadbclient

# ---------------------------------------------------------------- 변수
# := 는 즉시 대입 (그 자리에서 값을 확정), ?= 는 "아직 값이 없을 때만" 대입
# (?= 로 둔 변수는 make MYSQL_INC=... 처럼 명령줄이나 환경변수로 바꿀 수 있다)
CC       := gcc
MYSQL_INC ?= C:/msys64/ucrt64/include/mysql
MYSQL_CLI ?= C:/Program Files/MySQL/MySQL Server 8.4/bin/mysql.exe

# 컴파일 옵션
#   -std=c11                C11 표준으로 컴파일
#   -O2                     최적화 수준 2
#   -Wall -Wextra           경고를 넉넉히 켠다 (실수를 일찍 발견)
#   -Wno-unused-parameter   쓰지 않는 매개변수 경고는 끈다 (라우터 함수들이 req/res 를 모두 쓰지는 않음)
#   -I...                   mysql.h 를 찾을 폴더
CFLAGS   := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -I$(MYSQL_INC)
# 링크할 라이브러리: MySQL 클라이언트, libcurl(메일), Winsock2(소켓), WinHTTP(HTTPS 클라이언트)
LDFLAGS  := -lmysqlclient -lcurl -lws2_32 -lwinhttp

SRCDIR   := src
OBJDIR   := build
TARGET   := server.exe

# MSYS2 셸에서는 TMPDIR 이 비어 gcc 가 C:\WINDOWS 에 임시 파일을 만들려다 실패한다.
# 빌드 디렉터리 안에 임시 폴더를 두어 환경에 의존하지 않게 한다.
export TMPDIR := $(CURDIR)/$(OBJDIR)/tmp
export TMP    := $(TMPDIR)
export TEMP   := $(TMPDIR)

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

# 링크: 모든 .o 를 묶어 server.exe 를 만든다.  $@ = 대상 이름(server.exe)
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
	@test -f "$(MYSQL_CLI)" || (echo "mysql.exe 를 못 찾았습니다: $(MYSQL_CLI)" && \
	  echo "경로가 다르면 make db MYSQL_CLI=... 로 넘기세요." && false)
	@"$(MYSQL_CLI)" -u $(MYSQL_USER) $(PW_ARG) -e "SELECT CONCAT('접속 성공: ', CURRENT_USER()) AS ok" \
	  || (echo "" && \
	      echo "$(MYSQL_USER) 계정으로 로그인하지 못했습니다. README 3절을 보세요." && false)

# 빌드 후 실행 (기본 80 포트)
run: $(TARGET)
	./$(TARGET)

# 빌드 산출물 삭제 (rm -rf 는 MSYS2 셸 기준)
clean:
	rm -rf $(OBJDIR) $(TARGET)
