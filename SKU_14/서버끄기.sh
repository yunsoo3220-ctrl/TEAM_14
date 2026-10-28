#!/usr/bin/env bash
# 리눅스·맥에서 서버 끄기
#   서비스로 등록돼 있으면 서비스를 끈다 (다음 부팅 때는 다시 켜진다. 아예 지우려면 install-service.sh --uninstall)
cd "$(dirname "$0")" || exit 1

if [ -f /etc/systemd/system/sku14.service ]; then
    sudo systemctl stop sku14 && echo "중지했습니다 (서비스 sku14)."
    exit $?
fi

bash scripts/start-server.sh --stop
