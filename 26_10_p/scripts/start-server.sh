#!/usr/bin/env bash
# 서버를 백그라운드로 띄운다 (리눅스·맥).
#
#   scripts/start-server.sh                 8080 포트, 이 컴퓨터에서만 접속
#   scripts/start-server.sh --bind-all      같은 와이파이의 다른 기기에서도 접속
#   scripts/start-server.sh --port 80       80 포트 (리눅스에서는 sudo 가 필요하다)
#   scripts/start-server.sh --no-embed      딥러닝 임베딩 서비스 없이 (TF-IDF 만으로 추천)
#   scripts/start-server.sh --stop          떠 있는 서버를 내리기만 한다
#
# 서버 로그는 logs/server.err.log, 임베딩 서비스 로그는 logs/embed.err.log 에 쌓인다.
# 이미 떠 있으면 먼저 내린다.
# 노트북을 서버로 계속 켜 둘 거라면 이 스크립트 대신 install-service.sh 로 서비스 등록을 권한다
# (부팅하면 자동으로 켜지고, 죽으면 다시 켜진다).
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"

PORT=8080
BIND_ALL=0
NO_EMBED=0
STOP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port)     PORT="${2:?--port 뒤에 번호를 주세요}"; shift 2 ;;
        --bind-all) BIND_ALL=1; shift ;;
        --no-embed) NO_EMBED=1; shift ;;
        --stop)     STOP=1; shift ;;
        -h|--help)  sed -n '2,14p' "$0"; exit 0 ;;
        *)          echo "알 수 없는 인자: $1 (--help 참고)" >&2; exit 2 ;;
    esac
done

LOG_DIR="$PROJECT/logs"
SERVER_PID="$LOG_DIR/server.pid"
EMBED_PID="$LOG_DIR/embed.pid"
mkdir -p "$LOG_DIR"

# systemd 서비스로 이미 돌고 있으면 그쪽으로 안내한다 (같은 포트를 두 번 열 수 없다)
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet sku14 2>/dev/null; then
    echo "서버가 systemd 서비스(sku14)로 이미 돌고 있습니다."
    echo "  상태: systemctl status sku14    끄기: sudo systemctl stop sku14    로그: journalctl -u sku14 -f"
    exit 1
fi

# PID 파일에 적힌 프로세스가 우리가 띄운 것이 맞으면 내린다.
#   $1 = PID 파일, $2 = 이름, $3 = 명령줄에 들어 있어야 하는 글자 (PID 재사용으로 엉뚱한 프로세스를 죽이지 않게)
stop_pidfile() {
    local file="$1" name="$2" want="$3" pid i
    [ -f "$file" ] || return 0
    pid="$(cat "$file" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null &&
       ps -p "$pid" -o args= 2>/dev/null | grep -q -- "$want"; then
        echo "실행 중인 $name 중지 (PID $pid)"
        kill "$pid" 2>/dev/null || true
        # 완전히 끝날 때까지 최대 6초 기다린다 (바로 다시 띄우면 포트가 아직 잡혀 있을 수 있다)
        for i in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.3
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || {
                echo "중지 실패 - 다른 사용자(root 등)가 띄운 서버라면 sudo 로 실행하세요." >&2
                return 1
            }
        fi
    fi
    rm -f "$file"
}

stop_pidfile "$EMBED_PID" "임베딩 서비스" "embed_server.py" || exit 1
stop_pidfile "$SERVER_PID" "서버" "server" || exit 1
if [ "$STOP" = 1 ]; then
    echo "중지했습니다."
    exit 0
fi

if [ ! -x "$PROJECT/server" ]; then
    echo "server 가 없습니다. 먼저 make 로 빌드하세요." >&2
    exit 1
fi

# DB 접속 정보 (db.env, git 에 올리지 않는다). 없으면 한 번 묻고 저장한다.
if [ ! -f "$PROJECT/db.env" ]; then
    echo "db.env 가 없습니다. (scripts/setup-linux.sh 를 돌렸다면 자동으로 만들어진다)"
    printf 'MySQL 계정 [root]: '
    read -r db_user
    printf 'MySQL 비밀번호 (화면에 보이지 않음): '
    stty -echo 2>/dev/null; read -r db_pass; stty echo 2>/dev/null; echo
    ( umask 077
      { echo "SKU_DB_USER=${db_user:-root}"; echo "SKU_DB_PASS=$db_pass"; } > "$PROJECT/db.env" )
