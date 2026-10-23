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

#  static: the archives, not the .so
cp -R "$PREFIX" static
rm -f static/lib/libmesa.so static/lib/libmesa.dylib
PC="$OUT/static/lib/pkgconfig"
$CC -o core "$HERE/core.c" $(PKG_CONFIG_PATH="$PC" pkg-config --cflags --libs --static libmesa)
./core

if [ -f "$PREFIX/lib/libmesa-host.a" ]; then
  $CC -o host "$HERE/host.c" $(PKG_CONFIG_PATH="$PC" pkg-config --cflags --libs --static libmesa-host)
  ./host
fi

#  shared
if [ -f "$PREFIX/lib/libmesa.so" ] || [ -f "$PREFIX/lib/libmesa.dylib" ]; then
  $CC -o core-so "$HERE/core.c" $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags --libs libmesa) -Wl,-rpath,"$PREFIX/lib"
  ./core-so
fi

echo "install check: ok"
