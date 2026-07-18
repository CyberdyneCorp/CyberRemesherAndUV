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
