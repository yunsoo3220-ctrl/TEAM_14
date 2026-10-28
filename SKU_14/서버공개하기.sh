#!/usr/bin/env bash
# 리눅스에서 서버를 "계속 켜 두고" 인터넷 어디서나 접속되게 연다 (한 번 실행하면 끝).
#
#   ./서버공개하기.sh             처음 설치 ~ 공개 주소 발급까지 한 번에
#   ./서버공개하기.sh --off       공개 주소(Funnel)만 닫는다 (서버는 이 컴퓨터에서 계속 돈다)
#
# 하는 일 (이미 된 단계는 건너뛰므로 몇 번을 다시 실행해도 된다. 코드를 고친 뒤 반영할 때도 이것만 돌리면 된다):
#   1) 패키지 설치·빌드·DB 준비      scripts/setup-linux.sh  (처음 한 번. 이후에는 make 만)
#   2) 관리자 계정이 없으면 만든다    scripts/run-server.sh --add-admin
#   3) systemd 서비스 등록           scripts/install-service.sh --keep-awake  (부팅 시 자동 시작, 잠들지 않게)
#      게시물이 하나도 없으면 학교 공모전 공지를 바로 한 번 수집한다 (이후는 매일 06:00 자동)
#   4) Tailscale Funnel 로 공개      https://<컴퓨터이름>.<tailnet>.ts.net  (접속하는 사람은 아무것도 깔 필요 없음)
#
# 주의: VSCode(snap) 안의 터미널에서는 sudo 가 막혀 있다. Ctrl+Alt+T 로 연 일반 터미널에서 실행한다.
set -euo pipefail

cd "$(dirname "$0")"
PROJECT="$(pwd)"
PORT=80

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[주의] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[실패] %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 점검
[ "$(id -u)" = 0 ] && die "sudo 없이 일반 사용자로 실행하세요: ./서버공개하기.sh (필요한 곳에서만 sudo 를 씁니다)"

if grep -q '^NoNewPrivs:[[:space:]]*1' /proc/$$/status 2>/dev/null; then
    die "이 터미널에서는 sudo 를 쓸 수 없습니다 (VSCode snap 터미널 등).
       Ctrl+Alt+T 로 일반 터미널을 열고 다시 실행하세요:
       cd \"$PROJECT\" && ./서버공개하기.sh"
fi

command -v tailscale >/dev/null || die "tailscale 이 없습니다: curl -fsSL https://tailscale.com/install.sh | sh"

# ---------------------------------------------------------------- --off
if [ "${1:-}" = --off ]; then
    sudo tailscale funnel reset
    echo "공개 주소를 닫았습니다. 서버는 이 컴퓨터(http://localhost)에서 계속 돕니다. 서버까지 끄려면 ./서버끄기.sh"
    exit 0
