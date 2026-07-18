#!/usr/bin/env bash
# Runs a CyberRemesher Python example (or run_all.py) with the environment set
# up: locates the built C-ABI shared library, exports CYBER_CAPI_LIB, puts the
# cyberremesh package on PYTHONPATH, and — if the interpreter's libstdc++ is too
# old to load the GCC-built library (common with Anaconda) — preloads the system
# libstdc++.
#
#   examples/run.sh examples/01_quad_remesh.py
#   examples/run.sh examples/run_all.py
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

LIB="$(find "$ROOT/build" -name 'libcyber_capi.so' 2>/dev/null | head -1)"
if [ -z "$LIB" ]; then
  echo "C-ABI shared library not found. Build it first:" >&2
  echo "  cmake --preset cpu-headless && cmake --build --preset cpu-headless --target cyber_capi_shared" >&2
  exit 1
fi
export CYBER_CAPI_LIB="$LIB"
export PYTHONPATH="$ROOT/python/cyberremesh:${PYTHONPATH:-}"

# If the plain load fails (e.g. Anaconda ships an older libstdc++), preload the
# system one so the GCC-built library resolves its GLIBCXX symbols.
if ! python3 - "$LIB" <<'PY' >/dev/null 2>&1
import ctypes, sys
ctypes.CDLL(sys.argv[1])
PY
then
  for cand in /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /usr/lib64/libstdc++.so.6; do
    if [ -e "$cand" ]; then
      export LD_PRELOAD="$cand${LD_PRELOAD:+:$LD_PRELOAD}"
      break
    fi
  done
fi

exec python3 "$@"
