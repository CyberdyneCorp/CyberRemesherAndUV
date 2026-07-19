#!/usr/bin/env bash
# Builds QuadriFlow (github.com/hjwdzh/QuadriFlow) — a field-based / integer-grid
# quad remesher in the same lineage as AutoRemesher — as a headless CLI reference
# for examples/10_vs_reference.py. AutoRemesher itself is GUI-only and cannot run
# headless, so QuadriFlow stands in as the runnable state-of-the-art baseline.
#
# Patched to build WITHOUT Boost (uses QuadriFlow's bundled LEMON / hand-rolled
# max-flow instead), so only Eigen + a C++ toolchain are needed. Prints the path
# to the built binary on stdout; caches the build. Not committed — built on demand.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/QuadriFlow"
BIN="$SRC/build/quadriflow"
if [ -x "$BIN" ]; then echo "$BIN"; exit 0; fi

EIGEN=""
for cand in /usr/include/eigen3 /usr/local/include/eigen3 "$HOME"/vcpkg/packages/eigen3_*/include/eigen3; do
  if [ -d "$cand/Eigen" ]; then EIGEN="$cand"; break; fi
done
if [ -z "$EIGEN" ]; then echo "eigen3 not found (apt install libeigen3-dev)" >&2; exit 1; fi

if [ ! -d "$SRC/src" ]; then
  git clone --recursive --depth 1 https://github.com/hjwdzh/QuadriFlow "$SRC" >&2
fi

python3 - "$SRC" <<'PY'
import sys, os
src = sys.argv[1]
f = os.path.join(src, "src/flow.hpp"); s = open(f).read()
if "#ifdef WITH_BOOST" not in s:  # idempotent boost-free patch
    s = s.replace("#include <boost/graph/adjacency_list.hpp>",
                  "#ifdef WITH_BOOST\n#include <boost/graph/adjacency_list.hpp>")
    s = s.replace("#include <boost/graph/push_relabel_max_flow.hpp>",
                  "#include <boost/graph/push_relabel_max_flow.hpp>\n#endif")
    s = s.replace("using namespace boost;", "#ifdef WITH_BOOST\nusing namespace boost;\n#endif")
    i = s.index("class BoykovMaxFlowHelper"); j = s.index("class NetworkSimplexFlowHelper")
    s = s[:i] + "#ifdef WITH_BOOST\n" + s[i:j].rstrip() + "\n#endif\n\n" + s[j:]
    open(f, "w").write(s)
o = os.path.join(src, "src/optimizer.cpp"); t = open(o).read()
open(o, "w").write(t.replace("std::make_unique<BoykovMaxFlowHelper>()",
                             "std::make_unique<ECMaxFlowHelper>()"))
c = os.path.join(src, "CMakeLists.txt"); u = open(c).read()
open(c, "w").write(u.replace("find_package(Boost REQUIRED)", "# find_package(Boost REQUIRED)"))
PY

cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DEIGEN_INCLUDE_DIR="$EIGEN" >&2
cmake --build "$SRC/build" -j4 >&2
echo "$BIN"