fi

. "$SCRIPT_DIR/env.sh"
load_env "$PROJECT"

# 딥러닝 임베딩 서비스를 먼저 띄운다. 처음에는 모델(약 470MB)을 내려받느라 오래 걸리지만,
# 서버는 기다리지 않는다. 서비스가 준비되면 다음 추천 요청 때 임베딩을 넣어 다시 학습한다.
VENV_PY="$PROJECT/ml/.venv/bin/python"
if [ "$NO_EMBED" = 0 ]; then
    if [ -x "$VENV_PY" ]; then
        ( cd "$PROJECT" &&
          nohup "$VENV_PY" ml/embed_server.py </dev/null \
                >"$LOG_DIR/embed.log" 2>"$LOG_DIR/embed.err.log" &
          echo $! > "$EMBED_PID" )
        echo "임베딩 서비스 시작 (PID $(cat "$EMBED_PID"), 127.0.0.1:8001) - 로그: logs/embed.err.log"
    else
        echo "ml/.venv 가 없어 임베딩 없이 TF-IDF 만으로 추천합니다 (setup-linux.sh --with-embed 로 설치)."
    fi
fi

# 회원가입 인증 메일 설정 (smtp.env)
if [ "$SMTP_READY" = 1 ]; then
    echo "인증 메일 발송: $SKU_SMTP_URL ($SKU_SMTP_USER)"
elif [ "${SKU_MAIL_DEV:-}" = 1 ]; then
    echo "메일 개발 모드: 인증 코드를 서버 로그에만 남깁니다."
else
    echo "smtp.env 가 없어 회원가입 인증 메일을 보낼 수 없습니다 (smtp.env.example 참고)."
fi

# 서버 실행. stderr(로그 함수들이 쓰는 곳)는 server.err.log 로 간다.
# run-server.sh 가 exec 로 server 가 되므로 $! 가 곧 서버 PID 다.
ARGS=(--port "$PORT")
[ "$BIND_ALL" = 1 ] && ARGS+=(--bind-all)
nohup bash "$SCRIPT_DIR/run-server.sh" "${ARGS[@]}" </dev/null \
      >"$LOG_DIR/server.log" 2>"$LOG_DIR/server.err.log" &
echo $! > "$SERVER_PID"

# 3초 뒤에도 살아 있으면 정상 시작으로 본다 (DB 접속 실패 등은 보통 바로 종료된다)
sleep 3
pid="$(cat "$SERVER_PID")"
if kill -0 "$pid" 2>/dev/null; then
    if [ "$PORT" = 80 ]; then url="http://localhost/"; else url="http://localhost:$PORT/"; fi
    echo "서버 실행 중 (PID $pid, 포트 $PORT) - $url"
    if [ "$BIND_ALL" = 1 ]; then
        ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
        [ -z "$ip" ] && ip="$(ipconfig getifaddr en0 2>/dev/null || true)"
        [ -n "$ip" ] && echo "같은 와이파이의 다른 기기: http://$ip$([ "$PORT" = 80 ] || echo ":$PORT")/"
    fi
    echo "로그: logs/server.err.log    끄기: scripts/start-server.sh --stop"
    exit 0
fi

rm -f "$SERVER_PID"
echo "서버가 바로 종료되었습니다. 로그를 확인하세요:" >&2
tail -n 20 "$LOG_DIR/server.err.log" >&2
if grep -q "Access denied" "$LOG_DIR/server.err.log"; then
    echo "" >&2
    echo "DB 계정이나 비밀번호가 틀렸습니다. db.env 를 고치거나, scripts/setup-linux.sh --skip-packages 를" >&2
    echo "다시 돌리면 계정을 새로 만들어 db.env 에 저장합니다." >&2
elif grep -q "Can't connect" "$LOG_DIR/server.err.log"; then
    echo "" >&2
    echo "MySQL 이 꺼져 있는 것 같습니다: sudo systemctl start mysql (또는 mariadb)" >&2
fi
exit 1