fi
[ $# -gt 0 ] && die "알 수 없는 인자: $1  (사용법: ./서버공개하기.sh [--off])"

# sudo 비밀번호를 처음에 한 번 받아 둔다
sudo -v || die "sudo 권한이 필요합니다."

# ---------------------------------------------------------------- 1) 설치·빌드·DB
if [ ! -x ./server ] || [ ! -f ./db.env ]; then
    step "1/4 패키지 설치 · 빌드 · DB 준비 (처음 한 번, 몇 분 걸립니다)"
    bash scripts/setup-linux.sh
else
    step "1/4 빌드 (이미 설치돼 있어 바뀐 코드만 다시 컴파일)"
    make
fi

# ---------------------------------------------------------------- 2) 관리자 계정
step "2/4 관리자 계정"
. scripts/env.sh
load_env "$PROJECT"
ADMINS="$(MYSQL_PWD="${SKU_DB_PASS:-}" mysql -h "${SKU_DB_HOST:-localhost}" -u "${SKU_DB_USER:-sku}" -N \
          -e "SELECT COUNT(*) FROM users WHERE role = 'admin'" sku_contest 2>/dev/null || echo '?')"
if [ "$ADMINS" = 0 ]; then
    echo "관리자 계정이 없습니다. 지금 만듭니다 (그냥 Enter 를 누르면 건너뜀)."
    read -r -p "  학번(로그인 ID): " A_NO
    if [ -n "$A_NO" ]; then
        read -r -p "  이름: " A_NAME
        read -r -s -p "  비밀번호(8자 이상, 화면에 안 보임): " A_PW; echo
        bash scripts/run-server.sh --add-admin "$A_NO" "$A_NAME" "$A_PW" || warn "관리자 계정을 만들지 못했습니다. 나중에: scripts/run-server.sh --add-admin 학번 이름 비밀번호"
    else
        echo "건너뜀. 나중에: scripts/run-server.sh --add-admin 학번 이름 비밀번호"
    fi
elif [ "$ADMINS" = '?' ]; then
    warn "관리자 계정 수를 확인하지 못했습니다. 필요하면: scripts/run-server.sh --add-admin 학번 이름 비밀번호"
else
    echo "관리자 계정 ${ADMINS}개가 있습니다."
fi

if [ "${SMTP_READY:-0}" != 1 ]; then
    warn "smtp.env 가 채워지지 않아 회원가입 인증 메일이 나가지 않습니다.
       cp smtp.env.example smtp.env 후 Gmail 앱 비밀번호를 넣고 이 스크립트를 다시 실행하세요."
fi

# ---------------------------------------------------------------- 3) 서비스
step "3/4 서비스 등록 (부팅하면 자동 시작, 죽으면 재시작, 잠들지 않게)"
bash scripts/install-service.sh --port "$PORT" --keep-awake

# 서버가 포트를 열 때까지 기다린다
printf '서버 응답 기다리는 중'
for _ in $(seq 1 30); do
    if (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then echo " OK"; break; fi
    printf '.'; sleep 1
done
(exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null ||
    die "서버가 뜨지 않았습니다. 원인 확인: journalctl -u sku14 -n 50"

# 공지 수집 타이머는 매일 06:00 에만 돌기 때문에, 처음 설치했을 때는 게시물이 비어 있다. 비어 있으면 바로 한 번 수집한다.
POSTS="$(MYSQL_PWD="${SKU_DB_PASS:-}" mysql -h "${SKU_DB_HOST:-localhost}" -u "${SKU_DB_USER:-sku}" -N \
         -e "SELECT COUNT(*) FROM posts" sku_contest 2>/dev/null || echo '?')"
if [ "$POSTS" = 0 ]; then
    echo "게시물이 없어 학교 공모전 공지를 지금 한 번 수집합니다 (1~2분)."
    bash scripts/run-server.sh --crawl || warn "공지 수집에 실패했습니다. 나중에: scripts/run-server.sh --crawl"
fi

# ---------------------------------------------------------------- 4) Funnel
step "4/4 Tailscale Funnel 로 공개"
if ! tailscale status >/dev/null 2>&1; then
    echo "Tailscale 에 로그인합니다. 나오는 링크를 브라우저로 열어 로그인하세요."
    sudo tailscale up
fi
echo "처음이면 'Funnel 허용' 링크가 나옵니다. 브라우저로 열어 승인하면 계속 진행됩니다."
sudo tailscale funnel --bg "$PORT"

URL="$(tailscale funnel status 2>/dev/null | grep -o 'https://[^ ]*' | head -n 1 || true)"

step "완료"
cat <<EOF
공개 주소:  ${URL:-'tailscale funnel status' 로 확인}
이 주소를 공유하면 누구나 (휴대폰 데이터, 다른 와이파이 등) 브라우저로 접속할 수 있습니다.

  상태 / 로그        systemctl status sku14 / journalctl -u sku14 -f
  코드 고친 뒤 반영   ./서버공개하기.sh  (다시 실행)
  공개 주소만 닫기    ./서버공개하기.sh --off
  모두 지우기        scripts/install-service.sh --uninstall && sudo tailscale funnel reset

덮개를 닫아도 안 잠들게 하는 설정은 재부팅 후 완전히 적용됩니다 (sudo reboot). 서버와 공개 주소는 재부팅 뒤 자동으로 다시 켜집니다.
EOF
