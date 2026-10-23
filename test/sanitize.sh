#!/bin/sh
#  Build and run the unit tests under ASan, LeakSanitizer and UBSan, with
#  the system gcc. Run `zig build` first (for libmesa-urcrypt.a and the
#  sqlite source).
#
#    test/sanitize.sh [outdir]
set -e
cd "$(dirname "$0")/.."
OUT=${1:-zig-out/sanitize}
mkdir -p "$OUT"
U=$(dirname "$(find ~/.cache/zig/p -maxdepth 3 -type d -name urcrypt -path '*/urcrypt' | head -1)")
Q=$(dirname "$(find ~/.cache/zig/p -maxdepth 2 -name sqlite3.c | head -1)")
CC="gcc -std=gnu2x -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
    -fms-extensions -DU3_OS_ENDIAN_little=1 -DU3_OS_PROF=1 -DU3_OS_linux=1
    -Wno-builtin-declaration-mismatch -Wno-format-zero-length -Iinclude -Isrc -Isrc/wire -Isrc/eng
    -Ivendor -Ivendor/murmur3 -Ivendor/verstable
    -I$U/urcrypt -I$U/blake3 -I$U/ed25519/src -I$Q"
CORE="vendor/c3/defs.c vendor/murmur3/murmur3.c src/util.c src/ship.c
      src/noun.c src/crypto.c src/sha.c src/ident.c src/json.c src/azimuth.c
      src/knot.c src/aes_siv.c src/gall.c src/store_mem.c src/store_sqlite.c
      src/wire/bitset.c src/wire/lss.c src/wire/pact.c src/eng/*.c"
[ -f "$OUT/sqlite3.o" ] || gcc -O1 -c "$Q/sqlite3.c" -o "$OUT/sqlite3.o"
for t in engine store gall noun ident azimuth crypto aes_siv; do
  $CC $CORE test/${t}_test.c "$OUT/sqlite3.o" zig-out/lib/libmesa-urcrypt.a \
      -lpthread -lm -o "$OUT/$t-test"
  echo "== $t"
  #  not piped: a failing test (or a sanitizer report) fails the script
  if ! "$OUT/$t-test" > "$OUT/$t.log" 2>&1; then
    tail -40 "$OUT/$t.log"
    exit 1
  fi
  tail -2 "$OUT/$t.log"
done
