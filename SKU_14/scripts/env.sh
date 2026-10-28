# shellcheck shell=bash
# db.env / smtp.env / ai.env (KEY=VALUE 줄) 를 읽어 환경변수로 올린다. 리눅스·맥 스크립트가 source 로 불러 쓴다.
#   . "$SCRIPT_DIR/env.sh"; load_env "$PROJECT"
#
# 파일과 읽는 키:
#   db.env    SKU_DB_HOST, SKU_DB_USER, SKU_DB_PASS   (setup-linux.sh 가 만든다)
#   smtp.env  SKU_SMTP_*, SKU_MAIL_DEV, SKU_SITE_URL  (smtp.env.example 을 복사해 채운다)
#   ai.env    ANTHROPIC_API_KEY                       (선택. AI 분석·추천을 켤 때)
#
# smtp.env 규칙:
#   - 빈 줄과 # 주석 줄은 건너뛴다
#   - 예시 값을 그대로 둔 줄은 설정이 없는 것으로 본다
#   - Google 앱 비밀번호는 'abcd efgh ijkl mnop' 처럼 띄어 보여 주므로 공백을 뺀다
#   - 주소·계정·비밀번호가 모두 있어야 메일 설정으로 인정한다 (SMTP_READY=1)

# 파일을 한 줄씩 읽어 "키 값" 을 콜백 함수 $2 에 넘긴다. 파일이 없으면 1.
_env_each() {
    local file="$1" cb="$2" line k v
    [ -f "$file" ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"                              # Windows 에서 만든 파일의 CR 제거
        line="${line#"${line%%[![:space:]]*}"}"           # 앞 공백 제거
        line="${line%"${line##*[![:space:]]}"}"           # 뒤 공백 제거
        case "$line" in ''|'#'*) continue ;; esac
        case "$line" in *=*) ;; *) continue ;; esac
        k="${line%%=*}"
        v="${line#*=}"
        k="${k%"${k##*[![:space:]]}"}"
        v="${v#"${v%%[![:space:]]*}"}"
        [ -n "$k" ] && "$cb" "$k" "$v"
    done < "$file"
    return 0
}

_db_kv() {
    case "$1" in
        SKU_DB_HOST|SKU_DB_USER|SKU_DB_PASS) export "$1=$2" ;;
    esac
}

_ai_kv() {
    case "$1" in
        ANTHROPIC_API_KEY) export "$1=$2" ;;
    esac
}

_smtp_kv() {
    local k="$1" v="$2"
    case "$v" in *보내는계정*|*앱비밀번호*|*아이디@*) return 0 ;; esac
    [ "$k" = SKU_SMTP_PASS ] && v="${v//[[:space:]]/}"
    case "$k" in
        SKU_SMTP_*|SKU_MAIL_DEV|SKU_SITE_URL) export "$k=$v" ;;
    esac
}

# $1 = 프로젝트 폴더
load_env() {
    local project="$1"

    # 이전에 남아 있던 메일 설정부터 지운다 (파일에서 지운 설정이 환경변수에 남아 쓰이지 않게)
    unset SKU_SMTP_URL SKU_SMTP_USER SKU_SMTP_PASS SKU_SMTP_FROM SKU_SITE_URL

    _env_each "$project/db.env" _db_kv || true
    _env_each "$project/ai.env" _ai_kv || true

    SMTP_READY=0
    if _env_each "$project/smtp.env" _smtp_kv &&
       [ -n "${SKU_SMTP_URL:-}" ] && [ -n "${SKU_SMTP_USER:-}" ] && [ -n "${SKU_SMTP_PASS:-}" ]; then
        SMTP_READY=1
    else
        # 반쯤 된 설정으로 서버가 뜨지 않게 지운다
        unset SKU_SMTP_URL
    fi
}
