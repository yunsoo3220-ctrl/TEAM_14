#!/usr/bin/env bash
# 리눅스·맥에서 서버 켜기
#   서비스(install-service.sh)로 등록돼 있으면 서비스를 켜고, 아니면 start-server.sh 로 띄운다.
#   추가 인자는 start-server.sh 로 넘어간다 (예: ./서버켜기.sh --bind-all)
cd "$(dirname "$0")" || exit 1

if [ -f /etc/systemd/system/sku14.service ]; then
    sudo systemctl start sku14 && systemctl --no-pager status sku14 | head -n 5
    exit $?
fi

bash scripts/start-server.sh "$@" || exit 1

# 브라우저 열기 (화면이 있는 환경에서만)
port=8080
while [ $# -gt 0 ]; do [ "$1" = --port ] && port="${2:-8080}"; shift; done
url="http://localhost:$port/"
[ "$port" = 80 ] && url="http://localhost/"
if command -v xdg-open >/dev/null 2>&1 && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    xdg-open "$url" >/dev/null 2>&1 &
elif command -v open >/dev/null 2>&1 && [ "$(uname -s)" = Darwin ]; then
    open "$url"
fi
