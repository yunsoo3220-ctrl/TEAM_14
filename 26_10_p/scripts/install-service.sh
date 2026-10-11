#!/usr/bin/env bash
# 리눅스(systemd)에서 서버를 "서비스" 로 등록한다.
# 부팅하면 자동으로 켜지고, 죽으면 5초 뒤 다시 켜진다. 로그인하지 않아도 돈다.
#
#   scripts/install-service.sh                 80 포트, 다른 기기에서도 접속 (--bind-all)
#   scripts/install-service.sh --port 8080     다른 포트
#   scripts/install-service.sh --local-only    이 컴퓨터에서만 접속 (127.0.0.1)
#   scripts/install-service.sh --no-crawl      매일 아침 공지 자동 수집을 등록하지 않음
#   scripts/install-service.sh --keep-awake    노트북 덮개를 닫거나 가만히 둬도 잠들지 않게 (재부팅 후 적용)
#   scripts/install-service.sh --uninstall     등록한 서비스를 모두 지운다
#
# 등록되는 것 (/etc/systemd/system):
#   sku14.service         C 서버 (scripts/run-server.sh 로 db.env·smtp.env·ai.env 를 읽어 실행)
#   sku14-embed.service   딥러닝 임베딩 서비스 (ml/.venv 가 있을 때만)
#   sku14-crawl.timer     매일 06:00 학교 공지 수집 (server --crawl)
#
# 다시 실행하면 설정을 덮어쓰고 서비스를 재시작한다 (코드를 고치고 make 한 뒤에도 이것만 다시 돌리면 된다).
# 로그 보기: journalctl -u sku14 -f
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
UNIT_DIR=/etc/systemd/system
KEEP_AWAKE_CONF=/etc/systemd/logind.conf.d/sku14-keep-awake.conf
SLEEP_TARGETS="sleep.target suspend.target hibernate.target hybrid-sleep.target"

PORT=80
BIND_ALL=1
CRAWL=1
KEEP_AWAKE=0
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port)       PORT="${2:?--port 뒤에 번호를 주세요}"; shift ;;
        --local-only) BIND_ALL=0 ;;
        --no-crawl)   CRAWL=0 ;;
        --keep-awake) KEEP_AWAKE=1 ;;
        --uninstall)  UNINSTALL=1 ;;
        -h|--help)    sed -n '2,19p' "$0"; exit 0 ;;
        *)            echo "알 수 없는 인자: $1 (--help 참고)" >&2; exit 2 ;;
    esac
    shift
done

if ! command -v systemctl >/dev/null || [ ! -d /run/systemd/system ]; then
    echo "systemd 가 없는 시스템입니다. scripts/start-server.sh 로 띄우세요." >&2
    exit 1
fi

# 서비스를 돌릴 계정: sudo 로 실행했으면 원래 사용자, 아니면 지금 사용자
RUN_USER="${SUDO_USER:-$(id -un)}"
if [ "$RUN_USER" = root ]; then
    echo "root 가 아닌 일반 사용자로 실행하세요 (필요한 곳에서만 sudo 를 씁니다)." >&2
    exit 1
fi

# ---------------------------------------------------------------- 제거
if [ "$UNINSTALL" = 1 ]; then
    for u in sku14-crawl.timer sku14-crawl.service sku14-embed.service sku14.service; do
        sudo systemctl disable --now "$u" 2>/dev/null || true
        sudo rm -f "$UNIT_DIR/$u"
    done
    sudo systemctl daemon-reload
    if [ -f "$KEEP_AWAKE_CONF" ]; then
        sudo rm -f "$KEEP_AWAKE_CONF"
        # shellcheck disable=SC2086
        sudo systemctl unmask $SLEEP_TARGETS
        echo "잠자기 방지 설정도 되돌렸습니다 (재부팅 후 적용)."
    fi
    echo "서비스를 모두 지웠습니다."
    exit 0
fi

# ---------------------------------------------------------------- 점검
if [ ! -x "$PROJECT/server" ]; then
    echo "server 가 없습니다. 먼저 make (또는 scripts/setup-linux.sh) 를 실행하세요." >&2
    exit 1
fi
if [ ! -f "$PROJECT/db.env" ]; then
    echo "db.env 가 없습니다. 먼저 scripts/setup-linux.sh 를 실행하세요." >&2
    exit 1
fi
# start-server.sh 로 띄운 서버가 있으면 포트가 겹치므로 내린다
if [ -f "$PROJECT/logs/server.pid" ]; then
    bash "$SCRIPT_DIR/start-server.sh" --stop || true
fi

SERVER_ARGS="--port $PORT"
[ "$BIND_ALL" = 1 ] && SERVER_ARGS="$SERVER_ARGS --bind-all"

