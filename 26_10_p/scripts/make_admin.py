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

import argparse
import hashlib
import os
import subprocess
import sys

ITERATIONS = 12000          # src/sha256.c 의 PW_ITERATIONS
SALT_HEX_LEN = 32           # PW_SALT_HEX_LEN
MYSQL_DEFAULT = r"C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"


def pw_hash(salt_hex: str, password: str) -> str:
    """src/sha256.c 의 pw_hash() 와 같은 값을 돌려준다."""
    salt = salt_hex.encode("ascii")
    digest = hashlib.sha256(salt + password.encode("utf-8")).digest()
    for _ in range(ITERATIONS):
        digest = hashlib.sha256(digest + salt).digest()
    return digest.hex()


def sql_quote(s: str) -> str:
    return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"


def build_sql(student_no: str, name: str, password: str) -> str:
    salt = os.urandom(SALT_HEX_LEN // 2).hex()
    digest = pw_hash(salt, password)

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
    ap.add_argument("--db-pass", default=os.environ.get("SKU_DB_PASS", ""))
    ap.add_argument("--db-name", default="sku_contest")
    ap.add_argument("--mysql", default=MYSQL_DEFAULT, help="mysql.exe 경로")
    args = ap.parse_args()

    if not args.student_no.isdigit() or not (4 <= len(args.student_no) <= 20):
        print("학번은 숫자 4~20자여야 합니다.", file=sys.stderr)
        return 2
    if len(args.password) < 8:
        print("비밀번호는 8자 이상이어야 합니다.", file=sys.stderr)
        return 2

    sql = build_sql(args.student_no, args.name, args.password)

    if not args.apply:
        print("-- MySQL Workbench 에 붙여넣으세요 (USE sku_contest; 먼저)")
        print(sql)
        return 0

    cmd = [args.mysql, "-u", args.db_user]
    if args.db_pass:
        cmd.append("-p" + args.db_pass)
    cmd += ["-D", args.db_name, "--default-character-set=utf8mb4", "-e", sql]

    done = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    if done.returncode != 0:
        print(done.stderr.strip(), file=sys.stderr)
        return 1

    print("관리자 계정 준비 완료: {} ({})".format(args.student_no, args.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
