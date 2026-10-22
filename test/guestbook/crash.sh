#!/bin/bash
#  Exactly-once under kill -9: 40 ships' worth of signatures to
#  mesa-guestbook (as fake ~nec) while it's killed and restarted every few
#  seconds, with a webhook that fails 30% of the time. Every entry must be
#  recorded once, and reach the webhook.
#
#    test/guestbook/crash.sh [workdir]
#
#  Needs `zig build`, python3, and UDP ports 31338-31339 and TCP 18080 free.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/../../zig-out/bin
W=${1:-$(mktemp -d)}
mkdir -p "$W" && cd "$W" || exit 2
rm -f nec.db* bud.db* hooks.txt gb.log
python3 -I "$HERE/hook.py" 18080 "$W/hooks.txt" & HP=$!
start() {
  "$BIN/mesa-guestbook" -F ~nec -s nec.db -H http://127.0.0.1:18080/hook >> gb.log 2>&1 &
  echo $! > gb.pid
}
start; sleep 0.5
( for k in $(seq 1 25); do
    sleep $(( (RANDOM % 3) + 1 ))
    kill -9 "$(cat gb.pid)" 2>/dev/null; wait "$(cat gb.pid)" 2>/dev/null
    sleep 0.$(( RANDOM % 9 )); start
  done ) &
KP=$!
ok=0
for i in $(seq 1 40); do
  timeout 300 "$BIN/mesa-ping" -F ~bud -s bud.db ~nec guestbook "entry $i" >/dev/null 2>&1 && ok=$((ok+1))
done
wait $KP
for t in $(seq 1 60); do
  [ "$(python3 -I -c "import json;print(len({json.loads(l)['id'] for l in open('hooks.txt')}))" 2>/dev/null)" = 40 ] && break
  sleep 1
done
kill "$(cat gb.pid)" $HP 2>/dev/null
rows=$(python3 -I -c "import sqlite3;print(*sqlite3.connect('nec.db').execute('select count(*),count(distinct text) from gb_entry').fetchone())")
ids=$(python3 -I -c "import json;print(len({json.loads(l)['id'] for l in open('hooks.txt')}))")
echo "acked: $ok/40; rows (total distinct): $rows; webhook ids: $ids; restarts: $(grep -c 'guestbook of' gb.log)"
[ "$ok" = 40 ] && [ "$rows" = "40 40" ] && [ "$ids" = 40 ]
