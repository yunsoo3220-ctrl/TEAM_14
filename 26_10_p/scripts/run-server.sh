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

# 정적 파일(www/)을 상대 경로로 찾으므로 프로젝트 폴더에서 실행한다
cd "$PROJECT"
if [ ! -x ./server ]; then
    echo "server 가 없습니다. 먼저 make 로 빌드하세요." >&2
    exit 1
fi
exec ./server "$@"
