#!/usr/bin/env bash
# 리눅스·맥에서 처음 한 번 돌리는 준비 스크립트.
#   1) 패키지 설치 (gcc, make, MySQL/MariaDB 서버와 클라이언트 라이브러리, libcurl)
#   2) make 로 server 빌드
#   3) DB(sku_contest) 가 없으면 만들고 스키마·학과 데이터를 넣는다 (있으면 건드리지 않는다)
#   4) 서버 전용 DB 계정 sku 를 만들고 접속 정보를 db.env 에 저장한다
#
#   scripts/setup-linux.sh                  전부
#   scripts/setup-linux.sh --skip-packages  패키지 설치는 건너뛴다 (이미 깔려 있을 때)
#   scripts/setup-linux.sh --with-embed     딥러닝 임베딩용 파이썬 환경(ml/.venv)도 만든다 (약 1GB)
#   scripts/setup-linux.sh --hosts          /etc/hosts 에 www.sku14.com 을 이 컴퓨터로 등록
#
# 지원: Ubuntu/Debian(apt), Fedora(dnf), macOS(Homebrew). 다른 배포판은 패키지만 직접 깔고 --skip-packages.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
OS_NAME="$(uname -s)"

SKIP_PACKAGES=0
WITH_EMBED=0
ADD_HOSTS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-packages) SKIP_PACKAGES=1 ;;
        --with-embed)    WITH_EMBED=1 ;;
        --hosts)         ADD_HOSTS=1 ;;
        -h|--help)       sed -n '2,13p' "$0"; exit 0 ;;
        *)               echo "알 수 없는 인자: $1 (--help 참고)" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n== %s\n' "$*"; }

# ---------------------------------------------------------------- 1) 패키지
if [ "$SKIP_PACKAGES" = 0 ]; then
    step "패키지 설치"
    if [ "$OS_NAME" = Darwin ]; then
        command -v brew >/dev/null || { echo "Homebrew 가 필요합니다: https://brew.sh" >&2; exit 1; }
        brew install mariadb-connector-c mysql python
        brew services start mysql
    elif command -v apt-get >/dev/null; then
        sudo apt-get update
        # Ubuntu 는 mysql-server 가 있고, Debian 은 MariaDB 만 있다 (이 서버는 둘 다 된다)
        # (grep -q 는 일찍 끝나 pipefail 에 걸릴 수 있어 출력을 버리는 방식으로 쓴다)
        if apt-cache policy mysql-server 2>/dev/null | grep 'Candidate: [0-9]' >/dev/null; then
            DB_PKG=mysql-server
        else
            DB_PKG=mariadb-server
        fi
        sudo apt-get install -y build-essential pkg-config libmariadb-dev libcurl4-openssl-dev \
                                "$DB_PKG" python3 python3-venv
    elif command -v dnf >/dev/null; then
        sudo dnf install -y gcc make pkgconf-pkg-config mariadb-connector-c-devel libcurl-devel \
                            mariadb-server python3
        sudo systemctl enable --now mariadb
    else
        echo "이 배포판은 자동 설치를 지원하지 않습니다. 아래를 직접 설치한 뒤 --skip-packages 로 다시 실행하세요." >&2
        echo "  gcc, make, MariaDB Connector/C(또는 libmysqlclient) 개발 패키지, libcurl 개발 패키지," >&2
        echo "  MySQL 또는 MariaDB 서버, python3" >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------- 2) 빌드
step "빌드"
make -C "$PROJECT"

# ---------------------------------------------------------------- 3) DB
# 관리자 권한으로 mysql 을 실행한다.
#   리눅스: root 계정이 비밀번호 대신 OS 계정으로 인증(auth_socket/unix_socket)하므로 sudo mysql
#   맥(Homebrew): root 비밀번호가 비어 있으므로 mysql -u root
# 둘 다 안 되면 root 비밀번호를 물어본다.
ADMIN_MODE=""
admin_mysql() {
    case "$ADMIN_MODE" in
        sudo)  sudo mysql "$@" ;;
        plain) mysql -u root "$@" ;;
        pass)  MYSQL_PWD="$ROOT_PW" mysql -u root "$@" ;;
    esac
}

step "DB 관리자 접속 확인"
# 방금 설치한 DB 서버가 아직 뜨는 중일 수 있으므로 몇 초 기다리며 시도한다
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if [ "$OS_NAME" != Darwin ] && sudo mysql -e "SELECT 1" >/dev/null 2>&1; then
        ADMIN_MODE=sudo; break
    elif mysql -u root -e "SELECT 1" >/dev/null 2>&1; then
        ADMIN_MODE=plain; break
    fi
    sleep 1
done
if [ -z "$ADMIN_MODE" ]; then
    printf 'MySQL root 비밀번호 (화면에 보이지 않음): '
    stty -echo 2>/dev/null; read -r ROOT_PW; stty echo 2>/dev/null; echo
    ADMIN_MODE=pass
    if ! admin_mysql -e "SELECT 1" >/dev/null; then
        echo "root 로 접속하지 못했습니다. MySQL 이 켜져 있는지(sudo systemctl status mysql) 확인하세요." >&2
        exit 1
    fi
