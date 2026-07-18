#!/usr/bin/env bash
# UNVERIFIED: requires a windowing/display server (or headless GL/Metal
# surface) and the packaged desktop app; not runnable in the core headless CI.
# Placeholder for the app-launch screenshot smoke check (build-and-packaging
# spec: "plus an app-launch screenshot").
#
# Launches the packaged desktop app, waits for its window, captures a
# screenshot artifact and shuts it down. Fails if the app never renders.
set -euo pipefail

APP_BIN="${1:?usage: launch_screenshot.sh <path-to-app> <out.png>}"
OUT_PNG="${2:?output png path required}"

echo "==> Launching ${APP_BIN} in screenshot mode"
# --self-test asks the shell to render one frame headlessly and exit; the app
# writes the frame to the requested path (no interactive session needed).
"${APP_BIN}" --self-test --screenshot "${OUT_PNG}" --timeout 30

if [[ ! -s "${OUT_PNG}" ]]; then
    echo "SMOKE FAIL: no screenshot captured at ${OUT_PNG}" >&2
    exit 1
fi
echo "==> Launch screenshot captured: ${OUT_PNG}"
