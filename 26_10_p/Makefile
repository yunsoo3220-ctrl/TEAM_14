# 서경대 공모전 커뮤니티 서버 (Windows / MinGW-w64 gcc)
#
#   make            server.exe 빌드
#   make run        빌드 후 실행
#   make db         스키마와 학과 데이터 적용 (MYSQL_PW 필요)
#   make clean      산출물 삭제
#
# MySQL 클라이언트 헤더/라이브러리는 MSYS2 의 mingw-w64 패키지를 쓴다.
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libmariadbclient

CC       := gcc
MYSQL_INC ?= C:/msys64/ucrt64/include/mysql
MYSQL_CLI ?= C:/Program Files/MySQL/MySQL Server 8.4/bin/mysql.exe

CFLAGS   := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -I$(MYSQL_INC)
LDFLAGS  := -lmysqlclient -lcurl -lws2_32 -lwinhttp

SRCDIR   := src
OBJDIR   := build
TARGET   := server.exe

# MSYS2 셸에서는 TMPDIR 이 비어 gcc 가 C:\WINDOWS 에 임시 파일을 만들려다 실패한다.
# 빌드 디렉터리 안에 임시 폴더를 두어 환경에 의존하지 않게 한다.
export TMPDIR := $(CURDIR)/$(OBJDIR)/tmp
export TMP    := $(TMPDIR)
export TEMP   := $(TMPDIR)

SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all run db check-mysql clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "빌드 완료: $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)/tmp

# 스키마 + 학과 데이터 적용.
#   make db                      비밀번호를 물어본다 (권장)
#   make db MYSQL_PW=비밀번호    명령줄로 넘긴다 (셸 기록에 남는다)
#   make db MYSQL_USER=앱계정    root 외의 계정으로 적용
MYSQL_USER ?= root
PW_ARG     := $(if $(MYSQL_PW),-p$(MYSQL_PW),-p)

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

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)