# ---------------------------------------------------------------- 유닛 파일
# 경로에 공백이 있어도 되도록 ExecStart 의 경로는 큰따옴표로 감싼다.
# 스크립트는 bash 로 실행해 실행 권한(+x)이 빠져 있어도 동작하게 한다.
echo "sku14.service 등록 (포트 $PORT, 사용자 $RUN_USER)"
sudo tee "$UNIT_DIR/sku14.service" >/dev/null <<EOF
[Unit]
Description=SKU14 공모전 팀원 모집 커뮤니티 서버
# DB 가 먼저 떠야 한다 (없는 서비스 이름은 무시된다)
After=network-online.target mysql.service mariadb.service
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$PROJECT
ExecStart=/bin/bash "$PROJECT/scripts/run-server.sh" $SERVER_ARGS
# 죽거나 DB 가 늦게 떠서 시작에 실패해도 5초 뒤 다시 시도한다
Restart=always
RestartSec=5
# root 가 아니어도 1024 미만 포트(80)를 열 수 있게 하는 권한 하나만 준다
AmbientCapabilities=CAP_NET_BIND_SERVICE
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF

UNITS="sku14.service"

if [ -x "$PROJECT/ml/.venv/bin/python" ]; then
    echo "sku14-embed.service 등록 (딥러닝 임베딩, 127.0.0.1:8001)"
    sudo tee "$UNIT_DIR/sku14-embed.service" >/dev/null <<EOF
[Unit]
Description=SKU14 문장 임베딩 서비스 (ml/embed_server.py)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$PROJECT
ExecStart="$PROJECT/ml/.venv/bin/python" "$PROJECT/ml/embed_server.py"
Environment=PYTHONUNBUFFERED=1
Restart=on-failure
RestartSec=10
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF
    UNITS="$UNITS sku14-embed.service"
else
    echo "ml/.venv 가 없어 임베딩 서비스는 등록하지 않습니다 (TF-IDF 만으로 추천)."
    sudo systemctl disable --now sku14-embed.service 2>/dev/null || true
    sudo rm -f "$UNIT_DIR/sku14-embed.service"
fi

if [ "$CRAWL" = 1 ]; then
    echo "sku14-crawl.timer 등록 (매일 06:00 학교 공지 수집)"
    sudo tee "$UNIT_DIR/sku14-crawl.service" >/dev/null <<EOF
[Unit]
Description=SKU14 학교 공지 수집 (server --crawl)
After=network-online.target mysql.service mariadb.service
Wants=network-online.target

[Service]
Type=oneshot
User=$RUN_USER
WorkingDirectory=$PROJECT
ExecStart=/bin/bash "$PROJECT/scripts/run-server.sh" --crawl
NoNewPrivileges=true
EOF
    sudo tee "$UNIT_DIR/sku14-crawl.timer" >/dev/null <<EOF
[Unit]
Description=SKU14 학교 공지 매일 수집

[Timer]
OnCalendar=*-*-* 06:00:00
# 그 시각에 꺼져 있었으면 켜진 뒤 바로 한 번 돌린다
Persistent=true
RandomizedDelaySec=10min

[Install]
WantedBy=timers.target
EOF
    UNITS="$UNITS sku14-crawl.timer"
else
    sudo systemctl disable --now sku14-crawl.timer 2>/dev/null || true
    sudo rm -f "$UNIT_DIR/sku14-crawl.timer" "$UNIT_DIR/sku14-crawl.service"
fi

# ---------------------------------------------------------------- 잠자기 방지 (선택)
if [ "$KEEP_AWAKE" = 1 ]; then
    echo "잠자기 방지: 덮개를 닫아도 계속 켜 두고, 절전·최대 절전을 막는다"
    sudo mkdir -p "$(dirname "$KEEP_AWAKE_CONF")"
    sudo tee "$KEEP_AWAKE_CONF" >/dev/null <<EOF
# scripts/install-service.sh --keep-awake 가 만들었다. --uninstall 로 지운다.
[Login]
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=ignore
HandleLidSwitchDocked=ignore
IdleAction=ignore
EOF
    # shellcheck disable=SC2086
    sudo systemctl mask $SLEEP_TARGETS
fi

# ---------------------------------------------------------------- 켜기
sudo systemctl daemon-reload
# shellcheck disable=SC2086
sudo systemctl enable $UNITS
# shellcheck disable=SC2086
sudo systemctl restart $UNITS

sleep 3
echo ""
if systemctl is-active --quiet sku14; then
    if [ "$PORT" = 80 ]; then suffix=""; else suffix=":$PORT"; fi
    echo "서버가 서비스로 돌고 있습니다: http://localhost$suffix/"
    if [ "$BIND_ALL" = 1 ]; then
        ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
        [ -n "$ip" ] && echo "같은 와이파이의 다른 기기: http://$ip$suffix/"
        if command -v ufw >/dev/null && sudo ufw status 2>/dev/null | grep '^Status: active' >/dev/null; then
            echo "방화벽(ufw)이 켜져 있습니다. 다른 기기에서 들어오게 하려면: sudo ufw allow $PORT/tcp"
        fi
    fi
else
    echo "서버가 아직 뜨지 않았습니다. 로그를 확인하세요: journalctl -u sku14 -n 30" >&2
fi
echo ""
echo "상태: systemctl status sku14      로그: journalctl -u sku14 -f"
echo "재시작: sudo systemctl restart sku14   끄기: sudo systemctl stop sku14"
[ "$KEEP_AWAKE" = 1 ] && echo "잠자기 방지 설정은 재부팅한 뒤부터 적용됩니다."
exit 0
