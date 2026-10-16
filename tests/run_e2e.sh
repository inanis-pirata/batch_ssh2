#!/bin/sh
# End-to-end tests for batchssh2 against a real sshd.
# Usage: sh tests/run_e2e.sh <path-to-binary> <user> <password>
BIN=${1:?binary}
U=${2:?user}
P=${3:?password}
H=localhost
PASS=0; FAIL=0

ck() { # name expected actual
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "PASS: $1";
    else FAIL=$((FAIL+1)); echo "FAIL: $1 (expected [$2] got [$3])"; fi
}

echo "== 1. basic exec =="
OUT=$("$BIN" -H $H -u "$U" -p "$P" -c 'echo hello-world' 2>/dev/null)
ck "exec stdout" "hello-world" "$(echo "$OUT" | grep -m1 hello-world | tr -d '\r')"

echo "== 2. script mode with CRLF line endings =="
printf 'echo CRLF_OK\r\necho second-line\r\n' > /tmp/bs2_crlf.sh
OUT=$("$BIN" -H $H -u "$U" -p "$P" -c @/tmp/bs2_crlf.sh 2>/dev/null)
if echo "$OUT" | grep -q 'CRLF_OK' && echo "$OUT" | grep -q 'second-line' && ! echo "$OUT" | grep -q 'command not found'; then
    ck "crlf script" "ok" "ok"
else
    ck "crlf script" "ok" "bad: $OUT"
fi

echo "== 3. json output is valid & complete =="
OUT=$("$BIN" -H $H -u "$U" -p "$P" -c 'true' --json 2>/dev/null)
echo "$OUT" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[0]["status"]=="success" and d[0]["exit_code"]==0' 2>/dev/null
ck "json parse" "0" "$?"
OUT=$("$BIN" -H $H -u "$U" -p "$P" --json -c true 2>&1 >/dev/null)
ck "json suppresses progress" "" "$OUT"

echo "== 4. csv output =="
OUT=$("$BIN" -H $H -u "$U" -p "$P" -c 'echo a,b' --csv 2>/dev/null)
ck "csv header" "Host,Status,ExitCode,TimeCost(ms),Stdout,Stderr,ErrorMsg" "$(echo "$OUT" | head -1)"
echo "$OUT" | python3 -c '
import csv, sys
rows = list(csv.reader(sys.stdin))
assert len(rows) == 2, rows
assert rows[0][0] == "Host"
assert "a,b" in rows[1][4], rows[1]
' 2>/dev/null
ck "csv quoting (rfc4180 round-trip)" "0" "$?"

echo "== 5. upload file + verify content remotely =="
echo uploaded-content > /tmp/bs2_up.txt
"$BIN" -H $H -u "$U" -p "$P" --upload /tmp/bs2_up.txt:/tmp/bs2_dir/up.txt >/dev/null 2>&1
ck "upload rc" "0" "$?"
OUT=$("$BIN" -H $H -u "$U" -p "$P" -c 'cat /tmp/bs2_dir/up.txt' 2>/dev/null)
ck "uploaded content" "uploaded-content" "$(echo "$OUT" | grep -m1 uploaded-content | tr -d '\r')"

echo "== 6. upload directory recursively =="
rm -rf /tmp/bs2_tree; mkdir -p /tmp/bs2_tree/sub
echo one > /tmp/bs2_tree/a.txt; echo two > /tmp/bs2_tree/sub/b.txt
"$BIN" -H $H -u "$U" -p "$P" --upload /tmp/bs2_tree:/tmp/bs2_dir/tree >/dev/null 2>&1
ck "dir upload rc" "0" "$?"
OUT=$("$BIN" -H $H -u "$U" -p "$P" -c 'cat /tmp/bs2_dir/tree/sub/b.txt' 2>/dev/null)
ck "nested file content" "two" "$(echo "$OUT" | grep -m1 two | tr -d '\r')"

echo "== 7. download with per-host subdirectory =="
rm -rf /tmp/bs2_dl
"$BIN" -H localhost,127.0.0.1 -u "$U" -p "$P" --download /etc/hostname:/tmp/bs2_dl >/dev/null 2>&1
ck "download rc" "0" "$?"
ck "per-host dirs" "2" "$(ls /tmp/bs2_dl | wc -l | tr -d ' ')"
ck "downloaded content" "$(hostname)" "$(cat /tmp/bs2_dl/127.0.0.1/hostname | tr -d '\r')"

echo "== 8. timeout detection =="
"$BIN" -H $H -u "$U" -p "$P" -T 2 -c 'sleep 30' >/dev/null 2>&1
ck "timeout exit code" "2" "$?"

echo "== 9. wrong password =="
"$BIN" -H $H -u "$U" -p definitely-wrong -c true >/dev/null 2>&1
ck "auth failure exit code" "2" "$?"

echo "== 10. mixed success/failure =="
"$BIN" -H $H,no-such-host.invalid -u "$U" -p "$P" -T 5 -c true >/dev/null 2>&1
ck "partial failure exit code" "1" "$?"

echo "== 11. password from environment =="
OUT=$(BATCHSSH2_PWD="$P" "$BIN" -H $H -u "$U" -c 'echo env-ok' 2>/dev/null)
ck "env password" "env-ok" "$(echo "$OUT" | grep -m1 env-ok | tr -d '\r')"

echo "== 12. password via @file =="
echo "$P" > /tmp/bs2_pwd
OUT=$("$BIN" -H $H -u "$U" -p @/tmp/bs2_pwd -c 'echo file-ok' 2>/dev/null)
ck "file password" "file-ok" "$(echo "$OUT" | grep -m1 file-ok | tr -d '\r')"

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
