#!/bin/sh
# Verify the keyboard-interactive auth fallback: temp sshd on :2222 with
# password auth disabled. Usage: sh tests/kbd_test.sh <binary> [sudo_pw]
BIN=${1:?binary}
PW=${2:-ubuntu}

echo "$PW" | sudo -S /usr/sbin/sshd -p 2222 -o PidFile=/tmp/bs2_sshd.pid \
    -o PasswordAuthentication=no -o KbdInteractiveAuthentication=yes \
    -o UsePAM=yes 2>&1 | grep -v '^\[sudo\]'
sleep 1

"$BIN" -H localhost --port 2222 -u ubuntu -p ubuntu -c 'echo KBD-INTERACTIVE-OK'
RC=$?

if [ -f /tmp/bs2_sshd.pid ]; then
    echo "$PW" | sudo -S kill "$(cat /tmp/bs2_sshd.pid)" 2>/dev/null
    rm -f /tmp/bs2_sshd.pid
fi
echo "EXIT=$RC"
[ "$RC" = "0" ]
