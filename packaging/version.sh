#!/usr/bin/env bash
# UNVERIFIED: release packaging helper. This script cannot be exercised in the
# headless Linux CI used for the core build; it is driven by the release/
# package-smoke GitHub Actions lanes on macOS, Windows, iOS and Android
# runners. Logic is kept realistic and idempotent.
#
# Single source of truth for the CyberRemesher release version.
#
# The canonical value is the CMake `project(... VERSION x.y.z ...)` field in the
# repository-root CMakeLists.txt. That same value is compiled into every binary
# as CYBER_VERSION_STRING (see src/core/CMakeLists.txt) and surfaced by
# `cyberremesh --version` and the About panel. Artifact filenames and the
# release tag are derived here so none of them can diverge (build-and-packaging
# spec: "Single version identity").
#
# Usage:
#   packaging/version.sh                      # print the bare version, e.g. 0.1.0
#   packaging/version.sh --name macos dmg     # print cyberremesh-0.1.0-macos.dmg
#   packaging/version.sh --verify-tag v0.1.0  # assert a tag matches the source
#   source packaging/version.sh               # exposes cyber_* functions + $CYBER_VERSION
set -euo pipefail

CYBER_PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
CYBER_REPO_ROOT="$(cd "${CYBER_PKG_DIR}/.." && pwd)"

# Extract the semantic version from the project() call in the root CMakeLists.
cyber_project_version() {
    local file="${CYBER_REPO_ROOT}/CMakeLists.txt"
    local version
    version="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${file}" | head -n1)"
    if [[ -z "${version}" ]]; then
        echo "version.sh: could not read project VERSION from ${file}" >&2
        return 1
    fi
    printf '%s' "${version}"
}

# The version used for this build. A CYBER_VERSION_OVERRIDE (e.g. a nightly
# stamp) wins, otherwise the CMake source of truth is used.
cyber_version() {
    if [[ -n "${CYBER_VERSION_OVERRIDE:-}" ]]; then
        printf '%s' "${CYBER_VERSION_OVERRIDE}"
    else
        cyber_project_version
    fi
}

# Assert that a pushed tag (vX.Y.Z) matches the compiled-in version so a release
# can never ship binaries whose --version disagrees with the tag/filenames.
cyber_verify_tag() {
    local tag="${1:?expected a tag like v1.2.0}"
    local expected stripped
    expected="$(cyber_project_version)"
    stripped="${tag#v}"
    if [[ "${stripped}" != "${expected}" ]]; then
        echo "version.sh: tag ${tag} does not match project version ${expected}" >&2
        return 1
    fi
    printf '%s' "${expected}"
}

# Versioned artifact name: cyber_artifact_name <platform> <ext> [suffix]
#   cyber_artifact_name macos dmg          -> cyberremesh-0.1.0-macos.dmg
#   cyber_artifact_name windows zip x64    -> cyberremesh-0.1.0-windows-x64.zip
cyber_artifact_name() {
    local platform="${1:?platform required}"
    local ext="${2:?extension required}"
    local suffix="${3:-}"
    local version
    version="$(cyber_version)"
    if [[ -n "${suffix}" ]]; then
        printf 'cyberremesh-%s-%s-%s.%s' "${version}" "${platform}" "${suffix}" "${ext}"
    else
        printf 'cyberremesh-%s-%s.%s' "${version}" "${platform}" "${ext}"
    fi
}

# When sourced (not executed) expose the version eagerly for callers.
if [[ "${BASH_SOURCE[0]:-}" != "${0}" ]]; then
    CYBER_VERSION="$(cyber_version)"
    export CYBER_VERSION
    return 0 2>/dev/null || true
fi

# Direct execution: small CLI over the helpers above.
main() {
    local cmd="${1:-print}"
    case "${cmd}" in
        print|--print|"")
            cyber_version
            echo
            ;;
        --name)
            cyber_artifact_name "${2:?platform}" "${3:?ext}" "${4:-}"
            echo
            ;;
        --verify-tag)
            cyber_verify_tag "${2:?tag}"
            echo
            ;;
        --github-output)
            # Emit key=value pairs for a GitHub Actions step (writes $GITHUB_OUTPUT).
            local version
            version="$(cyber_version)"
            {
                echo "version=${version}"
                echo "tag=v${version}"
            } >>"${GITHUB_OUTPUT:-/dev/stdout}"
            ;;
        *)
            echo "version.sh: unknown command '${cmd}'" >&2
            return 2
            ;;
    esac
}

main "$@"
