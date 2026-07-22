# CyberRemesher — quad remeshing / retopology / UV / bake engine.
# Task runner mirroring the CyberdyneCorp library convention (SciPP / NumPP):
# `just <recipe>`. Everything is a thin wrapper over the CMake presets.

# CPU-only headless preset. Builds the in-process Geogram QuadCover solver
# (-DCYBER_WITH_QUADCOVER=ON) so the default quad-cover extractor beats
# QuadriFlow. Override with `just preset=cpu-headless-debug <recipe>` etc.
preset := "cpu-headless"

# List recipes.
default:
    @just --list

# Configure + build the library, CLI, and tests.
build:
    cmake --preset {{preset}}
    cmake --build --preset {{preset}}

# Build, then run the full test suite.
test: build
    ctest --preset {{preset}} --output-on-failure

# Run the test suite only (assumes an existing build).
ctest:
    ctest --preset {{preset}} --output-on-failure

# Debug build with ASan/UBSan.
debug:
    cmake --preset cpu-headless-debug
    cmake --build --preset cpu-headless-debug

# ASan/UBSan build + tests.
asan: debug
    ctest --preset cpu-headless-debug --output-on-failure

# Build + test with GCC instead of the default compiler.
gcc:
    cmake -S . -B build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
    cmake --build build/gcc
    ctest --test-dir build/gcc --output-on-failure

# Validate the OpenSpec change proposals + specs.
spec:
    openspec list
    openspec validate --all --strict

# Local CI: default build+test, a GCC build, and spec validation.
ci: test gcc spec

# Probe the available GPU compute backends.
gpu-detect:
    @echo "CUDA:   $(command -v nvcc >/dev/null 2>&1 && nvcc --version | tail -1 || echo 'not found')"
    @echo "OpenCL: $(ls /etc/OpenCL/vendors/*.icd 2>/dev/null | wc -l | tr -d ' ') ICD(s)"
    @echo "Metal:  $([ "$(uname)" = Darwin ] && echo 'available (Apple)' || echo 'n/a (non-Apple)')"

# Install the find_package(CyberRemesher) CONFIG package to PREFIX (default ./dist).
# Downstream: find_package(CyberRemesher CONFIG) then link cyber::capi + <cyber_capi.h>.
install prefix="./dist":
    cmake --preset {{preset}} -DCMAKE_INSTALL_PREFIX={{prefix}}
    cmake --build --preset {{preset}} --target cyber_capi_shared
    cmake --install build/{{preset}}
    @echo "Installed to {{prefix}} -> find_package(CyberRemesher CONFIG) -> cyber::capi"

# First-run setup: build the engine, then install the package.
bootstrap: build install

# Run every Python example and stitch the gallery.
examples:
    examples/run.sh examples/run_all.py

# The scored retopology benchmark vs QuadriFlow / AutoRemesher.
bench:
    examples/run.sh examples/11_benchmark.py

# Remove all build/output directories.
clean:
    rm -rf build build-* dist
