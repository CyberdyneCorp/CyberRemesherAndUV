#!/usr/bin/env bash
# Builds a Qt-free headless CLI around AutoRemesher's QuadCover core (isotropic remesh
# -> seamless-UV parameterize -> isoline quad extract), as a SECOND reference for
# examples/11_benchmark.py alongside QuadriFlow. AutoRemesher (github.com/huxingyi/
# autoremesher) is MIT; its GUI is Qt but the parameterize+extract core is Qt-free, so
# we compile only src/AutoRemesher/*, thirdparty/isotropicremesher/* and the curated
# Geogram subset the project's .pro lists (incl. exploragram quad_cover). Links system
# TBB + zlib. Prints the binary path on stdout; caches the build. Not committed.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/autoremesher-src"
BUILD="$SRC/headless-build"
BIN="$BUILD/autoremesher_cli"
HARNESS="$DIR/autoremesher_harness.cpp"

if [ -x "$BIN" ] && [ "$BIN" -nt "$HARNESS" ]; then echo "$BIN"; exit 0; fi

if [ ! -d "$SRC/src" ]; then
  git clone --depth 1 https://github.com/huxingyi/autoremesher.git "$SRC" >&2
fi

mkdir -p "$BUILD"
cd "$SRC"

INCS=(-I"$DIR/qt-shim" -Iinclude -Ithirdparty/eigen -Ithirdparty/isotropicremesher
      -Ithirdparty/geogram/geogram-1.8.3/src/lib
      -Ithirdparty/geogram/geogram-1.8.3/src/lib/geogram/third_party/libMeshb/sources
      -Ithirdparty/geogram)
DEFS=(-DNDEBUG -D_USE_MATH_DEFINES -DNOMINMAX -DGEO_STATIC_LIBS)
CXXFLAGS=(-std=c++14 -O2 -w -fopenmp -pthread "${DEFS[@]}" "${INCS[@]}")
CFLAGS=(-O2 -w -fopenmp -pthread "${DEFS[@]}" "${INCS[@]}")

# Core (Qt-free) translation units the .pro compiles.
mapfile -t SRCS < <(grep -E '^SOURCES \+=' autoremesher.pro | awk '{print $3}' \
  | grep -E '^(thirdparty/geogram|src/AutoRemesher|thirdparty/isotropicremesher)/')

: > "$BUILD/errors.log"
OBJS=()
compile() {  # $1 = source path
  local s="$1"
  local o="$BUILD/$(echo "$s" | tr '/.' '__').o"
  if [ -f "$o" ] && [ "$o" -nt "$s" ]; then return 0; fi
  if [[ "$s" == *.c ]]; then
    gcc "${CFLAGS[@]}" -c "$s" -o "$o" 2>>"$BUILD/errors.log"
  else
    g++ "${CXXFLAGS[@]}" -c "$s" -o "$o" 2>>"$BUILD/errors.log"
  fi
}
export -f compile
export BUILD
# shellcheck disable=SC2068
printf '%s\n' "${SRCS[@]}" "$HARNESS" \
  | CXXFLAGS="${CXXFLAGS[*]}" CFLAGS="${CFLAGS[*]}" \
    xargs -P "$(nproc)" -I{} bash -c '
      s="{}"; o="'"$BUILD"'/$(echo "$s" | tr "/." "__").o"
      if [ -f "$o" ] && [ "$o" -nt "$s" ]; then exit 0; fi
      if [[ "$s" == *.c ]]; then
        gcc $CFLAGS -c "$s" -o "$o" 2>>"'"$BUILD"'/errors.log" || echo "CFAIL $s" >>"'"$BUILD"'/errors.log"
      else
        g++ $CXXFLAGS -c "$s" -o "$o" 2>>"'"$BUILD"'/errors.log" || echo "CXXFAIL $s" >>"'"$BUILD"'/errors.log"
      fi'

if grep -qE '^(C|CXX)FAIL ' "$BUILD/errors.log"; then
  echo "compile failures:" >&2
  grep -E '^(C|CXX)FAIL ' "$BUILD/errors.log" >&2
  tail -30 "$BUILD/errors.log" >&2
  exit 1
fi

# Link everything.
for s in "${SRCS[@]}" "$HARNESS"; do
  OBJS+=("$BUILD/$(echo "$s" | tr '/.' '__').o")
done
g++ -O2 -fopenmp -pthread "${OBJS[@]}" -o "$BIN" -ltbb -lz -ldl -lm 2>>"$BUILD/errors.log" || {
  echo "link failed:" >&2; tail -30 "$BUILD/errors.log" >&2; exit 1; }

echo "$BIN"
