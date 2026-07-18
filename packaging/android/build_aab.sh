#!/usr/bin/env bash
# UNVERIFIED: requires the Android SDK/NDK, Gradle and a release keystore.
# Cannot run in headless Linux CI; exercised by release.yml's android lane.
# Produces a signed Android App Bundle (build-and-packaging spec: "Android
# (APK/AAB)").
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=../version.sh
source "${REPO_ROOT}/packaging/version.sh"

DIST_DIR="${REPO_ROOT}/dist"
GRADLE_DIR="${REPO_ROOT}/apps/mobile/android"
AAB_NAME="$(cyber_artifact_name android aab)"

# versionName is the single-source semver; versionCode is a monotonic integer
# derived from it (major*10000 + minor*100 + patch) so store uploads increase.
IFS='.' read -r MAJOR MINOR PATCH <<<"${CYBER_VERSION}"
VERSION_CODE=$(( MAJOR * 10000 + MINOR * 100 + PATCH ))

echo "==> Building CyberRemesher ${CYBER_VERSION} AAB (versionCode ${VERSION_CODE})"

# The NDK build of the engine (Vulkan render + OpenCL) is driven by CMake via
# Gradle's externalNativeBuild using the android preset.
( cd "${GRADLE_DIR}" && ./gradlew bundleRelease \
    -PcyberVersionName="${CYBER_VERSION}" \
    -PcyberVersionCode="${VERSION_CODE}" )

SRC_AAB="${GRADLE_DIR}/app/build/outputs/bundle/release/app-release.aab"

# Sign with the release keystore (placeholder secrets from the workflow).
if [[ -n "${ANDROID_KEYSTORE_PATH:-}" ]]; then
    echo "==> Signing bundle with jarsigner"
    jarsigner -keystore "${ANDROID_KEYSTORE_PATH}" \
        -storepass "${ANDROID_KEYSTORE_PASSWORD:?}" \
        -keypass "${ANDROID_KEY_PASSWORD:?}" \
        "${SRC_AAB}" "${ANDROID_KEY_ALIAS:?}"
else
    echo "==> No ANDROID_KEYSTORE_PATH set; producing unsigned bundle"
fi

mkdir -p "${DIST_DIR}"
cp "${SRC_AAB}" "${DIST_DIR}/${AAB_NAME}"
echo "${DIST_DIR}/${AAB_NAME}"
