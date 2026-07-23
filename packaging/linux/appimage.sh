#!/usr/bin/env bash
# UNVERIFIED: requires linuxdeploy + appimagetool and an FUSE-capable runner to
# assemble/relocate a portable AppImage. Not run in the core headless CI build
# job (which only configures/builds/tests); exercised by release.yml's linux
# lane. The underlying cmake build IS the same cpu-headless config used in CI.
#
# Produces a versioned Linux AppImage bundling the CLI and desktop shell
# (build-and-packaging spec: "Linux (AppImage)").
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=../version.sh
source "${REPO_ROOT}/packaging/version.sh"

DIST_DIR="${REPO_ROOT}/dist"
APPDIR="${DIST_DIR}/linux/CyberRemesher.AppDir"
IMAGE_NAME="$(cyber_artifact_name linux AppImage x86_64)"

echo "==> Building CyberRemesher ${CYBER_VERSION} (Linux)"
cmake --preset cpu-headless -DCYBER_BUILD_APPS=ON
cmake --build --preset cpu-headless

echo "==> Assembling AppDir"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/share/applications" \
         "${APPDIR}/usr/share/icons/hicolor/256x256/apps"

cp "${REPO_ROOT}/build/cpu-headless/apps/cli/cyberremesh" "${APPDIR}/usr/bin/"
cp "${REPO_ROOT}/build/cpu-headless/apps/desktop/CyberRemesher" \
   "${APPDIR}/usr/bin/" 2>/dev/null || echo "   (desktop shell not built; CLI-only AppImage)"

cp "${SCRIPT_DIR}/cyberremesher.desktop" \
   "${APPDIR}/usr/share/applications/cyberremesher.desktop"
cp "${SCRIPT_DIR}/cyberremesher.desktop" "${APPDIR}/cyberremesher.desktop"
# A real icon ships in assets/; a 1x1 placeholder keeps appimagetool happy.
: >"${APPDIR}/usr/share/icons/hicolor/256x256/apps/cyberremesher.png"
cp "${APPDIR}/usr/share/icons/hicolor/256x256/apps/cyberremesher.png" \
   "${APPDIR}/cyberremesher.png"

cat >"${APPDIR}/AppRun" <<'APPRUN'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "${0}")")"
# The desktop shell is a placeholder today, so fall back to the CLI rather than
# exec'ing a binary the AppDir does not contain.
if [ -x "${HERE}/usr/bin/CyberRemesher" ]; then
  exec "${HERE}/usr/bin/CyberRemesher" "$@"
fi
exec "${HERE}/usr/bin/cyberremesh" "$@"
APPRUN
chmod +x "${APPDIR}/AppRun"

echo "==> Packaging ${IMAGE_NAME}"
mkdir -p "${DIST_DIR}"
ARCH=x86_64 appimagetool "${APPDIR}" "${DIST_DIR}/${IMAGE_NAME}"

echo "${DIST_DIR}/${IMAGE_NAME}"