fi
echo "접속 방법: $ADMIN_MODE"

step "스키마"
if [ "$(admin_mysql -N -e "SELECT COUNT(*) FROM information_schema.schemata WHERE schema_name = 'sku_contest'")" = 0 ]; then
    # 스키마(01) → 학과(02) → 학과 프로필(05) → 관심 키워드(07)  (Makefile 의 make db 와 같은 순서)
    for f in 01_schema 02_seed_departments 05_dept_profiles 07_interest_tags; do
        echo "적용: sql/$f.sql"
        admin_mysql --default-character-set=utf8mb4 < "$PROJECT/sql/$f.sql"
    done
else
    echo "sku_contest 가 이미 있어 스키마는 건드리지 않습니다."
    echo "(처음부터 다시 만들려면: sudo mysql < sql/01_schema.sql ... - 데이터가 모두 지워진다)"
fi

step "서버 전용 DB 계정 (sku)"
# 이미 db.env 의 계정으로 접속되면 그대로 쓴다
. "$SCRIPT_DIR/env.sh"
load_env "$PROJECT"
if [ -n "${SKU_DB_USER:-}" ] && [ "${SKU_DB_USER}" != root ] &&
   MYSQL_PWD="${SKU_DB_PASS:-}" mysql -h "${SKU_DB_HOST:-127.0.0.1}" -u "$SKU_DB_USER" \
         -e "SELECT 1" sku_contest >/dev/null 2>&1; then
    echo "db.env 의 계정($SKU_DB_USER)으로 접속됩니다. 그대로 씁니다."
else
    # 난수 32자 비밀번호. 'localhost' 는 유닉스 소켓, '127.0.0.1' 은 TCP 접속용 계정이다.
    DB_PW="$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
    admin_mysql <<SQL
CREATE USER IF NOT EXISTS 'sku'@'localhost' IDENTIFIED BY '$DB_PW';
CREATE USER IF NOT EXISTS 'sku'@'127.0.0.1' IDENTIFIED BY '$DB_PW';
ALTER USER 'sku'@'localhost' IDENTIFIED BY '$DB_PW';
ALTER USER 'sku'@'127.0.0.1' IDENTIFIED BY '$DB_PW';
GRANT ALL PRIVILEGES ON sku_contest.* TO 'sku'@'localhost';
GRANT ALL PRIVILEGES ON sku_contest.* TO 'sku'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
    # 서버는 SKU_DB_HOST=localhost 이면 TCP 대신 유닉스 소켓으로 접속한다.
    # (MySQL 8 의 기본 인증 방식은 암호화되지 않은 TCP 로 처음 로그인할 때 TLS 를 요구할 수 있어서 소켓이 안전하다)
    ( umask 077
      { echo "# 서버 DB 접속 정보 (scripts/setup-linux.sh 가 만들었다. git 에 올리지 않는다)"
        echo "SKU_DB_HOST=localhost"
        echo "SKU_DB_USER=sku"
        echo "SKU_DB_PASS=$DB_PW"; } > "$PROJECT/db.env" )
    echo "계정 sku 를 만들고 db.env 에 저장했습니다 (권한 600)."
fi

# ---------------------------------------------------------------- 선택: 임베딩
if [ "$WITH_EMBED" = 1 ]; then
    step "딥러닝 임베딩 환경 (ml/.venv)"
    python3 -m venv "$PROJECT/ml/.venv"
    PIP="$PROJECT/ml/.venv/bin/python -m pip"
    $PIP install --upgrade pip
    if [ "$OS_NAME" = Darwin ]; then
        $PIP install torch
    else
        # GPU 없는 노트북이면 CPU 판 PyTorch 가 훨씬 작다
        $PIP install torch --index-url https://download.pytorch.org/whl/cpu
    fi
    $PIP install sentence-transformers pymysql
fi

# ---------------------------------------------------------------- 선택: hosts
if [ "$ADD_HOSTS" = 1 ]; then
    step "/etc/hosts 에 www.sku14.com 등록"
    if grep -q 'www\.sku14\.com' /etc/hosts; then
        echo "이미 등록되어 있습니다."
    else
        printf '127.0.0.1  www.sku14.com\n127.0.0.1  sku14.com\n' | sudo tee -a /etc/hosts >/dev/null
        echo "등록했습니다 (이 컴퓨터에서만 유효)."
    fi
fi

step "준비 끝"
cat <<EOF
다음 단계:
  1. 관리자 계정 만들기
       scripts/run-server.sh --add-admin 20250001 운영자 비밀번호8자이상
  2. (선택) 인증 메일: cp smtp.env.example smtp.env 후 값 채우기
  3. 시험 실행 (Ctrl+C 로 끈다)
       scripts/run-server.sh --port 8080        → http://localhost:8080
  4. 계속 켜 두기 (리눅스, 부팅 시 자동 시작)
       scripts/install-service.sh
자세한 내용: 서버실행방법-리눅스.md
EOF
