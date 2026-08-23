#!/usr/bin/env bash
# UNVERIFIED: drives an already-packaged CLI binary; the package it consumes is
# built on platform runners, not in the headless core CI. The logic is real and
# self-contained (it shells out to whatever cyberremesh path it is given) so it
# can be pointed at a mounted DMG / extracted zip / AppImage.
#
# Packaged-form CLI smoke test (build-and-packaging spec: "Package smoke
# tests"): remesh a reference model and assert a valid, non-empty output plus
# exit code 0. Exits nonzero to block publication on failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CLI_BIN="${1:?usage: cli_remesh_smoke.sh <path-to-cyberremesh> [sample.obj]}"
SAMPLE="${2:-${SCRIPT_DIR}/assets/reference_cube.obj}"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT
OUT="${WORKDIR}/out.obj"
REPORT="${WORKDIR}/report.json"

# A packaged binary must resolve every shared library it needs from what the
# package carries plus the host's core runtime. The 0.2.3 Windows zip shipped
# without the MinGW DLLs and the Linux AppImage used to bundle no libraries at
# all, so both died at startup on a clean machine while every CI check stayed
# green — the runner that built them happened to have the libraries.
if command -v ldd >/dev/null 2>&1; then
    echo "==> Checking shared-library resolution"
    if ldd "${CLI_BIN}" 2>/dev/null | grep -q 'not found'; then
        echo "SMOKE FAIL: ${CLI_BIN} has unresolved shared libraries:" >&2
        ldd "${CLI_BIN}" | grep 'not found' >&2
        exit 1
    fi
fi

echo "==> cyberremesh --version"
"${CLI_BIN}" --version

echo "==> Remeshing ${SAMPLE}"
"${CLI_BIN}" \
    --input "${SAMPLE}" \
    --output "${OUT}" \
    --report "${REPORT}" \
    --target-quads 2000

# Assert the packaged run produced a real result.
if [[ ! -s "${OUT}" ]]; then
    echo "SMOKE FAIL: output mesh ${OUT} is missing or empty" >&2
    exit 1
fi
if ! grep -q '^f ' "${OUT}"; then
    echo "SMOKE FAIL: output mesh ${OUT} contains no faces" >&2
    exit 1
fi
if [[ ! -s "${REPORT}" ]]; then
    echo "SMOKE FAIL: run report ${REPORT} is missing or empty" >&2
    exit 1
fi

echo "==> Smoke OK: $(grep -c '^f ' "${OUT}") faces written"
