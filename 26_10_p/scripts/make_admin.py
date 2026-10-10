#!/usr/bin/env python3
"""관리자 계정을 만드는 INSERT 문을 찍어준다.

server.exe --add-admin 이 하는 일과 똑같은 해시를 계산한다.
이 PC처럼 exe 실행이 막힌 환경에서 Workbench 로 계정을 넣을 때 쓴다.

해시 규칙 (src/sha256.c 의 pw_hash() 와 같아야 한다):
    salt  = 16바이트 난수를 소문자 hex 32자로
    d     = sha256(salt_hex + password)
    12000번 반복: d = sha256(d + salt_hex)
    저장값 = d 를 hex 64자로

사용:
    python scripts/make_admin.py 20250001 운영자 mypassword123
    python scripts/make_admin.py 20250001 운영자 mypassword123 --apply
"""
# 첫 줄의 #!/usr/bin/env python3 (셔뱅)은 유닉스 계열에서 ./make_admin.py 로 바로 실행할 때
# 어떤 인터프리터를 쓸지 알려 준다. Windows 에서는 무시된다.

import argparse        # 명령줄 인자 해석
import hashlib         # SHA-256
import os              # os.urandom - 암호학적으로 안전한 난수
import subprocess      # --apply 일 때 mysql.exe 실행
import sys

ITERATIONS = 12000          # src/sha256.c 의 PW_ITERATIONS
SALT_HEX_LEN = 32           # PW_SALT_HEX_LEN
# r"..." 는 raw 문자열: 역슬래시를 이스케이프로 해석하지 않는다 (Windows 경로에 편리)
MYSQL_DEFAULT = r"C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"


def pw_hash(salt_hex: str, password: str) -> str:
    """src/sha256.c 의 pw_hash() 와 같은 값을 돌려준다."""
    # 주의: C 쪽은 솔트를 "16진 문자열 그대로(ASCII 32바이트)" 해시에 넣는다.
    # 그래서 여기서도 bytes.fromhex 로 풀지 않고 ascii 로 인코딩해 같은 바이트열을 만든다.
    salt = salt_hex.encode("ascii")
    digest = hashlib.sha256(salt + password.encode("utf-8")).digest()   # 첫 회: salt || password
    for _ in range(ITERATIONS):
        digest = hashlib.sha256(digest + salt).digest()                  # 반복: digest || salt
    return digest.hex()                                                  # 소문자 16진 64자


def sql_quote(s: str) -> str:
    # SQL 문자열 리터럴로 감싼다. 역슬래시는 \\ 로, 작은따옴표는 '' 로 이스케이프한다.
    # (역슬래시를 먼저 바꿔야 나중에 넣는 문자가 다시 바뀌지 않는다)
    return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"


def build_sql(student_no: str, name: str, password: str) -> str:
    # 16바이트 난수 → 16진 32자 솔트 (C 의 random_hex(salt, 32) 와 같은 형식)
    salt = os.urandom(SALT_HEX_LEN // 2).hex()
    digest = pw_hash(salt, password)

    # src/api.c 의 api_create_admin 과 같은 문장 (이미 있는 학번이면 관리자로 바꾸고 비밀번호 재설정)
    return (
        "INSERT INTO users "
        "(student_no, name, nickname, department_id, role, pw_salt, pw_hash)\n"
        "VALUES ({}, {}, {}, NULL, 'admin', {}, {})\n"
        "ON DUPLICATE KEY UPDATE name = VALUES(name), role = 'admin',\n"
        "  pw_salt = VALUES(pw_salt), pw_hash = VALUES(pw_hash);"
    ).format(
        sql_quote(student_no), sql_quote(name), sql_quote(name),
        sql_quote(salt), sql_quote(digest),
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="관리자 계정 INSERT 문 생성")
    ap.add_argument("student_no", help="학번 (숫자 4~20자)")
    ap.add_argument("name", help="이름 (닉네임으로도 쓰인다)")
    ap.add_argument("password", help="비밀번호 (8자 이상)")
    ap.add_argument("--apply", action="store_true",
                    help="찍어만 보지 말고 mysql 로 바로 실행한다")
    ap.add_argument("--db-user", default="root")
    ap.add_argument("--db-pass", default=os.environ.get("SKU_DB_PASS", ""))   # 서버와 같은 환경변수
    ap.add_argument("--db-name", default="sku_contest")
    ap.add_argument("--mysql", default=MYSQL_DEFAULT, help="mysql.exe 경로")
    args = ap.parse_args()

    # 서버(api_create_admin)와 같은 검증 규칙
    if not args.student_no.isdigit() or not (4 <= len(args.student_no) <= 20):
        print("학번은 숫자 4~20자여야 합니다.", file=sys.stderr)
        return 2
    if len(args.password) < 8:
        print("비밀번호는 8자 이상이어야 합니다.", file=sys.stderr)
        return 2

    sql = build_sql(args.student_no, args.name, args.password)

    # 기본 동작: SQL 을 화면에 찍기만 한다 (Workbench 에 붙여 넣는 용도)
    if not args.apply:
        print("-- MySQL Workbench 에 붙여넣으세요 (USE sku_contest; 먼저)")
        print(sql)
        return 0

    # --apply: mysql.exe -u 사용자 -p비밀번호 -D DB -e "SQL" 로 바로 실행
    # (인자를 리스트로 넘기면 셸을 거치지 않아 따옴표·특수문자 문제가 없다)
    cmd = [args.mysql, "-u", args.db_user]
    if args.db_pass:
        cmd.append("-p" + args.db_pass)   # mysql 은 -p 와 비밀번호 사이에 공백이 없어야 한다
    cmd += ["-D", args.db_name, "--default-character-set=utf8mb4", "-e", sql]

    done = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if done.returncode != 0:
        print(done.stderr.strip(), file=sys.stderr)
        return 1

    print("관리자 계정 준비 완료: {} ({})".format(args.student_no, args.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
