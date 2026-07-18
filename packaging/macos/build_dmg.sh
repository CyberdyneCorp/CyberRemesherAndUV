#!/usr/bin/env bash
# UNVERIFIED: requires a real macOS runner with Xcode, codesign, an Apple
# Developer ID identity and notarytool credentials. Cannot run in headless
# Linux CI; exercised by .github/workflows/release.yml on the macos lane.
#
# Builds the CyberRemesher desktop app + CLI, assembles a .app bundle, wraps it
# in a signed/notarized DMG with a versioned filename (build-and-packaging
# spec: "Platform packages"). Signing/notarization use placeholder secrets and
# are skipped when the identity is absent (unsigned local dev DMG).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=../version.sh
source "${REPO_ROOT}/packaging/version.sh"

DIST_DIR="${REPO_ROOT}/dist"
STAGE_DIR="${DIST_DIR}/macos/stage"
APP_NAME="CyberRemesher.app"
DMG_NAME="$(cyber_artifact_name macos dmg)"

# Placeholder secrets (provided by the release workflow env / GitHub secrets).
CODESIGN_IDENTITY="${MACOS_CODESIGN_IDENTITY:-}"        # "Developer ID Application: ..."
NOTARY_PROFILE="${MACOS_NOTARY_KEYCHAIN_PROFILE:-}"     # notarytool stored profile

echo "==> Building CyberRemesher ${CYBER_VERSION} (macOS, Metal)"
cmake --preset macos-metal
cmake --build --preset macos-metal --config Release

echo "==> Staging ${APP_NAME}"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/${APP_NAME}/Contents/MacOS"
mkdir -p "${STAGE_DIR}/${APP_NAME}/Contents/Resources"

# Bundle the desktop shell and the headless CLI so smoke tests can drive it.
cp "${REPO_ROOT}/build/macos-metal/apps/desktop/CyberRemesher" \
   "${STAGE_DIR}/${APP_NAME}/Contents/MacOS/CyberRemesher"
cp "${REPO_ROOT}/build/macos-metal/apps/cli/cyberremesh" \
   "${STAGE_DIR}/${APP_NAME}/Contents/MacOS/cyberremesh"
cp "${SCRIPT_DIR}/Info.plist.in" \
   "${STAGE_DIR}/${APP_NAME}/Contents/Info.plist"
sed -i '' "s/@CYBER_VERSION@/${CYBER_VERSION}/g" \
   "${STAGE_DIR}/${APP_NAME}/Contents/Info.plist"

if [[ -n "${CODESIGN_IDENTITY}" ]]; then
    echo "==> Codesigning (hardened runtime)"
    codesign --force --deep --options runtime --timestamp \
        --sign "${CODESIGN_IDENTITY}" \
        "${STAGE_DIR}/${APP_NAME}"
else
    echo "==> No MACOS_CODESIGN_IDENTITY set; producing unsigned dev DMG"
fi

echo "==> Building DMG ${DMG_NAME}"
mkdir -p "${DIST_DIR}"
rm -f "${DIST_DIR}/${DMG_NAME}"
hdiutil create -volname "CyberRemesher ${CYBER_VERSION}" \
    -srcfolder "${STAGE_DIR}" -ov -format UDZO \
    "${DIST_DIR}/${DMG_NAME}"

if [[ -n "${CODESIGN_IDENTITY}" && -n "${NOTARY_PROFILE}" ]]; then
    "${SCRIPT_DIR}/notarize.sh" "${DIST_DIR}/${DMG_NAME}"
else
    echo "==> Skipping notarization (identity or notary profile missing)"
fi

echo "${DIST_DIR}/${DMG_NAME}"
