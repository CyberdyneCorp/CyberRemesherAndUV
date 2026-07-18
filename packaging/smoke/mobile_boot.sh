#!/usr/bin/env bash
# UNVERIFIED: requires an iOS Simulator (macOS/Xcode) or Android emulator
# (KVM-capable runner). Placeholder mobile boot smoke check (build-and-packaging
# spec: "Mobile artifacts SHALL at minimum boot in a simulator/emulator").
#
# Installs the built mobile artifact into a simulator/emulator, boots it,
# launches the app and asserts it reaches a running foreground state.
set -euo pipefail

PLATFORM="${1:?usage: mobile_boot.sh <ios|android> <artifact>}"
ARTIFACT="${2:?artifact path required}"
BUNDLE_ID="ai.cyberremesher.mobile"

case "${PLATFORM}" in
    ios)
        DEVICE="${IOS_SIM_DEVICE:-iPhone 15}"
        echo "==> Booting iOS simulator '${DEVICE}'"
        xcrun simctl boot "${DEVICE}" || true
        xcrun simctl install "${DEVICE}" "${ARTIFACT}"
        xcrun simctl launch "${DEVICE}" "${BUNDLE_ID}"
        # Assert the process is alive.
        xcrun simctl spawn "${DEVICE}" launchctl list | grep -q "${BUNDLE_ID}" \
            || { echo "SMOKE FAIL: app not running in simulator" >&2; exit 1; }
        ;;
    android)
        echo "==> Booting Android emulator"
        adb wait-for-device
        adb install -r "${ARTIFACT}"
        adb shell monkey -p "${BUNDLE_ID}" -c android.intent.category.LAUNCHER 1
        adb shell pidof "${BUNDLE_ID}" >/dev/null \
            || { echo "SMOKE FAIL: app not running in emulator" >&2; exit 1; }
        ;;
    *)
        echo "mobile_boot.sh: unknown platform '${PLATFORM}'" >&2
        exit 2
        ;;
esac

echo "==> Mobile boot OK (${PLATFORM})"
