#!/bin/sh
#  test/install/check.sh PREFIX: build and run programs against an
#  installed libmesa with the system C compiler and pkg-config, the way
#  an application outside this repo would. Statically and, for the core,
#  against libmesa.so too.
set -eu

PREFIX=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-cc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --exists libmesa
cd "$OUT"   #  nothing from the source tree on the include path

#  static: libmesa-static and libmesa-host name the archives, so the
#  .so next to them must not be picked up
PC="$PREFIX/lib/pkgconfig"
static() {   #  linked against no libmesa shared library (ldd: Linux only)
  if ldd "$1" 2>/dev/null | grep -q libmesa; then
    echo "install check: $1 links libmesa.so" >&2
    exit 1
  fi
}
$CC -o core "$HERE/core.c" $(PKG_CONFIG_PATH="$PC" pkg-config --cflags --libs libmesa-static)
./core
static ./core

if [ -f "$PREFIX/lib/libmesa-host.a" ]; then
  $CC -o host "$HERE/host.c" $(PKG_CONFIG_PATH="$PC" pkg-config --cflags --libs libmesa-host)
  ./host
  static ./host
fi

#  shared
if [ -f "$PREFIX/lib/libmesa.so" ] || [ -f "$PREFIX/lib/libmesa.dylib" ]; then
  $CC -o core-so "$HERE/core.c" $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs libmesa) -Wl,-rpath,"$PREFIX/lib"
  ./core-so
fi

echo "install check: ok"
