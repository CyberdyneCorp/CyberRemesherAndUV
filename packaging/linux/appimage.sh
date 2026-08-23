#!/usr/bin/env bash
# UNVERIFIED: requires appimagetool and an FUSE-capable runner to assemble the
# AppImage. Not run in the core headless CI build job (which only
# configures/builds/tests); exercised by release.yml's linux lane. The
# underlying cmake build IS the same cpu-headless config used in CI, and the
# dependency bundling below runs anywhere ldd does.
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

# Libraries an AppImage must inherit from the host rather than carry: the
# dynamic loader and the glibc/GCC core, which cannot be safely mixed with the
# host's, plus libz which is on every desktop. This is the AppImage project's
# excludelist trimmed to what this binary actually pulls in. Everything else the
# binary needs is copied in beside it — the QuadCover solver links OpenMP, TBB
# and zlib, so a stock desktop with no libtbb.so.12 could not start the CLI at
# all, which is the same class of defect as the 0.2.3 missing-Windows-DLL bug.
BUNDLE_EXCLUDE_RE='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libnsl|libutil|libgcc_s|libz)\.so'

# Copies every non-excluded shared library $1 resolves into $2.
bundle_runtime_libs() {
    local binary="$1" libdir="$2" lib base
    mkdir -p "${libdir}"
    while read -r lib; do
        base="$(basename "${lib}")"
        if [[ "${base}" =~ ${BUNDLE_EXCLUDE_RE} ]]; then
            continue
        fi
        cp -L "${lib}" "${libdir}/${base}"
        echo "   bundled ${base}  (from ${lib})"
    done < <(ldd "${binary}" | awk '{ for (i = 1; i <= NF; i++) if (substr($i, 1, 1) == "/") { print $i; break } }' | sort -u)
}

# Fails the package when a non-excluded dependency was neither bundled nor is a
# host library we deliberately inherit — i.e. when the image would die at
# startup with "error while loading shared libraries" on a clean desktop.
assert_dependencies_bundled() {
    local binary="$1" libdir="$2" missing=() base
    while read -r base; do
        if [[ "${base}" =~ ${BUNDLE_EXCLUDE_RE} ]]; then
            continue
        fi
        [[ -f "${libdir}/${base}" ]] || missing+=("${base}")
    done < <(ldd "${binary}" | awk '{ print $1 }' | grep -E '^lib.*\.so' | sort -u)
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "PACKAGE FAIL: ${binary} needs ${missing[*]} but the AppDir bundles none of them" >&2
        exit 1
    fi
}

echo "==> Building CyberRemesher ${CYBER_VERSION} (Linux)"
# CYBER_REQUIRE_QUADCOVER: the shipped Linux image is the artifact the README's
# benchmark numbers describe, so it must carry the vendored Geogram field. Without
# it the build would quietly fall back to the native seamless-UV solver whenever
# the runner image dropped OpenMP or TBB, and nothing downstream would say so.
# Install libtbb-dev + an OpenMP-capable compiler before running this.
cmake --preset cpu-headless -DCYBER_BUILD_APPS=ON -DCYBER_REQUIRE_QUADCOVER=ON
cmake --build --preset cpu-headless

echo "==> Assembling AppDir"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/share/applications" \
         "${APPDIR}/usr/share/icons/hicolor/256x256/apps"

cp "${REPO_ROOT}/build/cpu-headless/apps/cli/cyberremesh" "${APPDIR}/usr/bin/"
cp "${REPO_ROOT}/build/cpu-headless/apps/desktop/CyberRemesher" \
   "${APPDIR}/usr/bin/" 2>/dev/null || echo "   (desktop shell not built; CLI-only AppImage)"

echo "==> Bundling runtime libraries"
for exe in "${APPDIR}/usr/bin/"*; do
    bundle_runtime_libs "${exe}" "${APPDIR}/usr/lib"
done
for exe in "${APPDIR}/usr/bin/"*; do
    assert_dependencies_bundled "${exe}" "${APPDIR}/usr/lib"
done

# Licence notices and the field-solver identity travel with the artifact: the
# MIT/BSD/MPL licences of the statically linked Geogram/AutoRemesher/Eigen tree
# require the notice in binary redistributions, and without the solver record
# nobody can tell which quadrangulator a downloaded image contains.
cp "${REPO_ROOT}/LICENSE" "${REPO_ROOT}/THIRD_PARTY_NOTICES.md" "${APPDIR}/"
{
    echo "cyberremesh ${CYBER_VERSION}"
    echo "field_solver=$(cat "${REPO_ROOT}/build/cpu-headless/cyber_field_solver.txt" \
        2>/dev/null || echo native-seamless-uv)"
    echo "bundled_libs=$(ls "${APPDIR}/usr/lib" 2>/dev/null | tr '\n' ' ')"
} >"${APPDIR}/BUILD_INFO.txt"
cat "${APPDIR}/BUILD_INFO.txt"

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
# The bundled libraries (libtbb, libgomp, libstdc++, …) live under usr/lib.
# Without this the image runs only where those libraries are already installed —
# the opposite of what a portable AppImage promises.
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
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
