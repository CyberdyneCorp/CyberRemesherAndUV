#!/usr/bin/env bash
# UNVERIFIED: requires a macOS runner with Xcode, an iOS provisioning profile
# and an Apple distribution certificate. Cannot run in headless Linux CI;
# exercised by release.yml's ios lane. Produces a signed .ipa for the
# TestFlight/App Store lanes (build-and-packaging spec: "iPadOS/iOS archive").
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=../version.sh
source "${REPO_ROOT}/packaging/version.sh"

DIST_DIR="${REPO_ROOT}/dist"
BUILD_DIR="${REPO_ROOT}/build/ios"
ARCHIVE_PATH="${BUILD_DIR}/CyberRemesher.xcarchive"
IPA_NAME="$(cyber_artifact_name ios ipa)"

# Placeholder signing configuration (GitHub secrets in the workflow).
TEAM_ID="${IOS_TEAM_ID:-ABCDE12345}"
EXPORT_PLIST="${SCRIPT_DIR}/ExportOptions.plist"

echo "==> Generating iOS Xcode project for CyberRemesher ${CYBER_VERSION}"
cmake --preset ios \
    -G Xcode \
    -DCYBER_MARKETING_VERSION="${CYBER_VERSION}" \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${TEAM_ID}"

echo "==> Archiving"
xcodebuild -project "${BUILD_DIR}/CyberRemesher.xcodeproj" \
    -scheme CyberRemesher \
    -configuration Release \
    -destination "generic/platform=iOS" \
    -archivePath "${ARCHIVE_PATH}" \
    archive

echo "==> Exporting IPA"
mkdir -p "${DIST_DIR}"
xcodebuild -exportArchive \
    -archivePath "${ARCHIVE_PATH}" \
    -exportOptionsPlist "${EXPORT_PLIST}" \
    -exportPath "${DIST_DIR}/ios"

mv "${DIST_DIR}/ios/CyberRemesher.ipa" "${DIST_DIR}/${IPA_NAME}"
echo "${DIST_DIR}/${IPA_NAME}"
