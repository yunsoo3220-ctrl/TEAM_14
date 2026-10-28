#!/usr/bin/env bash
# 서버를 "이 터미널에서" 실행한다 (리눅스·맥). db.env / smtp.env / ai.env 를 읽어 환경변수로 넘긴다.
# systemd 서비스(install-service.sh)와 start-server.sh 도 이 스크립트로 서버를 띄운다.
#
#   scripts/run-server.sh --port 8080                     서버 실행 (Ctrl+C 로 끈다)
#   scripts/run-server.sh --add-admin 20250001 운영자 비밀번호8자이상
#   scripts/run-server.sh --crawl                         학교 공지를 한 번 수집
#   scripts/run-server.sh --test-mail 받을주소@skuniv.ac.kr
#   scripts/run-server.sh --help                          옵션 전체
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"

. "$SCRIPT_DIR/env.sh"
load_env "$PROJECT"

# 메일 속 링크(참여 권유 등)에 쓸 공개 주소. smtp.env 에 SKU_SITE_URL 이 없으면
# Tailscale Funnel 로 공개 중인 주소(https://...ts.net)를 찾아 쓴다. 못 찾으면 서버가 접속 주소로 정한다.
if [ -z "${SKU_SITE_URL:-}" ] && command -v tailscale >/dev/null 2>&1; then
    SKU_SITE_URL="$(timeout 5 tailscale funnel status 2>/dev/null | grep -o 'https://[A-Za-z0-9.-]*' | head -n 1 || true)"
    [ -n "$SKU_SITE_URL" ] && export SKU_SITE_URL
fi

# 정적 파일(www/)을 상대 경로로 찾으므로 프로젝트 폴더에서 실행한다
cd "$PROJECT"
if [ ! -x ./server ]; then
    echo "server 가 없습니다. 먼저 make 로 빌드하세요." >&2
    exit 1
fi
exec ./server "$@"
